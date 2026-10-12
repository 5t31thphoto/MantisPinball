// fwpatch.js - patch the intro splash inside an ESP32-S3 app image, in the
// browser, then fix the image's checksum and SHA-256 so the bootloader accepts
// it. Pure functions; runs unchanged in the page and in Node (for CI tests).
//
// The firmware keeps the splash in one tagged blob (src/intro_bitmaps.h):
//   0 magic[16] "MANTIS_SPLASH_1\0" | 16 u16 version | 18 u16 capacity
//  20 u16 nlines | 22 u16 reserved | 24 line[3]{u16 w,h,stride,off} | 48 data[]
// Rows are `stride` bytes, MSB = leftmost pixel (matches blitBitmap()).

export const SPLASH_MAGIC = new TextEncoder().encode("MANTIS_SPLASH_1\0");
export const SPLASH_HDR = 48;
// drawIntro() places the lines at fixed y = 100 / 138 / 162, so each line has
// a height budget before it collides with the next one.
// Width 158 matches intro_designer.render_text_bitmap(max_w=158), the proven value.
export const LINE_LIMITS = [
  { maxW: 158, maxH: 36 },
  { maxW: 158, maxH: 22 },
  { maxW: 158, maxH: 48 },
];

function findAll(hay, needle) {
  const hits = [];
  outer: for (let i = 0; i <= hay.length - needle.length; i++) {
    for (let j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;
    hits.push(i);
  }
  return hits;
}

// Walk the ESP app image: 24-byte header, N segments of {addr,len,data},
// pad so the checksum byte ends a 16-byte block, optional 32-byte SHA-256.
export function parseAppImage(img) {
  if (img.length < 32 || img[0] !== 0xE9) throw new Error("not an ESP app image (magic != 0xE9)");
  const dv = new DataView(img.buffer, img.byteOffset, img.byteLength);
  const nseg = img[1];
  const hashAppended = img[23] === 1;
  let off = 24;
  const segs = [];
  for (let i = 0; i < nseg; i++) {
    if (off + 8 > img.length) throw new Error("truncated segment header");
    const len = dv.getUint32(off + 4, true);
    segs.push({ hdr: off, data: off + 8, len });
    off += 8 + len;
    if (off > img.length) throw new Error("segment runs past end of image");
  }
  const csumOff = off + (15 - (off % 16));
  const end = csumOff + 1 + (hashAppended ? 32 : 0);
  if (end > img.length) throw new Error("image shorter than its own layout");
  return { segs, csumOff, hashAppended, hashOff: hashAppended ? csumOff + 1 : -1, end };
}

export function readSplash(img) {
  const hits = findAll(img, SPLASH_MAGIC);
  if (hits.length !== 1) throw new Error(`splash tag found ${hits.length} times (need exactly 1)`);
  const m = hits[0];
  const dv = new DataView(img.buffer, img.byteOffset, img.byteLength);
  const cap = dv.getUint16(m + 18, true);
  const lines = [];
  for (let i = 0; i < 3; i++) {
    const b = m + 24 + i * 8;
    lines.push({ w: dv.getUint16(b, true), h: dv.getUint16(b + 2, true),
                 stride: dv.getUint16(b + 4, true), off: dv.getUint16(b + 6, true) });
  }
  return { at: m, cap, lines };
}

// Recompute the XOR checksum (seed 0xEF over every segment's data) and, if
// present, the SHA-256 over everything up to and including the checksum.
export async function fixImage(img) {
  const L = parseAppImage(img);
  let c = 0xEF;
  for (const s of L.segs) for (let i = s.data; i < s.data + s.len; i++) c ^= img[i];
  img[L.csumOff] = c;
  if (L.hashAppended) {
    const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", img.subarray(0, L.csumOff + 1)));
    img.set(digest, L.hashOff);
  }
  return img;
}

// Independent check used before flashing: does the image still verify?
export async function verifyImage(img) {
  const L = parseAppImage(img);
  let c = 0xEF;
  for (const s of L.segs) for (let i = s.data; i < s.data + s.len; i++) c ^= img[i];
  if (img[L.csumOff] !== c) return { ok: false, why: "checksum mismatch" };
  if (L.hashAppended) {
    const d = new Uint8Array(await crypto.subtle.digest("SHA-256", img.subarray(0, L.csumOff + 1)));
    for (let i = 0; i < 32; i++) if (img[L.hashOff + i] !== d[i]) return { ok: false, why: "sha256 mismatch" };
  }
  return { ok: true };
}

// Pack a 1-bit pixel grid (array of rows of booleans) MSB-first.
export function packBits(rows, w) {
  const stride = Math.ceil(w / 8);
  const out = new Uint8Array(stride * rows.length);
  rows.forEach((row, y) => {
    for (let x = 0; x < w; x++) if (row[x]) out[y * stride + (x >> 3)] |= 0x80 >> (x & 7);
  });
  return { w, h: rows.length, stride, bytes: out };
}

// Write three packed lines into a COPY of the app image and fix it up.
// Refuses anything that would not fit or that would land outside the single
// segment holding the blob - a bad patch must never reach the device.
export async function patchSplash(original, packed) {
  const img = new Uint8Array(original);          // never mutate the download
  const L = parseAppImage(img);
  const sp = readSplash(img);
  const seg = L.segs.find(s => sp.at >= s.data && sp.at + SPLASH_HDR + sp.cap <= s.data + s.len);
  if (!seg) throw new Error("splash blob is not wholly inside one segment");
  if (packed.length !== 3) throw new Error("need exactly 3 lines");
  let used = 0;
  packed.forEach((p, i) => {
    const lim = LINE_LIMITS[i];
    if (p.w > lim.maxW || p.h > lim.maxH) throw new Error(`line ${i + 1} is ${p.w}x${p.h}, max ${lim.maxW}x${lim.maxH}`);
    if (p.stride * 8 < p.w || p.bytes.length !== p.stride * p.h) throw new Error(`line ${i + 1} malformed`);
    used += p.bytes.length;
  });
  if (used > sp.cap) throw new Error(`splash needs ${used} B, capacity ${sp.cap}`);
  const dv = new DataView(img.buffer);
  const base = sp.at + SPLASH_HDR;
  img.fill(0, base, base + sp.cap);
  let off = 0;
  packed.forEach((p, i) => {
    const h = sp.at + 24 + i * 8;
    dv.setUint16(h, p.w, true); dv.setUint16(h + 2, p.h, true);
    dv.setUint16(h + 4, p.stride, true); dv.setUint16(h + 6, off, true);
    img.set(p.bytes, base + off);
    off += p.bytes.length;
  });
  await fixImage(img);
  const v = await verifyImage(img);
  if (!v.ok) throw new Error("patched image failed verification: " + v.why);
  return img;
}

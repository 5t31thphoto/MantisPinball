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

// ── colour scheme ─────────────────────────────────────────────────────────────
// The firmware's palette lives in a tagged block (src/theme_runtime.cpp):
//   char magic[16] "MANTIS_THEME_1" | u32 version | u32 explicit | u32 nonce | u16 pal[14]
// deriveShades() is tools/intro_designer.py derive_shades() exactly - including
// Python's round-half-to-even - so the website and the tkinter installer turn
// the same three colours into the same 14 shades.
export const THEME_MAGIC = new TextEncoder().encode("MANTIS_THEME_1\0\0");
export const THEME_SIZE = 56;
export const THEME_ORDER = ["TEAL_DEEP", "TEAL_DARK", "TEAL", "TEAL_BRIGHT", "TEAL_LIGHT",
  "PURPLE_DEEP", "PURPLE_DARK", "PURPLE", "PURPLE_BRIGHT", "PURPLE_LIGHT",
  "LIME_DARK", "LIME_MED", "LIME", "LIME_PALE"];

function pyRound(x) {                       // Python 3 round(): halves go to even
  const f = Math.floor(x), d = x - f;
  if (d > 0.5) return f + 1;
  if (d < 0.5) return f;
  return (f % 2 === 0) ? f : f + 1;
}
const lerpBlack = (rgb, t) => rgb.map(c => pyRound(c * t));
const scaleClamp = (rgb, k) => rgb.map(c => Math.max(0, Math.min(255, pyRound(c * k))));
const lerpWhite = (rgb, t) => rgb.map(c => pyRound(c + (255 - c) * t));
const light = (rgb, s, w) => lerpWhite(scaleClamp(rgb, s), w);
export const hexToRgb = h => [1, 3, 5].map(i => parseInt(h.replace("#", "").padStart(6, "0").slice(i - 1, i + 1), 16));
export const rgbToHex = c => "#" + c.map(v => Math.max(0, Math.min(255, v)).toString(16).padStart(2, "0")).join("");
export function rgb565(r, g, b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
export function rgb565ToHex(v) {                      // what the panel actually shows
  const r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  return rgbToHex([Math.round(r * 255 / 31), Math.round(g * 255 / 63), Math.round(b * 255 / 31)]);
}

export function deriveShades(teal, purple, lime) {
  return {
    TEAL_DEEP: lerpBlack(teal, 0.30), TEAL_DARK: lerpBlack(teal, 0.52), TEAL: teal.slice(),
    TEAL_BRIGHT: scaleClamp(teal, 1.56), TEAL_LIGHT: light(teal, 1.91, 0.20),
    PURPLE_DEEP: lerpBlack(purple, 0.27), PURPLE_DARK: lerpBlack(purple, 0.54), PURPLE: purple.slice(),
    PURPLE_BRIGHT: scaleClamp(purple, 1.61), PURPLE_LIGHT: light(purple, 2.36, 0.20),
    LIME_DARK: lerpBlack(lime, 0.31), LIME_MED: lerpBlack(lime, 0.63), LIME: lime.slice(),
    LIME_PALE: lerpWhite(lime, 0.70),
  };
}

export function themeNonce(shades) {                 // == intro_designer.theme_nonce()
  let h = 2166136261 >>> 0;
  for (const n of THEME_ORDER) for (const c of shades[n]) {
    h = (h ^ Math.max(0, Math.min(255, c))) >>> 0;
    h = Math.imul(h, 16777619) >>> 0;
  }
  return h || 1;
}

export function readTheme(img) {
  const hits = findAll(img, THEME_MAGIC);
  if (hits.length !== 1) throw new Error(`expected 1 colour block, found ${hits.length}`);
  const at = hits[0], dv = new DataView(img.buffer, img.byteOffset);
  const pal = []; for (let i = 0; i < 14; i++) pal.push(dv.getUint16(at + 28 + i * 2, true));
  return { at, version: dv.getUint32(at + 16, true), explicit: dv.getUint32(at + 20, true),
           nonce: dv.getUint32(at + 24, true), pal };
}

// Patch the chosen colours in. explicit=1 tells the board "these were chosen
// for this flash": it adopts them once and then keeps them across updates.
export async function patchTheme(original, shades) {
  const img = new Uint8Array(original);
  const L = parseAppImage(img);
  const th = readTheme(img);
  if (th.version !== 1) throw new Error("unknown colour block version " + th.version);
  if (!L.segs.find(s => th.at >= s.data && th.at + THEME_SIZE <= s.data + s.len))
    throw new Error("colour block is not wholly inside one segment");
  const dv = new DataView(img.buffer, img.byteOffset);
  dv.setUint32(th.at + 20, 1, true);
  dv.setUint32(th.at + 24, themeNonce(shades), true);
  THEME_ORDER.forEach((n, i) => dv.setUint16(th.at + 28 + i * 2, rgb565(...shades[n]), true));
  await fixImage(img);
  const v = await verifyImage(img);
  if (!v.ok) throw new Error("patched image failed verification: " + v.why);
  return img;
}

// site.js - Mantis Pinball web installer.
// The board is flashed with ESP Web Tools. We hand it a manifest we build
// ourselves so the app part can be the user's splash-patched copy (a blob URL).
import { readSplash, patchSplash, packBits, LINE_LIMITS, parseAppImage } from "./fwpatch.js";

const $ = (id) => document.getElementById(id);
const abs = (p) => new URL(p, location.href).href;
const W = 170, H = 320;

let BUILD = null, THEME = {}, FW = null, SPLASH = null;   // firmware bytes + decoded splash
let patchedURL = null, manifestURL = null;

// ---------------------------------------------------------------- startup
async function loadJSON(path) {
  const r = await fetch(path, { cache: "no-store" });
  if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`);
  return r.json();
}

async function init() {
  try { THEME = await loadJSON("js/theme.json"); } catch { THEME = {}; }
  try { BUILD = await loadJSON("firmware/build.json"); } catch (e) { BUILD = null; }
  renderModes();
  buildEditors();
  buddy();
  if (!BUILD || !BUILD.parts || !BUILD.parts.length) {
    status("installStatus", "No firmware in this build of the site yet — the GitHub build attaches it.", "bad");
    $("installAct").disabled = true; $("dlMerged").style.display = "none";
    drawPreviewLoop();
    return;
  }
  $("buildMeta").textContent = `Build ${BUILD.version}${BUILD.commit ? " · " + BUILD.commit : ""} · ${BUILD.built}`;
  $("footBuild").textContent = `Firmware ${BUILD.version} · built ${BUILD.built}`;
  const app = BUILD.parts.find(p => p.path === "firmware.bin");
  try {
    const r = await fetch(abs("firmware/" + app.path));
    FW = new Uint8Array(await r.arrayBuffer());
    parseAppImage(FW);
    SPLASH = decodeSplash(FW);
  } catch (e) {
    console.warn("splash decode failed", e);
    $("optSplash").disabled = true;
    status("splashStatus", "This firmware has no customizable splash.", "bad");
  }
  ["optContent", "optSplash"].forEach(id => $(id).addEventListener("change", refreshManifest));
  await refreshManifest();
  drawPreviewLoop();
}

function status(id, msg, cls = "") { const el = $(id); el.textContent = msg; el.className = "status " + cls; }

// ---------------------------------------------------------------- manifest
async function refreshManifest() {
  if (!BUILD) return;
  const parts = BUILD.parts.map(p => ({ path: abs("firmware/" + p.path), offset: p.offset }));
  let note = "Stock splash";
  if ($("optSplash").checked) {
    $("installAct").disabled = true;
    status("installStatus", "Writing your splash into the firmware…");
    try {
      const img = await patchSplash(FW, currentPacked());
      if (patchedURL) URL.revokeObjectURL(patchedURL);
      patchedURL = URL.createObjectURL(new Blob([img], { type: "application/octet-stream" }));
      parts.find(p => p.path.endsWith("/firmware.bin")).path = patchedURL;
      note = "Your custom splash";
    } catch (e) {
      $("optSplash").checked = false;
      status("installStatus", "Splash could not be applied: " + e.message, "bad");
      $("installAct").disabled = false;
      return refreshManifest();
    }
  }
  if ($("optContent").checked && BUILD.content) {
    parts.push({ path: abs("firmware/" + BUILD.content.path), offset: BUILD.content.offset });
  }
  const manifest = {
    name: "Mantis Crypto Pinball", version: BUILD.version,
    new_install_prompt_erase: true,
    builds: [{ chipFamily: BUILD.chipFamily || "ESP32-S3", parts }],
  };
  if (manifestURL) URL.revokeObjectURL(manifestURL);
  manifestURL = URL.createObjectURL(new Blob([JSON.stringify(manifest)], { type: "application/json" }));
  $("installBtn").setAttribute("manifest", manifestURL);
  $("installAct").disabled = false;
  status("installStatus",
    `Ready: firmware${$("optContent").checked && BUILD.content ? " + content" : ""} · ${note}`, "ok");
}

// ---------------------------------------------------------------- splash
function decodeSplash(img) {
  const sp = readSplash(img);
  return sp.lines.map(L => {
    const base = sp.at + 48 + L.off;
    const rows = [];
    for (let y = 0; y < L.h; y++) {
      const row = [];
      for (let x = 0; x < L.w; x++) row.push(!!(img[base + y * L.stride + (x >> 3)] & (0x80 >> (x & 7))));
      rows.push(row);
    }
    return { w: L.w, h: L.h, rows };
  });
}

const FONTS = [
  { name: "Bungee", css: "'Bungee'", wt: 400 },
  { name: "Press Start 2P", css: "'Press Start 2P'", wt: 400 },
  { name: "Silkscreen", css: "'Silkscreen'", wt: 700 },
  { name: "Orbitron", css: "'Orbitron'", wt: 900 },
  { name: "Rubik Mono One", css: "'Rubik Mono One'", wt: 400 },
  { name: "VT323", css: "'VT323'", wt: 400 },
  { name: "Arial Black", css: "'Arial Black', Arial, sans-serif", wt: 900 },
];
const LABELS = ["Line 1 — big title", "Line 2 — subtitle", "Line 3 — on the ribbon"];
const DEFAULT_PX = [30, 18, 15];
const lineState = [0, 1, 2].map(i => ({ text: "", font: 0, px: DEFAULT_PX[i], bmp: null, fitPx: 0 }));

function buildEditors() {
  const host = $("lines");
  lineState.forEach((st, i) => {
    const d = document.createElement("div"); d.className = "lineed";
    d.innerHTML = `<label>${LABELS[i]} (max ${LINE_LIMITS[i].maxW}×${LINE_LIMITS[i].maxH} px)</label>
      <input type="text" maxlength="32" placeholder="leave empty to keep the current line">
      <select>${FONTS.map((f, k) => `<option value="${k}">${f.name}</option>`).join("")}</select>
      <input type="range" min="8" max="60" value="${st.px}" title="size">
      <div class="fit"></div>`;
    const [txt, sel, rng] = d.querySelectorAll("input[type=text],select,input[type=range]");
    const fit = d.querySelector(".fit");
    const upd = async () => {
      st.text = txt.value; st.font = +sel.value; st.px = +rng.value;
      await renderLineState(i, fit);
      restartPreview();
      if ($("optSplash").checked) refreshManifest();
    };
    txt.addEventListener("input", upd); sel.addEventListener("change", upd); rng.addEventListener("input", upd);
    host.appendChild(d);
  });
  $("replay").onclick = restartPreview;
  $("resetSplash").onclick = () => {
    document.querySelectorAll("#lines input[type=text]").forEach(e => e.value = "");
    lineState.forEach(s => { s.text = ""; s.bmp = null; });
    document.querySelectorAll("#lines .fit").forEach(e => { e.textContent = ""; e.className = "fit"; });
    restartPreview(); if ($("optSplash").checked) refreshManifest();
  };
}

// Draw text, threshold to 1 bit, crop. Shrinks the size until it fits the
// line's budget (drawIntro puts the lines at fixed heights, so they can't grow).
async function renderLineState(i, fitEl) {
  const st = lineState[i];
  if (!st.text.trim()) { st.bmp = null; fitEl.textContent = ""; fitEl.className = "fit"; return; }
  const f = FONTS[st.font], lim = LINE_LIMITS[i];
  for (let px = st.px; px >= 6; px--) {
    const spec = `${f.wt} ${px}px ${f.css}`;
    try { await document.fonts.load(spec, st.text); } catch {}
    const bmp = rasterize(st.text, spec, px);
    if (bmp.w <= lim.maxW && bmp.h <= lim.maxH) {
      st.bmp = bmp; st.fitPx = px;
      fitEl.textContent = px === st.px ? `${bmp.w}×${bmp.h} px — fits` : `shrunk to ${px}px to fit (${bmp.w}×${bmp.h})`;
      fitEl.className = "fit"; return;
    }
  }
  st.bmp = null; fitEl.textContent = "Too long to fit — shorten the text"; fitEl.className = "fit over";
}

function rasterize(text, spec, px) {
  const c = document.createElement("canvas"); c.width = Math.max(64, px * text.length * 1.3 + 32); c.height = px * 3;
  const g = c.getContext("2d", { willReadFrequently: true });
  g.fillStyle = "#000"; g.fillRect(0, 0, c.width, c.height);
  g.fillStyle = "#fff"; g.font = spec; g.textBaseline = "top"; g.fillText(text, 8, Math.round(px * 0.6));
  const d = g.getImageData(0, 0, c.width, c.height).data;
  let x0 = c.width, y0 = c.height, x1 = -1, y1 = -1;
  const on = (x, y) => d[(y * c.width + x) * 4] >= 128;
  for (let y = 0; y < c.height; y++) for (let x = 0; x < c.width; x++)
    if (on(x, y)) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
  if (x1 < 0) return { w: 0, h: 0, rows: [] };
  const rows = [];
  for (let y = y0; y <= y1; y++) { const r = []; for (let x = x0; x <= x1; x++) r.push(on(x, y)); rows.push(r); }
  return { w: x1 - x0 + 1, h: y1 - y0 + 1, rows };
}

function lineBitmap(i) { return lineState[i].bmp || (SPLASH ? SPLASH[i] : demoLine(i)); }
function currentPacked() { return [0, 1, 2].map(i => { const b = lineBitmap(i); return packBits(b.rows, b.w); }); }
function demoLine(i) {   // only used if the site has no firmware to read from
  const t = ["MANTIS", "CRYPTO PINBALL", "READY"][i];
  return rasterize(t, `400 ${DEFAULT_PX[i]}px 'Bungee'`, DEFAULT_PX[i]);
}

// ---------------------------------------------------------------- preview
// A faithful port of drawIntro(): same positions, easings, timings, colors.
const C = (n, fb) => THEME[n] || fb;
const cv = $("preview"), cx = cv.getContext("2d");
let frame = 0;
function restartPreview() { frame = 0; }
function easeOutCubic(t) { if (t <= 0) return 0; if (t >= 1) return 1; const u = 1 - t; return 1 - u * u * u; }
function easeOutBack(t) { if (t <= 0) return 0; if (t >= 1) return 1; const c1 = 1.70158, c3 = c1 + 1, u = t - 1; return 1 + c3 * u * u * u + c1 * u * u; }
const clampf = (v, a, b) => v < a ? a : (v > b ? b : v);
function blit(b, x, y, col) {
  cx.fillStyle = col;
  for (let by = 0; by < b.h; by++) { const dy = y + by; if (dy < 0 || dy >= H) continue;
    for (let bx = 0; bx < b.w; bx++) if (b.rows[by][bx]) { const dx = x + bx; if (dx >= 0 && dx < W) cx.fillRect(dx, dy, 1, 1); } }
}
function drawIntro(f) {
  const BG = C("TEAL", "#000058"), MAIN = C("PURPLE", "#c00438"), DEEP = C("TEAL_DEEP", "#000018"),
        BRIGHT = C("TEAL_BRIGHT", "#080090"), LIME = C("LIME", "#f8fc50"), PALE = C("LIME_PALE", "#f8fcc8"),
        LDARK = C("LIME_DARK", "#484c18"), LMED = C("LIME_MED", "#a0a030");
  cx.fillStyle = BG; cx.fillRect(0, 0, W, H);
  const L0 = lineBitmap(0), L1 = lineBitmap(1), L2 = lineBitmap(2);
  { // line 1 drops in from the top with an overshoot
    const tx = Math.trunc((W - L0.w) / 2), ty = 100;
    const yM = Math.trunc(-L0.h + (ty + L0.h) * easeOutBack(clampf((f - 4) / 18, 0, 1)));
    const yI = Math.trunc(-L0.h + (ty + L0.h) * easeOutCubic(clampf((f - 7) / 18, 0, 1)));
    const yB = Math.trunc(-L0.h + (ty + L0.h) * easeOutCubic(clampf((f - 10) / 18, 0, 1)));
    blit(L0, tx + 2, yB + 2, DEEP); blit(L0, tx + 1, yI + 1, BRIGHT); blit(L0, tx, yM, MAIN);
  }
  const slide = (b, tx, ty, start, dur, from) => {
    const at = (d) => Math.trunc(from + (tx - from) * easeOutCubic(clampf((f - start - d) / dur, 0, 1)));
    blit(b, at(6) + 2, ty + 2, DEEP); blit(b, at(3) + 1, ty + 1, BRIGHT); blit(b, at(0), ty, MAIN);
  };
  slide(L1, Math.trunc((W - L1.w) / 2), 138, 14, 18, W + 30);
  { // line 3 rides in on a lime ribbon wipe
    const ty = 162, wipe = Math.trunc(W * easeOutCubic(clampf((f - 22) / 18, 0, 1)));
    const ribY = ty - 2, ribH = L2.h + 5;
    if (wipe > 0) { cx.fillStyle = LIME; cx.fillRect(0, ribY, wipe, ribH);
      cx.fillStyle = PALE; cx.fillRect(0, ribY, wipe, 1); cx.fillStyle = LDARK; cx.fillRect(0, ribY + ribH - 1, wipe, 1); }
    slide(L2, Math.trunc((W - L2.w) / 2), ty, 22, 18, -L2.w - 20);
  }
  if (f > 42 && THEME._INTRO_URL) {
    cx.fillStyle = ((f >> 4) & 1) ? LIME : LMED;
    cx.font = "8px monospace"; cx.textAlign = "center"; cx.textBaseline = "middle";
    cx.fillText(THEME._INTRO_URL, W / 2, H - 12); cx.textAlign = "start";
  }
}
let last = 0;
function drawPreviewLoop(ts = 0) {
  if (ts - last >= 16) { last = ts; drawIntro(frame); if (frame < 400) frame++; }
  requestAnimationFrame(drawPreviewLoop);
}

// ---------------------------------------------------------------- modes
function renderModes() {
  const list = (BUILD && BUILD.modes) || [];
  $("modesSub").textContent = list.length ? `${list.length} modes in this build:` : "Mode list appears once the firmware is built.";
  $("modeList").innerHTML = list.filter(m => m !== "SETUP").map(m => `<div class="mode">${m}</div>`).join("");
}

// ---------------------------------------------------------------- buddy
function buddy() {
  const el = $("buddy"), bub = $("bubble");
  if (matchMedia("(prefers-reduced-motion: reduce)").matches) return;
  const size = () => el.clientWidth;
  const show = (i) => el.style.backgroundPosition = `${-i * size()}px 0`;
  const idle = [[0, 2200], [1, 140], [0, 1400], [2, 900], [6, 1200], [7, 900], [8, 1300], [3, 500], [4, 500], [0, 900]];
  let k = 0, busy = false;
  const tick = () => { if (busy) return setTimeout(tick, 300); const [p, ms] = idle[k++ % idle.length]; show(p); setTimeout(tick, ms); };
  tick();
  const lines = ["Pick a game!", "Two buttons. That's it.", "Grab a board ↗", "Try the splash designer!", "Hit big!"];
  let n = 0;
  el.addEventListener("click", () => {
    busy = true; bub.textContent = lines[n++ % lines.length]; el.classList.add("talk");
    show(9); setTimeout(() => show(5), 260); setTimeout(() => show(11), 600);
    setTimeout(() => { el.classList.remove("talk"); busy = false; }, 1800);
  });
}

init();

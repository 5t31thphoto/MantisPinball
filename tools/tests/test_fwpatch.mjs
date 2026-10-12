import fs from "fs";
import { patchSplash, readSplash, packBits, verifyImage, parseAppImage } from "../../website/js/fwpatch.js";
const orig = new Uint8Array(fs.readFileSync("app.bin"));
// three new lines: a checkerboard, a solid bar, a diagonal - all inside limits
const mk = (w,h,f) => packBits(Array.from({length:h},(_,y)=>Array.from({length:w},(_,x)=>f(x,y))), w);
const lines = [ mk(120,30,(x,y)=>((x>>2)+(y>>2))&1), mk(150,20,()=>true), mk(90,40,(x,y)=>x===y*2) ];
const out = await patchSplash(orig, lines);
fs.writeFileSync("app_patched.bin", out);
// 1) splash reads back exactly
const sp = readSplash(out); let ok = true;
lines.forEach((p,i)=>{ const L=sp.lines[i]; const got=out.subarray(sp.at+48+L.off, sp.at+48+L.off+L.stride*L.h);
  const same = L.w===p.w && L.h===p.h && L.stride===p.stride && got.every((b,k)=>b===p.bytes[k]);
  console.log(`  line ${i+1}: ${L.w}x${L.h} stride ${L.stride} ${same?"reads back exactly":"MISMATCH"}`); ok=ok&&same; });
// 2) nothing outside blob/checksum/hash changed
const P = parseAppImage(out); let stray = 0;
for (let i=0;i<out.length;i++){ const inBlob = i>=sp.at+24 && i<sp.at+48+sp.cap; const inFix = i===P.csumOff || (i>P.csumOff && i<=P.csumOff+32);
  if (!inBlob && !inFix && out[i]!==orig[i]) stray++; }
console.log(`  bytes changed outside blob/checksum/hash: ${stray}`);
console.log(`  original download untouched: ${fs.readFileSync("app.bin").equals(Buffer.from(orig))}`);
// 3) rejections - a bad patch must never reach the device
for (const [name, bad] of [["line 2 too tall (30px > 22)", [lines[0], mk(50,30,()=>1), lines[2]]],
                           ["line too wide (170px)",     [mk(170,10,()=>1), lines[1], lines[2]]],
                           ["only two lines",            [lines[0], lines[1]]]]) {
  try { await patchSplash(orig, bad); console.log(`  REJECT FAILED: ${name}`); ok=false; }
  catch(e){ console.log(`  rejected: ${name}`); } }
const dbl = new Uint8Array(orig.length+16); dbl.set(orig); dbl.set(new TextEncoder().encode("MANTIS_SPLASH_1\0"), orig.length);
try { await patchSplash(dbl, lines); console.log("  REJECT FAILED: duplicate tag"); ok=false; } catch(e){ console.log("  rejected: duplicate splash tag"); }
process.exit(ok && stray===0 ? 0 : 1);

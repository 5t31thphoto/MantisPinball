import fs from "fs";
import { readSplash, patchSplash, packBits, verifyImage } from "../../website/js/fwpatch.js";
// same decode the site uses
function decodeSplash(img){ const sp=readSplash(img); return sp.lines.map(L=>{ const base=sp.at+48+L.off, rows=[];
  for(let y=0;y<L.h;y++){const r=[];for(let x=0;x<L.w;x++) r.push(!!(img[base+y*L.stride+(x>>3)]&(0x80>>(x&7))));rows.push(r);} return {w:L.w,h:L.h,rows}; }); }
const orig=new Uint8Array(fs.readFileSync("app.bin"));
const lines=decodeSplash(orig).map(b=>packBits(b.rows,b.w));        // "custom splash" ticked, nothing edited
const out=await patchSplash(orig, lines);
let diff=0; for(let i=0;i<orig.length;i++) if(orig[i]!==out[i]) diff++;
console.log(`  untouched-designer patch: ${diff} bytes differ from the original (must be 0)`);
console.log(`  verifies: ${(await verifyImage(out)).ok}`);
process.exit(diff===0?0:1);

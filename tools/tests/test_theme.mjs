// The colour patch writes ONLY the colour block (+ the image checksum/hash),
// the result verifies, the original download is never modified, and it chains
// with the splash patch.
import fs from "fs";
import { parseAppImage, verifyImage, readTheme, patchTheme, deriveShades, themeNonce, rgb565,
         THEME_ORDER, THEME_SIZE, readSplash, patchSplash } from "../../website/js/fwpatch.js";
const orig = new Uint8Array(fs.readFileSync(process.argv[2] || "app.bin"));
const before = orig.slice();
let fails = 0; const check = (ok, w) => { console.log(`  ${ok ? "ok  " : "FAIL"} ${w}`); if (!ok) fails++; };
const th0 = readTheme(orig);
check(th0.explicit === 0, "a default build's colour block is NOT explicit (boards keep their colours)");
const shades = deriveShades([255, 64, 0], [20, 200, 90], [240, 240, 10]);
const out = await patchTheme(orig, shades);
const th = readTheme(out);
check(th.explicit === 1 && th.nonce === themeNonce(shades), "patched block is explicit, with the nonce for these colours");
check(THEME_ORDER.every((n, i) => th.pal[i] === rgb565(...shades[n])), "all 14 shades written as rgb565");
check((await verifyImage(out)).ok, "patched image passes checksum + SHA-256");
const L = parseAppImage(out);
let stray = 0;
for (let i = 0; i < out.length; i++) {
  if (out[i] === orig[i]) continue;
  const inBlock = i >= th.at && i < th.at + THEME_SIZE, inTail = i >= L.csumOff && i <= L.csumOff + 32;
  if (!inBlock && !inTail) stray++;
}
check(stray === 0, "no byte changed outside the colour block and the checksum/hash");
check(orig.every((b, i) => b === before[i]), "the original download is untouched");
// chain with the splash patch (both customised)
const sp = readSplash(orig);
const packed = [0, 1, 2].map(() => ({ w: 8, h: 2, stride: 1, bytes: new Uint8Array([0xFF, 0x81]) }));
const both = await patchTheme(await patchSplash(orig, packed), shades);
check((await verifyImage(both)).ok && readTheme(both).explicit === 1 && readSplash(both).lines[0].w === 8,
      "splash + colours patched together still verifies, both applied");
console.log(fails ? `\n${fails} FAILED` : "\nCOLOUR PATCH CHECKS PASSED");
process.exit(fails);

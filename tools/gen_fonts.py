#!/usr/bin/env python3
# ============================================================================
#  gen_fonts.py
#
#  Regenerates src/intro_bitmaps.h and mockup/intro_mockup.png from a TTF.
#
#  The intro animation (three title lines sliding in with parallax) reads
#  these baked bitmaps and renders them pixel-by-pixel on the device. By
#  swapping the input TTF and re-running this script, you can change the
#  intro typeface without touching any C++ code.
#
#  Usage:
#      python3 tools/gen_fonts.py
#          uses the bundled-default font (looks up a system Bold TTF)
#
#      python3 tools/gen_fonts.py --ttf path/to/font.ttf
#          uses the supplied TTF
#
#      python3 tools/gen_fonts.py --ttf PressStart2P-Regular.ttf
#          recommended retro arcade font:
#          https://fonts.google.com/specimen/Press+Start+2P
#
#  After regenerating, just rebuild and reflash:
#      pio run -t upload
#
#  Requirements: Pillow.
#      pip install Pillow
# ============================================================================

import argparse
import os
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.stderr.write("Pillow is required. Install with:  pip install Pillow\n")
    sys.exit(1)

ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
#  Default font fallback
# ---------------------------------------------------------------------------
DEFAULT_FONTS = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
]

def find_default_font():
    for p in DEFAULT_FONTS:
        if os.path.exists(p):
            return p
    raise SystemExit(
        "No bundled-default TTF found on this system.\n"
        "Pass --ttf path/to/font.ttf  (e.g. PressStart2P-Regular.ttf)"
    )


# ---------------------------------------------------------------------------
#  Title-line specifications.
#
#  Each line targets a cap-height in pixels that, combined with the chosen
#  TTF, produces a width that fits inside W=170 with comfortable margins.
#  If a line ends up too wide (some heavy TTFs like Press Start 2P render
#  wider than DejaVu), we shrink-to-fit.
# ---------------------------------------------------------------------------
INTRO_LINES = [
    # (var_name,     text,         target_cap_height_px, max_width_px)
    ("KYLER",        "KYLER",        28,                   150),
    ("MICHAELSON",   "MICHAELSON",   14,                   158),
    ("STUDIOS",      "STUDIOS",      18,                   135),
]


def render_line_bitmap(ttf_path, text, target_cap_h, max_w):
    """Render `text` as a tight 1-bit bitmap.

    Strategy: pick a font size so the rendered cap-height (height of "H")
    is approximately `target_cap_h`. If the result exceeds `max_w` pixels
    wide, scale down until it fits. No anti-aliasing - the device is
    drawing pixel-by-pixel, so we want crisp 1-bit output.
    """
    # Initial size guess: cap height is typically ~70% of em size.
    size = int(target_cap_h / 0.70)

    for attempt in range(8):
        font = ImageFont.truetype(ttf_path, size)
        # Measure height of "H" for cap-height calibration.
        h_bbox = font.getbbox("H")
        cap_h = h_bbox[3] - h_bbox[1]
        # Measure full text width.
        t_bbox = font.getbbox(text)
        text_w = t_bbox[2] - t_bbox[0]

        too_wide = text_w > max_w
        if too_wide:
            size = max(6, int(size * (max_w / text_w) * 0.97))
            continue
        # If cap_h is way off, retune.
        if abs(cap_h - target_cap_h) > 2 and attempt < 4:
            size = max(6, int(size * target_cap_h / max(1, cap_h)))
            continue
        break

    # Final render. Use a generous canvas, then crop to ink bounds.
    canvas_w = max_w + 20
    canvas_h = int(target_cap_h * 2.5) + 10
    img = Image.new("L", (canvas_w, canvas_h), 0)
    d = ImageDraw.Draw(img)
    # Draw at (0,0) using the font's natural origin alignment ("la" = left ascender)
    d.text((4, 4), text, fill=255, font=font, anchor="la")

    # Threshold to 1-bit (no AA - pixels >= 128 become "on")
    img = img.point(lambda v: 255 if v >= 128 else 0, mode="L").convert("1")

    # Crop to the actual ink bounding box.
    bbox = img.getbbox()
    if bbox is None:
        # Empty - shouldn't happen, but degrade gracefully
        return Image.new("1", (1, 1), 0)
    img = img.crop(bbox)

    # Final width sanity-check: if STILL too wide, scale 1-bit down with NEAREST.
    if img.size[0] > max_w:
        new_w = max_w
        new_h = max(1, int(img.size[1] * max_w / img.size[0]))
        img = img.resize((new_w, new_h), Image.NEAREST)

    return img


def pack_bitmap_to_bytes(img):
    """Pack a PIL '1' image into MSB-first row-major bytes.

    Each row is padded to a whole number of bytes. The packed format is
    what intro.cpp expects: byte 0 has bits 7..0 = pixels 0..7 of row 0.
    """
    w, h = img.size
    bytes_per_row = (w + 7) // 8
    px = img.load()
    out = bytearray(bytes_per_row * h)
    for y in range(h):
        for x in range(w):
            if px[x, y]:                          # white pixel = bit set
                out[y * bytes_per_row + (x // 8)] |= (0x80 >> (x % 8))
    return bytes(out)


def write_intro_header(path, ttf_path, lines_data):
    """Emit src/intro_bitmaps.h with packed bitmap arrays."""
    with open(path, "w") as f:
        f.write("// ============================================================\n")
        f.write("//  intro_bitmaps.h - GENERATED FILE - do not edit by hand.\n")
        f.write("//\n")
        f.write("//  Regenerate via:    python3 tools/gen_fonts.py [--ttf FILE]\n")
        f.write("//\n")
        f.write(f"//  Source TTF:  {os.path.basename(ttf_path)}\n")
        f.write("//\n")
        f.write("//  Bitmap format: MSB-first row-major, 1 bit per pixel.\n")
        f.write("//  byte index = y * bytesPerRow + (x >> 3)\n")
        f.write("//  bit  index = 7 - (x & 7)   (1 = ink, 0 = transparent)\n")
        f.write("// ============================================================\n")
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")

        f.write("struct IntroBitmap {\n")
        f.write("    const uint8_t* data;\n")
        f.write("    int            width;\n")
        f.write("    int            height;\n")
        f.write("    int            stride;     // bytes per row\n")
        f.write("};\n\n")

        for var, text, w, h, packed in lines_data:
            f.write(f"// \"{text}\"  ({w}x{h})\n")
            f.write(f"static const uint8_t INTRO_BMP_{var}_DATA[] = {{\n    ")
            for i, b in enumerate(packed):
                if i and i % 12 == 0:
                    f.write("\n    ")
                f.write(f"0x{b:02X}, ")
            f.write("\n};\n")
            stride = (w + 7) // 8
            f.write(f"static const IntroBitmap INTRO_BMP_{var} = {{\n")
            f.write(f"    INTRO_BMP_{var}_DATA, {w}, {h}, {stride}\n")
            f.write("};\n\n")


# ---------------------------------------------------------------------------
#  Mockup PNG
#
#  Produces a 170x320 image showing roughly what the intro looks like at
#  the moment all the text has settled. Stacked-shadow rendering of the
#  three lines, the parallax orbs hinted, the URL line, the corner ticks.
# ---------------------------------------------------------------------------
TEAL_DEEP   = (0,  35,  35)
TEAL        = (0, 115, 115)
TEAL_BRIGHT = (0, 180, 180)
PURPLE_DARK = (50, 0, 50)
PURPLE      = (93, 0, 93)
LIME        = (0, 255, 0)
LIME_DARK   = (0,  80, 0)
LIME_PALE   = (180, 255, 180)


def stamp_bitmap(canvas, bmp_img, x, y, color):
    """Stamp a 1-bit PIL image onto the RGB canvas at (x, y) using `color`."""
    cw, ch = canvas.size
    bw, bh = bmp_img.size
    bpx = bmp_img.load()
    cpx = canvas.load()
    for by in range(bh):
        for bx in range(bw):
            if not bpx[bx, by]:
                continue
            cx, cy = x + bx, y + by
            if 0 <= cx < cw and 0 <= cy < ch:
                cpx[cx, cy] = color


def render_mockup(out_path, lines):
    W, H = 170, 320
    bg = Image.new("RGB", (W, H), TEAL)

    # Three parallax orbs (drawn before text)
    d = ImageDraw.Draw(bg)
    for cx, cy, r, col in [
        (45, 60,  6, PURPLE_DARK),
        (130, 200, 5, LIME_DARK),
        (90, 255,  7, PURPLE),
    ]:
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)

    # Text region backing
    d.rectangle([0, 88, W, 88 + 78], fill=(0, 60, 60))

    bmp_kyler = lines["KYLER"]["img"]
    bmp_mich  = lines["MICHAELSON"]["img"]
    bmp_studs = lines["STUDIOS"]["img"]

    # KYLER - centered at y=96, stacked shadow
    kx = (W - bmp_kyler.size[0]) // 2
    ky = 96
    stamp_bitmap(bg, bmp_kyler, kx + 2, ky + 2, TEAL_DEEP)
    stamp_bitmap(bg, bmp_kyler, kx + 1, ky + 1, TEAL_BRIGHT)
    stamp_bitmap(bg, bmp_kyler, kx,     ky,     PURPLE)

    # MICHAELSON
    mx = (W - bmp_mich.size[0]) // 2
    my = 132
    stamp_bitmap(bg, bmp_mich, mx + 1, my + 1, TEAL_DEEP)
    stamp_bitmap(bg, bmp_mich, mx,     my,     PURPLE)

    # STUDIOS - sitting in a lime ribbon
    rib_y = 158
    rib_h = max(bmp_studs.size[1] + 6, 22)
    d.rectangle([0, rib_y, W, rib_y + rib_h], fill=LIME)
    d.rectangle([0, rib_y - 1, W, rib_y], fill=LIME_PALE)
    d.rectangle([0, rib_y + rib_h, W, rib_y + rib_h + 1], fill=LIME_DARK)
    sx = (W - bmp_studs.size[0]) // 2
    sy = rib_y + (rib_h - bmp_studs.size[1]) // 2
    stamp_bitmap(bg, bmp_studs, sx, sy, PURPLE)

    # PRESS TO PLAY
    try:
        small = ImageFont.truetype(DEFAULT_FONTS[0], 11)
    except Exception:
        small = ImageFont.load_default()
    d.text((W // 2, H - 38), "PRESS  TO  PLAY", font=small,
           fill=LIME, anchor="mm")
    d.text((W // 2, H - 12), "kylermichaelson.carrd.co", font=small,
           fill=LIME, anchor="mm")

    # Corner ticks (lime)
    for (x0, y0, x1, y1) in [
        (0, 0, 14, 1),  (0, 0, 1, 14),
        (W - 14, 0, W, 1), (W - 1, 0, W, 14),
        (0, H - 1, 14, H), (0, H - 14, 1, H),
        (W - 14, H - 1, W, H), (W - 1, H - 14, W, H),
    ]:
        d.rectangle([x0, y0, x1, y1], fill=LIME)

    # 2x scale for visibility in the mockup
    bg = bg.resize((W * 2, H * 2), Image.NEAREST)
    bg.save(out_path)
    print(f"  -> {out_path}")


# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ttf", default=None,
                    help="Path to TTF (default: bundled-system Sans Bold)")
    args = ap.parse_args()

    ttf = args.ttf or find_default_font()
    if not os.path.exists(ttf):
        sys.exit(f"TTF not found: {ttf}")
    print(f"Using font: {ttf}")

    lines_for_header = []
    lines_for_mockup = {}

    for var, text, target_h, max_w in INTRO_LINES:
        img = render_line_bitmap(ttf, text, target_h, max_w)
        w, h = img.size
        packed = pack_bitmap_to_bytes(img)
        lines_for_header.append((var, text, w, h, packed))
        lines_for_mockup[var] = {"img": img, "w": w, "h": h}
        print(f"  '{text}': {w} x {h}  ({len(packed)} bytes)")

    out_h = ROOT / "src" / "intro_bitmaps.h"
    out_h.parent.mkdir(parents=True, exist_ok=True)
    write_intro_header(out_h, ttf, lines_for_header)
    print(f"Wrote {out_h}")

    out_png = ROOT / "mockup" / "intro_mockup.png"
    out_png.parent.mkdir(parents=True, exist_ok=True)
    render_mockup(out_png, lines_for_mockup)
    print("Done.")


if __name__ == "__main__":
    main()

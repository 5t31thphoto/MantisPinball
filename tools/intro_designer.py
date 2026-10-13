#!/usr/bin/env python3
# ============================================================================
#  intro_designer.py - GUI for customizing the intro screen and theme colors
#
#  Lets students change:
#    - The three primary theme colors (TEAL/PURPLE/LIME equivalents)
#    - The three title lines (text, TTF font file, font size)
#    - The URL line shown at the bottom
#
#  Live-previews the intro animation in a Tk canvas at 2x scale, and on
#  Export writes:
#    - src/theme_colors.h   (14 derived color shades + INTRO_URL)
#    - src/intro_bitmaps.h  (packed bitmaps for the three title lines)
#
#  Requirements: Pillow.   pip install Pillow
# ============================================================================

import json
import math
import os
import sys
import tkinter as tk
from pathlib import Path
from tkinter import colorchooser, filedialog, messagebox, ttk

try:
    from PIL import Image, ImageDraw, ImageFont, ImageTk
except ImportError:
    sys.stderr.write("Pillow is required.  pip install Pillow\n")
    sys.exit(1)

ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "src"

W, H = 170, 320
SCALE = 2

# ---------------------------------------------------------------------------
#  Default-font fallback (used only when the user doesn't pick a TTF)
# ---------------------------------------------------------------------------
DEFAULT_FONTS = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
]


def find_default_font():
    for p in DEFAULT_FONTS:
        if os.path.exists(p):
            return p
    return None


# ---------------------------------------------------------------------------
#  Shade derivation
#
#  Each base color has a fixed set of "tints" relative to itself. We compute
#  each shade as a brightness scale of the base (toward black) or a lerp
#  toward white (for the lighter tints). These match the original hand-tuned
#  shades closely when the base colors are at their original values.
#
#  scale < 1 darkens; scale > 1 brightens (lerps to white)
# ---------------------------------------------------------------------------
def lerp_black(rgb, t):
    return tuple(int(round(c * t)) for c in rgb)


def scale_clamp(rgb, factor):
    """Multiply each channel by `factor`, clamp to 0..255. Preserves hue
       better than lerping toward white (which adds color to dead channels)."""
    return tuple(max(0, min(255, int(round(c * factor)))) for c in rgb)


def lerp_white(rgb, t):
    return tuple(int(round(c + (255 - c) * t)) for c in rgb)


def derive_shades(base_teal, base_purple, base_lime):
    """Return dict of all 14 named shades for the given three primaries.

    Dark variants are brightness-scaled toward black; bright variants are
    brightness-scaled toward saturation; "light" variants get an extra
    touch of white-bias to match the original hand-tuned palette.
    """
    def light(rgb, scale, white_bias):
        scaled = scale_clamp(rgb, scale)
        return lerp_white(scaled, white_bias)

    return {
        # Teal family
        "TEAL_DEEP":    lerp_black(base_teal,   0.30),
        "TEAL_DARK":    lerp_black(base_teal,   0.52),
        "TEAL":         base_teal,
        "TEAL_BRIGHT":  scale_clamp(base_teal,  1.56),
        "TEAL_LIGHT":   light       (base_teal,  1.91, 0.20),
        # Purple family
        "PURPLE_DEEP":  lerp_black(base_purple, 0.27),
        "PURPLE_DARK":  lerp_black(base_purple, 0.54),
        "PURPLE":       base_purple,
        "PURPLE_BRIGHT":scale_clamp(base_purple,1.61),
        "PURPLE_LIGHT": light       (base_purple,2.36, 0.20),
        # Lime family
        "LIME_DARK":    lerp_black(base_lime,   0.31),
        "LIME_MED":     lerp_black(base_lime,   0.63),
        "LIME":         base_lime,
        "LIME_PALE":    lerp_white(base_lime,   0.70),
    }


def rgb_to_565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def hex_to_rgb(h):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


def rgb_to_hex(r, g, b):
    return f"#{r:02X}{g:02X}{b:02X}"


# ---------------------------------------------------------------------------
#  Bitmap rendering (same packing format as gen_fonts.py)
# ---------------------------------------------------------------------------
def render_text_bitmap(ttf_path, text, target_h, max_w=158):
    """Render `text` to a tight 1-bit PIL image, target cap-height target_h."""
    size = int(target_h / 0.70)
    for _ in range(8):
        try:
            font = ImageFont.truetype(ttf_path, size)
        except OSError:
            font = ImageFont.load_default()
            break
        h_bbox = font.getbbox("H")
        cap_h = h_bbox[3] - h_bbox[1]
        t_bbox = font.getbbox(text)
        text_w = t_bbox[2] - t_bbox[0]
        if text_w > max_w:
            size = max(6, int(size * (max_w / text_w) * 0.97))
            continue
        if abs(cap_h - target_h) > 2:
            size = max(6, int(size * target_h / max(1, cap_h)))
            continue
        break

    canvas_w = max_w + 20
    canvas_h = int(target_h * 2.5) + 10
    img = Image.new("L", (canvas_w, canvas_h), 0)
    d = ImageDraw.Draw(img)
    d.text((4, 4), text, fill=255, font=font, anchor="la")
    img = img.point(lambda v: 255 if v >= 128 else 0, mode="L").convert("1")
    bbox = img.getbbox()
    if bbox is None:
        return Image.new("1", (1, 1), 0)
    img = img.crop(bbox)
    if img.size[0] > max_w:
        new_w = max_w
        new_h = max(1, int(img.size[1] * max_w / img.size[0]))
        img = img.resize((new_w, new_h), Image.NEAREST)
    return img


def pack_bitmap(img):
    w, h = img.size
    bpr = (w + 7) // 8
    px = img.load()
    out = bytearray(bpr * h)
    for y in range(h):
        for x in range(w):
            if px[x, y]:
                out[y * bpr + (x // 8)] |= (0x80 >> (x % 8))
    return bytes(out), w, h, bpr


def write_intro_bitmaps_header(path, lines):
    """lines = list of (var_name, text, packed_bytes, w, h, bpr)

    Writes the PATCHABLE blob format (see gen_intro_blob.py) so the web
    flasher's splash designer keeps working after a tkinter build. The names
    drawIntro() uses are unchanged, so the firmware sees no difference.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import gen_intro_blob as GB
    order = {"KYLER": 0, "MICHAELSON": 1, "STUDIOS": 2}
    rows = sorted(lines, key=lambda L: order.get(L[0], 9))
    blob_lines = [(text, w, h, bpr, bytes(packed)) for _v, text, packed, w, h, bpr in rows]
    with open(path, "wb") as f:
        f.write(GB.emit(blob_lines).encode("utf-8"))


THEME_ORDER = ["TEAL_DEEP", "TEAL_DARK", "TEAL", "TEAL_BRIGHT", "TEAL_LIGHT",
               "PURPLE_DEEP", "PURPLE_DARK", "PURPLE", "PURPLE_BRIGHT", "PURPLE_LIGHT",
               "LIME_DARK", "LIME_MED", "LIME", "LIME_PALE"]


def theme_nonce(shades):
    """FNV-1a over the 14 shades: identifies 'this colour choice'."""
    h = 2166136261
    for name in THEME_ORDER:
        for c in shades[name]:
            h ^= max(0, min(255, int(c))); h = (h * 16777619) & 0xFFFFFFFF
    return h or 1


def write_theme_header(path, shades, url, explicit=True):
    """theme_colors.h for the RUNTIME palette.

    The 14 names (TEAL ... LIME_PALE) read g_theme, filled at boot by
    themeInit() from a patchable block in the firmware (theme_runtime.cpp).
    explicit=True  -> this build CHOSE these colours (tkinter): the board adopts
                      them once, then keeps them across later updates.
    explicit=False -> project defaults (the website build): a board that
                      already has colours keeps its own.
    Each default is also written as 'NAME = rgb565(r, g, b)' so tools can read
    the theme back from this file.
    """
    with open(path, "w") as f:
        f.write("// theme_colors.h - GENERATED. Edit via tools/intro_designer.py\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("// rgb565 color packing helper - must appear before all color constants\n")
        f.write("#ifndef RGB565_DEFINED\n#define RGB565_DEFINED\n")
        f.write("static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {\n")
        f.write("    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));\n")
        f.write("}\n#endif\n\n")
        f.write("// Runtime palette: 14 shades derived from 3 base colours. themeInit() fills\n")
        f.write("// it at boot - from this build's colours or the board's saved ones.\n")
        f.write("struct MantisPalette { uint16_t s[14]; };\n")
        f.write("extern MantisPalette g_theme;\n")
        f.write("void themeInit();\n\n")
        for i, name in enumerate(THEME_ORDER):
            f.write("#define %-14s (g_theme.s[%d])\n" % (name, i))
        f.write("\n// Tokens for const tables (a table can't hold a runtime value without\n")
        f.write("// moving into RAM and freezing the boot-time colour): store TH_x, read via thc().\n")
        f.write("enum : uint16_t {\n")
        for i, name in enumerate(THEME_ORDER):
            f.write("    TH_%s = %d,\n" % (name, i + 1))
        f.write("};\n")
        f.write("static inline uint16_t thc(uint16_t c) { return (c >= 1 && c <= 14) ? g_theme.s[c - 1] : c; }\n\n")
        f.write("#define THEME_EXPLICIT %d\n" % (1 if explicit else 0))
        f.write("#define THEME_NONCE    0x%08Xu\n" % theme_nonce(shades))
        f.write("#define THEME_DEFAULTS { \\\n")
        for name in THEME_ORDER:
            r, g, b = (max(0, min(255, int(v))) for v in shades[name])
            pad = " " * (14 - len(name))
            f.write("    rgb565(%3d, %3d, %3d), /* %s%s= rgb565(%3d, %3d, %3d) */ \\\n" % (r, g, b, name, pad, r, g, b))
        f.write("}\n")
        safe_url = url.replace('\\', '\\\\').replace('"', '\\"')
        f.write('#define INTRO_URL "%s"\n' % safe_url)


def ease_out_cubic(t):
    if t <= 0: return 0
    if t >= 1: return 1
    u = 1 - t
    return 1 - u * u * u


def ease_out_back(t):
    if t <= 0: return 0
    if t >= 1: return 1
    c1 = 1.70158
    c3 = c1 + 1.0
    u = t - 1.0
    return 1.0 + c3 * u * u * u + c1 * u * u


# Orb specs matching the firmware
INTRO_ORBS = [
    (85,  240, 60, 30, 0.008, 0.00, 8, "PURPLE_DARK"),
    (85,  280, 70, 20, 0.011, 2.10, 7, "TEAL_DARK"),
    (60,  60,  35, 18, 0.018, 1.20, 5, "PURPLE"),
    (120, 220, 40, 25, 0.020, 3.50, 5, "LIME_DARK"),
    (90,  200, 55, 35, 0.030, 0.80, 3, "LIME_MED"),
    (85,  260, 45, 20, 0.035, 4.20, 3, "PURPLE_LIGHT"),
]


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def render_intro_frame(frame, shades, bitmaps, url):
    """Render one animation frame as a PIL RGB image at native 170x320."""
    img = Image.new("RGB", (W, H), shades["TEAL"])
    d = ImageDraw.Draw(img)

    # Orbs
    for (bx, by, rx, ry, sp, off, sz, ckey) in INTRO_ORBS:
        a = frame * sp + off
        ox = int(bx + math.cos(a) * rx)
        oy = int(by + math.sin(a) * ry)
        tx = int(bx + math.cos(a - 0.15) * rx)
        ty = int(by + math.sin(a - 0.15) * ry)
        trail_sz = sz - 2 if sz > 4 else 1
        d.ellipse([tx - trail_sz, ty - trail_sz, tx + trail_sz, ty + trail_sz],
                  fill=shades["TEAL_DARK"])
        d.ellipse([ox - sz, oy - sz, ox + sz, oy + sz], fill=shades[ckey])

    # Title backing band
    d.rectangle([0, 90, W, 90 + 92], fill=shades["TEAL_DARK"])
    d.line([(0, 90), (W, 90)], fill=shades["TEAL"])
    d.line([(0, 181), (W, 181)], fill=shades["TEAL"])

    purple_main = shades["PURPLE"]
    teal_deep = shades["TEAL_DEEP"]
    teal_bright = shades["TEAL_BRIGHT"]
    lime = shades["LIME"]
    lime_pale = shades["LIME_PALE"]
    lime_dark = shades["LIME_DARK"]
    lime_med = shades["LIME_MED"]

    def blit_bmp(bmp_img, x, y, color):
        cw, ch = img.size
        bw, bh = bmp_img.size
        bpx = bmp_img.load()
        ipx = img.load()
        for by_ in range(bh):
            dy = y + by_
            if dy < 0 or dy >= ch:
                continue
            for bx_ in range(bw):
                if not bpx[bx_, by_]:
                    continue
                dx = x + bx_
                if 0 <= dx < cw:
                    ipx[dx, dy] = color

    # KYLER - drops from above
    bmp = bitmaps["KYLER"]
    if bmp:
        bw, bh = bmp.size
        targetX = (W - bw) // 2
        targetY = 100
        t1 = clamp((frame - 4) / 18.0, 0, 1)
        t2 = clamp((frame - 7) / 18.0, 0, 1)
        t3 = clamp((frame - 10) / 18.0, 0, 1)
        e1 = ease_out_back(t1)
        e2 = ease_out_cubic(t2)
        e3 = ease_out_cubic(t3)
        yM = int(-bh + (targetY + bh) * e1)
        yMid = int(-bh + (targetY + bh) * e2)
        yBack = int(-bh + (targetY + bh) * e3)
        blit_bmp(bmp, targetX + 2, yBack + 2, teal_deep)
        blit_bmp(bmp, targetX + 1, yMid + 1, teal_bright)
        blit_bmp(bmp, targetX, yM, purple_main)

    def layered_slide(bmp, targetX, targetY, startFrame, duration, slideFromX, overshoot):
        t = clamp((frame - startFrame) / duration, 0, 1)
        tMid = clamp((frame - startFrame - 3) / duration, 0, 1)
        tBack = clamp((frame - startFrame - 6) / duration, 0, 1)
        ef = ease_out_back if overshoot else ease_out_cubic
        eM = ef(t)
        eMid = ef(tMid)
        eBack = ef(tBack)
        xM = int(slideFromX + (targetX - slideFromX) * eM)
        xMid = int(slideFromX + (targetX - slideFromX) * eMid)
        xBack = int(slideFromX + (targetX - slideFromX) * eBack)
        blit_bmp(bmp, xBack + 2, targetY + 2, teal_deep)
        blit_bmp(bmp, xMid + 1, targetY + 1, teal_bright)
        blit_bmp(bmp, xM, targetY, purple_main)

    bmp = bitmaps["MICHAELSON"]
    if bmp:
        bw, _ = bmp.size
        layered_slide(bmp, (W - bw) // 2, 138, 14, 18, W + 30, False)

    bmp = bitmaps["STUDIOS"]
    if bmp:
        bw, bh = bmp.size
        targetX = (W - bw) // 2
        targetY = 162
        ribT = clamp((frame - 22) / 18.0, 0, 1)
        ribE = ease_out_cubic(ribT)
        wipeW = int(W * ribE)
        ribY = targetY - 2
        ribH = bh + 5
        if wipeW > 0:
            d.rectangle([0, ribY, wipeW, ribY + ribH], fill=lime)
            d.line([(0, ribY), (wipeW, ribY)], fill=lime_pale)
            d.line([(0, ribY + ribH - 1), (wipeW, ribY + ribH - 1)], fill=lime_dark)
        layered_slide(bmp, targetX, targetY, 22, 18, -bw - 20, False)

    if frame > 42:
        pulse = (frame >> 4) & 1
        try:
            small = ImageFont.truetype(find_default_font() or "", 11)
        except Exception:
            small = ImageFont.load_default()
        d.text((W // 2, H - 36), "PRESS  TO  PLAY",
               font=small, fill=lime if pulse else lime_med, anchor="mm")
        d.text((W // 2, H - 12), url, font=small, fill=lime, anchor="mm")

    # Corner ticks
    for (x0, y0, x1, y1) in [
        (0, 0, 14, 1), (0, 0, 1, 14),
        (W - 14, 0, W, 1), (W - 1, 0, W, 14),
        (0, H - 1, 14, H), (0, H - 14, 1, H),
        (W - 14, H - 1, W, H), (W - 1, H - 14, W, H),
    ]:
        d.rectangle([x0, y0, x1, y1], fill=lime)

    return img


# ---------------------------------------------------------------------------
#  Tk application
# ---------------------------------------------------------------------------
class App:
    def __init__(self, root):
        self.root = root
        root.title("Pinball-S3 Intro Designer")
        root.geometry("920x720")
        root.configure(bg="#202020")

        self.default_ttf = find_default_font()

        # State
        self.lines = [
            {"var": "KYLER",      "text": tk.StringVar(value="KYLER"),
             "ttf": tk.StringVar(value=self.default_ttf or ""),
             "size": tk.IntVar(value=28)},
            {"var": "MICHAELSON", "text": tk.StringVar(value="MICHAELSON"),
             "ttf": tk.StringVar(value=self.default_ttf or ""),
             "size": tk.IntVar(value=14)},
            {"var": "STUDIOS",    "text": tk.StringVar(value="STUDIOS"),
             "ttf": tk.StringVar(value=self.default_ttf or ""),
             "size": tk.IntVar(value=18)},
        ]
        self.url_var = tk.StringVar(value="kylermichaelson.carrd.co")
        self.color_teal   = "#007373"
        self.color_purple = "#5D005D"
        self.color_lime   = "#00FF00"

        self.frame_idx = 0
        self.bitmaps_cache = {}
        self.bitmaps_dirty = True

        self._build_ui()
        self._render_bitmaps()
        self._tick_preview()

    # ---- UI build ----
    def _build_ui(self):
        left  = tk.Frame(self.root, bg="#202020")
        right = tk.Frame(self.root, bg="#202020")
        left.pack(side="left", fill="y", padx=10, pady=10)
        right.pack(side="right", fill="both", expand=True, padx=10, pady=10)

        # ----- COLORS -----
        col_box = tk.LabelFrame(left, text="THEME COLORS",
                                fg="white", bg="#202020")
        col_box.pack(fill="x", pady=(0, 10))

        self.swatches = {}
        for label, attr, initial in [
            ("Primary 1 (teal)",   "color_teal",   self.color_teal),
            ("Primary 2 (purple)", "color_purple", self.color_purple),
            ("Primary 3 (lime)",   "color_lime",   self.color_lime),
        ]:
            row = tk.Frame(col_box, bg="#202020")
            row.pack(fill="x", padx=6, pady=3)
            tk.Label(row, text=label, fg="white", bg="#202020", width=18,
                     anchor="w").pack(side="left")
            sw = tk.Frame(row, width=24, height=20, bg=initial,
                          highlightthickness=1, highlightbackground="white")
            sw.pack(side="left", padx=4)
            sw.pack_propagate(False)
            self.swatches[attr] = sw
            tk.Button(row, text="Pick...",
                      command=lambda a=attr: self._pick_color(a)).pack(side="left")

        # ----- TITLE LINES -----
        for i, line in enumerate(self.lines):
            box = tk.LabelFrame(left, text=f"TITLE LINE {i + 1}",
                                fg="white", bg="#202020")
            box.pack(fill="x", pady=4)
            r1 = tk.Frame(box, bg="#202020"); r1.pack(fill="x", padx=6, pady=2)
            tk.Label(r1, text="Text", fg="white", bg="#202020", width=8,
                     anchor="w").pack(side="left")
            tk.Entry(r1, textvariable=line["text"], width=22).pack(side="left")
            line["text"].trace_add("write", lambda *_: self._mark_dirty())
            r2 = tk.Frame(box, bg="#202020"); r2.pack(fill="x", padx=6, pady=2)
            tk.Label(r2, text="Font", fg="white", bg="#202020", width=8,
                     anchor="w").pack(side="left")
            tk.Entry(r2, textvariable=line["ttf"], width=22).pack(side="left")
            tk.Button(r2, text="...", width=2,
                      command=lambda L=line: self._pick_ttf(L)).pack(side="left")
            line["ttf"].trace_add("write", lambda *_: self._mark_dirty())
            r3 = tk.Frame(box, bg="#202020"); r3.pack(fill="x", padx=6, pady=2)
            tk.Label(r3, text="Size", fg="white", bg="#202020", width=8,
                     anchor="w").pack(side="left")
            sp = tk.Spinbox(r3, from_=8, to=60, width=5,
                            textvariable=line["size"])
            sp.pack(side="left")
            line["size"].trace_add("write", lambda *_: self._mark_dirty())

        # ----- URL -----
        u = tk.LabelFrame(left, text="URL / BOTTOM LINE",
                          fg="white", bg="#202020")
        u.pack(fill="x", pady=4)
        tk.Entry(u, textvariable=self.url_var, width=30).pack(padx=6, pady=4)

        # ----- ACTIONS -----
        act = tk.Frame(left, bg="#202020")
        act.pack(fill="x", pady=10)
        tk.Button(act, text="Replay Preview",
                  command=self._restart_preview).pack(fill="x", pady=2)
        tk.Button(act, text="Load Preset...",
                  command=self._load_preset).pack(fill="x", pady=2)
        tk.Button(act, text="Save Preset...",
                  command=self._save_preset).pack(fill="x", pady=2)
        tk.Button(act, text="EXPORT TO FIRMWARE",
                  bg="#5D005D", fg="white", font=("TkDefaultFont", 11, "bold"),
                  command=self._export).pack(fill="x", pady=8)

        self.status = tk.Label(left, text="", fg="lightgreen",
                               bg="#202020", anchor="w", wraplength=300,
                               justify="left")
        self.status.pack(fill="x", pady=2)

        # ----- PREVIEW -----
        tk.Label(right, text="PREVIEW (live)", fg="white", bg="#202020",
                 font=("TkDefaultFont", 11, "bold")).pack(anchor="w")
        self.canvas = tk.Canvas(right, width=W * SCALE, height=H * SCALE,
                                bg="black", highlightthickness=0)
        self.canvas.pack(pady=8)
        self.preview_imgtk = None

    # ---- helpers ----
    def _mark_dirty(self):
        self.bitmaps_dirty = True

    def _pick_color(self, attr):
        cur = getattr(self, attr)
        col = colorchooser.askcolor(color=cur, title="Pick color")
        if col and col[1]:
            setattr(self, attr, col[1].upper())
            self.swatches[attr].configure(bg=col[1])
            self._mark_dirty()

    def _pick_ttf(self, line):
        p = filedialog.askopenfilename(title="Pick TTF font",
                                       filetypes=[("TrueType", "*.ttf *.otf"),
                                                  ("All", "*.*")])
        if p:
            line["ttf"].set(p)
            self._mark_dirty()

    def _render_bitmaps(self):
        if not self.bitmaps_dirty:
            return
        self.bitmaps_cache = {}
        for line in self.lines:
            ttf = line["ttf"].get() or self.default_ttf
            txt = line["text"].get() or " "
            sz  = max(6, int(line["size"].get() or 14))
            try:
                img = render_text_bitmap(ttf, txt, sz)
                self.bitmaps_cache[line["var"]] = img
            except Exception as e:
                self.bitmaps_cache[line["var"]] = None
                self._status(f"Error rendering '{txt}': {e}", error=True)
        self.bitmaps_dirty = False

    def _status(self, msg, error=False):
        self.status.configure(text=msg,
                              fg="#FF6060" if error else "lightgreen")

    # ---- preview loop ----
    def _restart_preview(self):
        self.frame_idx = 0

    def _tick_preview(self):
        self._render_bitmaps()
        shades = derive_shades(hex_to_rgb(self.color_teal),
                               hex_to_rgb(self.color_purple),
                               hex_to_rgb(self.color_lime))
        try:
            frm = render_intro_frame(self.frame_idx, shades,
                                     self.bitmaps_cache, self.url_var.get())
            frm = frm.resize((W * SCALE, H * SCALE), Image.NEAREST)
            self.preview_imgtk = ImageTk.PhotoImage(frm)
            self.canvas.delete("all")
            self.canvas.create_image(0, 0, anchor="nw",
                                     image=self.preview_imgtk)
        except Exception as e:
            self._status(f"Preview error: {e}", error=True)
        self.frame_idx += 1
        # Loop the animation after it finishes
        if self.frame_idx > 200:
            self.frame_idx = 0
        self.root.after(33, self._tick_preview)   # ~30 fps preview

    # ---- preset I/O ----
    def _save_preset(self):
        p = filedialog.asksaveasfilename(defaultextension=".json",
                                         filetypes=[("JSON", "*.json")])
        if not p:
            return
        data = {
            "color_teal":   self.color_teal,
            "color_purple": self.color_purple,
            "color_lime":   self.color_lime,
            "url":          self.url_var.get(),
            "lines": [
                {"var": L["var"], "text": L["text"].get(),
                 "ttf": L["ttf"].get(), "size": L["size"].get()}
                for L in self.lines
            ],
        }
        with open(p, "w") as f:
            json.dump(data, f, indent=2)
        self._status(f"Saved {p}")

    def _load_preset(self):
        p = filedialog.askopenfilename(filetypes=[("JSON", "*.json")])
        if not p:
            return
        with open(p) as f:
            data = json.load(f)
        self.color_teal   = data.get("color_teal",   self.color_teal)
        self.color_purple = data.get("color_purple", self.color_purple)
        self.color_lime   = data.get("color_lime",   self.color_lime)
        self.swatches["color_teal"].configure(bg=self.color_teal)
        self.swatches["color_purple"].configure(bg=self.color_purple)
        self.swatches["color_lime"].configure(bg=self.color_lime)
        self.url_var.set(data.get("url", self.url_var.get()))
        for L, src in zip(self.lines, data.get("lines", [])):
            L["text"].set(src.get("text", L["text"].get()))
            L["ttf"].set(src.get("ttf",  L["ttf"].get()))
            L["size"].set(src.get("size", L["size"].get()))
        self._mark_dirty()
        self._status(f"Loaded {p}")

    # ---- export ----
    def _export(self):
        SRC_DIR.mkdir(parents=True, exist_ok=True)
        self._render_bitmaps()

        # Theme header
        shades = derive_shades(hex_to_rgb(self.color_teal),
                               hex_to_rgb(self.color_purple),
                               hex_to_rgb(self.color_lime))
        theme_path = SRC_DIR / "theme_colors.h"
        write_theme_header(theme_path, shades, self.url_var.get())

        # Bitmap header
        line_data = []
        for L in self.lines:
            bmp = self.bitmaps_cache.get(L["var"])
            if bmp is None:
                continue
            packed, w, h, bpr = pack_bitmap(bmp)
            line_data.append((L["var"], L["text"].get(), packed, w, h, bpr))
        bmp_path = SRC_DIR / "intro_bitmaps.h"
        write_intro_bitmaps_header(bmp_path, line_data)

        self._status(
            f"Exported:\n  {theme_path}\n  {bmp_path}\nRebuild with:  pio run -t upload"
        )


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()

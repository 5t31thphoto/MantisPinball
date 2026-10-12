#!/usr/bin/env python3
"""Headless build configuration — the CI replacement for installer.py's GUI.

installer.py is a tkinter app: it cannot run on a GitHub runner. This module
does the one job CI actually needs from it — turn a plain config.json into
src/build_config.h — with no tkinter import and no GUI.

DRIFT IS THE RISK, so this file does not keep its own copy of the mode list.
It PARSES the canonical tables straight out of installer.py's source text, so
adding an app to the installer automatically makes it available here. There is
no second list to forget to update.

Usage
  python tools/headless_config.py --check          validate tables + config
  python tools/headless_config.py --write          emit src/build_config.h
  python tools/headless_config.py --list           show every available flag
"""
from __future__ import annotations
import argparse, json, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INSTALLER = ROOT / "tools" / "installer.py"
BUILD_CONFIG = ROOT / "src" / "build_config.h"
CONFIG_JSON = ROOT / "config.json"


# ---------------------------------------------------------------------------
# Parse the canonical tables out of installer.py (no tkinter needed)
# ---------------------------------------------------------------------------
def _installer_source() -> str:
    if not INSTALLER.exists():
        sys.exit(f"missing {INSTALLER}")
    return INSTALLER.read_text(encoding="utf-8")


def menu_modes(src: str) -> list[tuple[str, str]]:
    """[(ENABLE_FLAG, human label), ...] in the installer's own order."""
    m = re.search(r"^MENU_MODES\s*=\s*\[(.*?)^\]", src, re.M | re.S)
    if not m:
        sys.exit("could not find MENU_MODES in installer.py")
    return re.findall(r'\(\s*"(ENABLE_\w+)"\s*,\s*"([^"]*)"', m.group(1))


def flag_meta(src: str) -> dict[str, tuple[str, str]]:
    """ENABLE_FLAG -> (STATE_ENUM, on-device menu label)."""
    m = re.search(r"flag_meta = \{(.*?)\n        \}", src, re.S)
    if not m:
        sys.exit("could not find flag_meta in installer.py")
    out = {}
    for flag, state, label in re.findall(
            r'"(ENABLE_\w+)"\s*:\s*\(\s*"(STATE_\w+)"\s*,\s*"([^"]*)"\s*\)', m.group(1)):
        out[flag] = (state, label)
    return out


def guides(src: str) -> list[tuple[int, str]]:
    m = re.search(r"^GUIDES\s*=\s*\[(.*?)^\]", src, re.M | re.S)
    if not m:
        return []
    return [(int(b), n) for b, n in re.findall(r'\(\s*(\d+)\s*,\s*"([^"]*)"', m.group(1))]


def topics(src: str) -> list[tuple[int, str]]:
    m = re.search(r"^RABBIT_TOPICS\s*=\s*\[(.*?)^\]", src, re.M | re.S)
    if not m:
        return []
    return [(int(b), n) for b, n in re.findall(r'\(\s*(\d+)\s*,\s*"([^"]*)"', m.group(1))]


# ---------------------------------------------------------------------------
# Consistency checks — these are what stop silent drift
# ---------------------------------------------------------------------------
def check(src: str) -> int:
    modes = menu_modes(src)
    meta = flag_meta(src)
    mode_flags = {f for f, _ in modes}
    problems, warnings = [], []

    # every toggleable mode must have a state enum + label
    for f in mode_flags:
        if f not in meta:
            problems.append(f"{f} is toggleable but has no flag_meta entry")

    # entries in flag_meta that no longer exist in the firmware are cruft
    main_cpp = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8", errors="replace")
    for f, (state, _label) in meta.items():
        if f not in mode_flags:
            warnings.append(f"{f} is in flag_meta but not toggleable (dead entry)")
        if state not in main_cpp:
            problems.append(f"{f} maps to {state}, which is not in main.cpp")

    # the AppState enum and the registry's A_ enum must stay index-aligned
    reg = (ROOT / "src" / "sdk" / "app_registry.cpp").read_text(encoding="utf-8", errors="replace")
    st = re.findall(r"(STATE_\w+)", re.search(r"enum AppState \{(.*?)\};", main_cpp, re.S).group(1))
    an = re.findall(r"(A_\w+)", re.search(r"enum \{(.*?)\};", reg, re.S).group(1))
    if len(st) != len(an):
        problems.append(f"AppState has {len(st)} entries, A_ enum has {len(an)}")
    else:
        for i, (a, b) in enumerate(zip(st, an)):
            if a != "STATE_" + b[2:]:
                problems.append(f"enum misaligned at index {i}: {a} vs {b}")
                break
    dupes = {x for x in st if st.count(x) > 1}
    if dupes:
        problems.append(f"duplicate AppState entries: {sorted(dupes)}")

    print(f"  modes toggleable : {len(mode_flags)}")
    print(f"  flag_meta entries: {len(meta)}")
    print(f"  AppState entries : {len(st)}  (A_ enum: {len(an)})")
    print(f"  guides           : {len(guides(src))}")
    for w in warnings:
        print(f"  WARN  {w}")
    for p in problems:
        print(f"  FAIL  {p}")
    if problems:
        print(f"\n{len(problems)} problem(s)")
        return 1
    print("\nconfig tables consistent")
    return 0


# ---------------------------------------------------------------------------
# config.json -> build_config.h
# ---------------------------------------------------------------------------
def _mask_bits(text: str, macro: str) -> list[int]:
    m = re.search(rf"^#define {macro} 0x([0-9A-Fa-f]+)u", text, re.M)
    if not m:
        return []
    v = int(m.group(1), 16)
    return [i for i in range(16) if v & (1 << i)]


def _cur_str(text: str, macro: str, dflt: str = "") -> str:
    m = re.search(rf'^#define {macro} "(.*)"', text, re.M)
    return m.group(1) if m else dflt



# ---------------------------------------------------------------------------
# Theme: reuse intro_designer's own derive_shades/write_theme_header so the
# shading math is never duplicated. intro_designer imports tkinter at module
# level (it is also a GUI), so stub tkinter just for this import - CI runners
# have no display and the functions used here never touch it.
# ---------------------------------------------------------------------------
THEME_H = ROOT / "src" / "theme_colors.h"


def _intro_designer():
    import types
    sys.path.insert(0, str(ROOT / "tools"))
    if "tkinter" not in sys.modules:
        for m in ["tkinter", "tkinter.colorchooser", "tkinter.filedialog",
                  "tkinter.messagebox", "tkinter.ttk"]:
            sys.modules[m] = types.ModuleType(m)
    import intro_designer
    return intro_designer


def current_theme():
    t = THEME_H.read_text(encoding="utf-8", errors="replace")
    c = {n: (int(r), int(g), int(b)) for n, r, g, b in
         re.findall(r"(\w+)\s*=\s*rgb565\(\s*(\d+),\s*(\d+),\s*(\d+)\)", t)}
    url = re.search(r'#define INTRO_URL "(.*)"', t)
    hx = lambda v: "#%02x%02x%02x" % v
    return {"teal": hx(c["TEAL"]), "purple": hx(c["PURPLE"]), "lime": hx(c["LIME"]),
            "url": url.group(1) if url else ""}


def apply_theme(cfg) -> bool:
    """Regenerate theme_colors.h only if config.json's theme differs from the
    file - an untouched config never rewrites a working theme."""
    want = cfg.get("theme")
    if not want:
        return False
    cur = current_theme()
    if {k: str(want.get(k, cur[k])).lower() for k in cur} == {k: v.lower() for k, v in cur.items()}:
        return False
    ID = _intro_designer()
    rgb = lambda s: ID.hex_to_rgb(str(s))
    shades = ID.derive_shades(rgb(want.get("teal", cur["teal"])),
                              rgb(want.get("purple", cur["purple"])),
                              rgb(want.get("lime", cur["lime"])))
    ID.write_theme_header(THEME_H, shades, str(want.get("url", cur["url"])))
    return True


def default_config(src: str) -> dict:
    """Snapshot the CURRENT build_config.h so --init round-trips cleanly."""
    cur = ""
    if BUILD_CONFIG.exists():
        with open(BUILD_CONFIG, "r", encoding="utf-8", newline="") as f:
            cur = f.read()
    on = {}
    for flag, _ in menu_modes(src):
        m = re.search(rf"^#define {flag} ([01])", cur, re.M)
        on[flag] = (m.group(1) == "1") if m else True
    tzm = re.search(r"^#define DEFAULT_TZ_OFFSET \(?(-?\d+)\)?", cur, re.M)
    dst = re.search(r"^#define DEFAULT_TZ_DST ([01])", cur, re.M)
    order = []
    meta = flag_meta(src)
    st2f = {st: f for f, (st, _l) in meta.items()}
    blk = re.search(r"#define PINBALL_MENU_ITEMS \\\r?\n((?:.*\\\r?\n)*)", cur)
    if blk:
        for st in re.findall(r"STATE_\w+", blk.group(1)):
            if st in st2f:
                order.append(st2f[st])
    return {
        "theme": current_theme(),
        "menu_order": order,
        "_comment": "Edit via the web configurator or by hand; CI applies this "
                    "to src/build_config.h. Omitted keys are left untouched.",
        "modes": on,
        "guides": _mask_bits(cur, "GUIDE_MASK"),
        "topics": _mask_bits(cur, "RABBIT_TOPIC_MASK"),
        "wifi_ssid": _cur_str(cur, "DEFAULT_WIFI_SSID"),
        "wifi_pass": _cur_str(cur, "DEFAULT_WIFI_PASS"),
        "btc_addr": _cur_str(cur, "DEFAULT_BTC_ADDR"),
        "bch_addr": _cur_str(cur, "DEFAULT_BCH_ADDR"),
        "dgb_addr": _cur_str(cur, "DEFAULT_DGB_ADDR"),
        "btc_pool": _cur_str(cur, "DEFAULT_BTC_POOL"),
        "wx_zip": _cur_str(cur, "DEFAULT_WX_ZIP"),
        "tz_offset_min": int(tzm.group(1)) if tzm else -420,
        "tz_dst": (dst.group(1) == "1") if dst else True,
        "tz_label": _cur_str(cur, "DEFAULT_TZ_LABEL", "MST"),
        "tz_label_dst": _cur_str(cur, "DEFAULT_TZ_LABEL_DST", "MDT"),
    }


def _unused_default(src: str) -> dict:
    return {
        "_comment": "Edit via the web configurator (GitHub Pages) or by hand. "
                    "CI turns this into src/build_config.h.",
        "modes": {f: True for f, _ in menu_modes(src)},
        "guides": [b for b, _ in guides(src)],
        "topics": [b for b, _ in topics(src)],
        "wifi_ssid": "",
        "wifi_pass": "",
        "btc_addr": "",
        "wx_zip": "",
        "tz_offset_min": -420,
        "tz_dst": True,
        "tz_label": "MST",
        "tz_label_dst": "MDT",
    }


def write_build_config(src: str, cfg: dict) -> str:
    """Edit the EXISTING build_config.h in place.

    An earlier version of this rebuilt the file from scratch and silently
    dropped ten real defines (the crypto addresses, the DST fields,
    ENABLE_FIRE/LIGHTNING, PINBALL_MENU_ORDER_DEFINED, RABBIT_TOPIC_MASK),
    which would not have compiled. Substituting into the real file instead
    means anything this tool does not know about is carried through untouched.
    """
    if not BUILD_CONFIG.exists():
        sys.exit("src/build_config.h missing - this tool edits it, it cannot "
                 "create one from nothing")
    with open(BUILD_CONFIG, "r", encoding="utf-8", newline="") as _f:
        text = _f.read()
    meta = flag_meta(src)
    modes = menu_modes(src)
    enabled = cfg.get("modes", {})

    # 1. mode flags: only those the config actually mentions.
    for flag, _ in modes:
        if flag not in enabled:
            continue
        val = 1 if enabled[flag] else 0
        text, n = re.subn(rf"^#define {flag} [01][ \t]*(?=\r?$)",
                          f"#define {flag} {val}", text, count=1, flags=re.M)
        if n == 0:   # flag defined nowhere yet: append beside the others
            text = re.sub(r"(^#define ENABLE_\w+ [01][ \t]*(?=\r?$))",
                          rf"\1\n#define {flag} {val}", text, count=1, flags=re.M)

    # 2. the menu block. Line-based, not regex: replace only the continued
    #    rows after "#define PINBALL_MENU_ITEMS \", and KEEP the terminator line
    #    (the first line without a trailing backslash - currently the fixed
    #    SETUP row, which is not a toggleable mode and must never be dropped).
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)
    try:
        i = next(k for k, l in enumerate(lines) if l.startswith("#define PINBALL_MENU_ITEMS"))
    except StopIteration:
        sys.exit("PINBALL_MENU_ITEMS not found in build_config.h")
    j = i + 1
    while j < len(lines) and lines[j].rstrip().endswith("\\"):
        j += 1
    # ORDER IS USER-VISIBLE (the installer lets you reorder the device menu),
    # so never regenerate in table order. Use config "menu_order" if given,
    # else the order currently in the file; newly enabled modes go last.
    state_to_flag = {st: f for f, (st, _lb) in meta.items()}
    current = []
    for l in lines[i + 1:j]:
        m = re.search(r"STATE_\w+", l)
        if m and m.group(0) in state_to_flag:
            current.append(state_to_flag[m.group(0)])
    order = list(cfg.get("menu_order") or current)
    for flag, _ in modes:
        if flag not in order:
            order.append(flag)
    rows = []
    for flag in order:
        if not enabled.get(flag, True) or flag not in meta:
            continue
        state, label = meta[flag]
        rows.append(f'    {{ "{label}", {state} }}, \\')
    lines[i + 1:j] = rows
    text = nl.join(lines)

    # 3. masks
    if cfg.get("guides"):
        gm = 0
        for b in cfg["guides"]:
            gm |= 1 << int(b)
        text = re.sub(r"^#define GUIDE_MASK 0x[0-9A-Fa-f]+u[ \t]*(?=\r?$)",
                      f"#define GUIDE_MASK 0x{gm:04X}u", text, count=1, flags=re.M)
    if cfg.get("topics"):
        tm = 0
        for b in cfg["topics"]:
            tm |= 1 << int(b)
        text = re.sub(r"^#define RABBIT_TOPIC_MASK 0x[0-9A-Fa-f]+u[ \t]*(?=\r?$)",
                      f"#define RABBIT_TOPIC_MASK 0x{tm:04X}u", text, count=1, flags=re.M)

    # 4. string + numeric defaults, each only if the config supplies it
    strdefs = {"wifi_ssid": "DEFAULT_WIFI_SSID", "wifi_pass": "DEFAULT_WIFI_PASS",
               "btc_addr": "DEFAULT_BTC_ADDR",  "bch_addr": "DEFAULT_BCH_ADDR",
               "dgb_addr": "DEFAULT_DGB_ADDR",  "btc_pool": "DEFAULT_BTC_POOL",
               "wx_zip": "DEFAULT_WX_ZIP",      "tz_label": "DEFAULT_TZ_LABEL",
               "tz_label_dst": "DEFAULT_TZ_LABEL_DST"}
    for key, macro in strdefs.items():
        if key in cfg:
            val = str(cfg[key]).replace("\\", "").replace('"', "")
            text = re.sub(rf'^#define {macro} ".*"[ \t]*(?=\r?$)',
                          f'#define {macro} "{val}"', text, count=1, flags=re.M)
    if "tz_offset_min" in cfg:
        text = re.sub(r"^#define DEFAULT_TZ_OFFSET \(?-?\d+\)?[ \t]*(?=\r?$)",
                      f"#define DEFAULT_TZ_OFFSET ({int(cfg['tz_offset_min'])})",
                      text, count=1, flags=re.M)
    if "tz_dst" in cfg:
        text = re.sub(r"^#define DEFAULT_TZ_DST [01][ \t]*(?=\r?$)",
                      f"#define DEFAULT_TZ_DST {1 if cfg['tz_dst'] else 0}",
                      text, count=1, flags=re.M)
    return text


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--init", action="store_true", help="write a default config.json")
    ap.add_argument("--config", default=str(CONFIG_JSON))
    a = ap.parse_args()
    src = _installer_source()

    if a.list:
        for f, label in menu_modes(src):
            st = flag_meta(src).get(f, ("?", "?"))
            print(f"  {f:<24} {label:<28} -> {st[0]}")
        return 0
    if a.init:
        Path(a.config).write_text(json.dumps(default_config(src), indent=2) + "\n")
        print(f"wrote {a.config}")
        return 0
    if a.check:
        return check(src)
    if a.write:
        p = Path(a.config)
        cfg = json.loads(p.read_text()) if p.exists() else default_config(src)
        out = write_build_config(src, cfg)
        with open(BUILD_CONFIG, "w", encoding="utf-8", newline="") as _f:
            _f.write(out)
        on = sum(1 for f, _ in menu_modes(src) if cfg.get("modes", {}).get(f, True))
        print(f"wrote {BUILD_CONFIG}  ({on} modes enabled)")
        if apply_theme(cfg):
            print(f"wrote {THEME_H}  (theme changed in config.json)")
        return 0
    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())

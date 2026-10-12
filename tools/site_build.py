#!/usr/bin/env python3
"""Stage the website after a firmware build (run by CI, also works locally).

Writes into website/:
  firmware/<part>.bin      bootloader, partitions, boot_app0, firmware (app)
  firmware/content.bin     LittleFS image of data/ (guides, packs, art)
  firmware/firmware-merged.bin   one-file download (no content partition)
  firmware/build.json      every part + offset, version, enabled modes
  js/theme.json            the device's real theme colors, for the splash preview

Offsets are never hardcoded: app parts come from merge_bin.py's parts.json
(PlatformIO's own upload list) and the content offset from the partition CSV.
"""
import csv, json, os, re, shutil, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SITE = ROOT / "website"
FW = SITE / "firmware"
ENV = "lilygo-t-display-s3"


def partition_offset(name):
    ini = (ROOT / "platformio.ini").read_text()
    m = re.search(r"board_build\.partitions\s*=\s*(\S+)", ini)
    rows = [r for r in csv.reader(l for l in (ROOT / m.group(1)).read_text().splitlines()
                                  if l.strip() and not l.lstrip().startswith("#"))]
    for r in rows:
        if r[0].strip() == name:
            return int(r[3].strip(), 16), int(r[4].strip(), 16)
    sys.exit(f"partition '{name}' not found")


def theme():
    t = (ROOT / "src" / "theme_colors.h").read_text(encoding="utf-8", errors="replace")
    out = {}
    for name, r, g, b in re.findall(r"(\w+)\s*=\s*rgb565\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)", t):
        # what the panel actually shows: quantized to 5/6/5 bits
        r, g, b = int(r) & 0xF8, int(g) & 0xFC, int(b) & 0xF8
        out[name] = f"#{r:02x}{g:02x}{b:02x}"
    url = re.search(r'#define INTRO_URL "(.*)"', t)
    out["_INTRO_URL"] = url.group(1) if url else ""
    return out


def modes():
    bc = (ROOT / "src" / "build_config.h").read_text(encoding="utf-8", errors="replace")
    blk = re.search(r"#define PINBALL_MENU_ITEMS \\\r?\n((?:.*\r?\n)*?.*STATE_SETUP.*)", bc)
    return re.findall(r'\{\s*"([^"]+)"', blk.group(1)) if blk else []


def main():
    FW.mkdir(parents=True, exist_ok=True)
    (SITE / "js").mkdir(parents=True, exist_ok=True)
    (SITE / "js" / "theme.json").write_text(json.dumps(theme(), indent=1))

    parts_dir = ROOT / "flash_parts"
    build = {"version": os.environ.get("BUILD_VERSION", "local"),
             "commit": os.environ.get("GITHUB_SHA", "")[:7],
             "built": time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime()),
             "chipFamily": "ESP32-S3", "parts": [], "content": None, "modes": modes()}

    pj = parts_dir / "parts.json"
    if pj.exists():
        for p in json.loads(pj.read_text()):
            shutil.copyfile(parts_dir / p["path"], FW / p["path"])
            build["parts"].append(p)
    else:
        print("WARN no flash_parts/parts.json (firmware not built) - site staged without firmware")

    lfs = ROOT / ".pio" / "build" / ENV / "littlefs.bin"
    off, size = partition_offset("content")
    if lfs.exists():
        if lfs.stat().st_size > size:
            sys.exit(f"content image {lfs.stat().st_size} > partition {size}")
        shutil.copyfile(lfs, FW / "content.bin")
        build["content"] = {"path": "content.bin", "offset": off}
    else:
        print("WARN no littlefs.bin - content partition not offered")

    merged = ROOT / "firmware_merged.bin"
    if merged.exists():
        shutil.copyfile(merged, FW / "firmware-merged.bin")

    (FW / "build.json").write_text(json.dumps(build, indent=2))
    print(f"staged: {len(build['parts'])} parts, content={'yes' if build['content'] else 'no'}, "
          f"{len(build['modes'])} modes, theme {len(theme())-1} colors")


if __name__ == "__main__":
    main()

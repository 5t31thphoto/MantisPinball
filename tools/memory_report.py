#!/usr/bin/env python3
"""memory_report.py - Compute the REAL memory budget for a new app.

Three sources of truth, in priority order:
  1. A PlatformIO build's reported RAM/Flash usage (compile-time static RAM).
  2. The device's serial capability manifest (runtime free heap + largest block).
  3. Known firmware constants (frame buffer sprite size, shared buffers).

It runs `pio run` (build only, no upload) for the CURRENT build_config, parses
the "RAM:" / "Flash:" lines, and combines them with the device's reported free
heap to produce the headroom a new app can safely use. Output is JSON the
prompt generator and installer consume.

Usage:
  python3 tools/memory_report.py            # build + parse, write tools/mem_report.json
  python3 tools/memory_report.py --no-build  # parse last build / use constants only
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tools", "mem_report.json")

# Hard facts about the T-Display-S3 firmware (from the build history).
ESP32S3_SRAM = 327680          # ~320KB usable internal SRAM (no PSRAM)
FB_SPRITE_BYTES = 170 * 320 * 2  # 16bpp frame buffer sprite, heap-allocated (~108KB)
SHARED_JPEG_BYTES = 61440      # g_jpegBuf, allocated on-demand by image apps
TLS_HANDSHAKE_BYTES = 40000    # WiFiClientSecure needs ~40KB transient heap

PIO_RAM_RE = re.compile(r"RAM:\s*\[[=\s]*\]\s*([\d.]+)%\s*\(used\s+(\d+)\s+bytes\s+from\s+(\d+)\s+bytes\)")
PIO_FLASH_RE = re.compile(r"Flash:\s*\[[=\s]*\]\s*([\d.]+)%\s*\(used\s+(\d+)\s+bytes\s+from\s+(\d+)\s+bytes\)")


def find_pio():
    for cand in ("pio", "platformio"):
        try:
            subprocess.run([cand, "--version"], capture_output=True, check=True)
            return [cand]
        except Exception:
            pass
    # PlatformIO installed as a python module
    return [sys.executable, "-m", "platformio"]


def run_build():
    pio = find_pio()
    try:
        r = subprocess.run(pio + ["run"], cwd=ROOT, capture_output=True, text=True)
        return r.stdout + "\n" + r.stderr
    except Exception as e:
        return f"(build failed to launch: {e})"


def parse_usage(text):
    ram = PIO_RAM_RE.search(text)
    flash = PIO_FLASH_RE.search(text)
    out = {}
    if ram:
        out["ramUsedBytes"] = int(ram.group(2))
        out["ramTotalBytes"] = int(ram.group(3))
        out["ramFreeBytes"] = int(ram.group(3)) - int(ram.group(2))
    if flash:
        out["flashUsedBytes"] = int(flash.group(2))
        out["flashTotalBytes"] = int(flash.group(3))
    return out


def load_device_manifest():
    """If the installer captured a serial manifest, use its live heap numbers."""
    p = os.path.join(ROOT, "tools", "device_manifest.json")
    if os.path.exists(p):
        try:
            return json.load(open(p, encoding="utf-8")).get("mem", {})
        except Exception:
            return {}
    return {}


def build_report(do_build=True):
    rep = {
        "schema": 1,
        "constants": {
            "sram_total": ESP32S3_SRAM,
            "frame_buffer_sprite": FB_SPRITE_BYTES,
            "shared_jpeg_buffer": SHARED_JPEG_BYTES,
            "tls_handshake_transient": TLS_HANDSHAKE_BYTES,
            "psram": False,
        },
        "static": {},
        "device": load_device_manifest(),
        "notes": [],
    }
    if do_build:
        text = run_build()
        usage = parse_usage(text)
        rep["static"] = usage
        if not usage:
            rep["notes"].append("Could not parse PlatformIO RAM/Flash output; "
                                "using constants only.")
    else:
        rep["notes"].append("Build skipped (--no-build); using constants/device only.")

    # Compute a conservative heap budget for a NEW app.
    # Prefer live device free heap; else estimate from static RAM usage.
    free_heap = rep["device"].get("freeHeapAtBoot")
    largest = rep["device"].get("largestFreeBlock")
    if free_heap is None and "ramFreeBytes" in rep["static"]:
        # Static free RAM minus the heap-allocated frame buffer that comes up at boot.
        free_heap = rep["static"]["ramFreeBytes"] - FB_SPRITE_BYTES
        rep["notes"].append("freeHeap estimated from static RAM minus frame buffer; "
                            "connect the device for exact runtime numbers.")
    if free_heap is None:
        free_heap = ESP32S3_SRAM - rep["static"].get("ramUsedBytes", 180000) - FB_SPRITE_BYTES
        rep["notes"].append("freeHeap is a rough fallback estimate.")

    # An app that does TLS networking must leave room for the handshake.
    safe_static = max(0, free_heap - TLS_HANDSHAKE_BYTES - 16000)  # 16KB safety margin
    safe_nonnet = max(0, free_heap - 24000)                        # smaller margin, no TLS

    rep["budget"] = {
        "estFreeHeapBytes": int(free_heap),
        "largestFreeBlockBytes": int(largest) if largest else None,
        "safeAppAllocIfNetworking": int(safe_static),
        "safeAppAllocNoNetworking": int(safe_nonnet),
        "guidance": (
            "Stay well under safeAppAllocIfNetworking if your app opens a TLS "
            "connection (weather/space/etc). Big buffers should be malloc'd on "
            "Enter and freed on Shutdown, never left in static BSS. The image "
            "decode buffer (g_jpegBuf, 60KB) is shared and on-demand."
        ),
    }
    return rep


def main():
    do_build = "--no-build" not in sys.argv
    rep = build_report(do_build)
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(rep, f, indent=2)
    b = rep["budget"]
    print(f"wrote {OUT}")
    print(f"  est free heap: {b['estFreeHeapBytes']} bytes")
    print(f"  safe app alloc (networking): {b['safeAppAllocIfNetworking']} bytes")
    print(f"  safe app alloc (no network): {b['safeAppAllocNoNetworking']} bytes")
    for n in rep["notes"]:
        print(f"  note: {n}")


if __name__ == "__main__":
    main()

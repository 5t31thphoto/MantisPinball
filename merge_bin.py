"""Post-build: stage the web-flasher parts and a merged image (ESP32-S3).

Adapted from the Synapse merge script, with the one difference that matters:
the ESP32-S3 bootloader lives at 0x0, not 0x1000 like the classic ESP32 /
Core2. Copying the Synapse offsets verbatim would produce an image that does
not boot. So this script does not hardcode offsets at all - it reads
FLASH_EXTRA_IMAGES, the same list PlatformIO uses for `pio run -t upload`,
and only falls back to the documented S3 layout if that list is missing.

Outputs (project root):
  firmware_merged.bin   bootloader + partitions + boot_app0 + app, from 0x0
  flash_parts/          each part separately + parts.json (offsets) for the
                        web flasher, so it can patch the app before flashing
"""
Import("env")
import json, os, shutil
from os.path import join, isfile, basename

S3_FALLBACK = [("0x0", "bootloader.bin"), ("0x8000", "partitions.bin"),
               ("0xe000", "boot_app0.bin")]


def _parts(env):
    build = env.subst("$BUILD_DIR")
    parts = []
    for off, path in env.get("FLASH_EXTRA_IMAGES", []):
        parts.append((env.subst(off), env.subst(path)))
    if not parts:
        print("[merge] FLASH_EXTRA_IMAGES empty - using documented S3 layout")
        for off, name in S3_FALLBACK:
            p = join(build, name)
            if name == "boot_app0.bin" and not isfile(p):
                p = join(env.subst("$PROJECT_PACKAGES_DIR"), "framework-arduinoespressif32",
                         "tools", "partitions", "boot_app0.bin")
            parts.append((off, p))
    app_off = env.subst("$ESP32_APP_OFFSET") or "0x10000"
    parts.append((app_off, join(build, "firmware.bin")))
    return parts


def after_build(source, target, env):
    try:
        proj = env.subst("$PROJECT_DIR")
        parts = _parts(env)
        missing = [p for _, p in parts if not isfile(p)]
        if missing:
            print("[merge] skip - missing:", missing)
            return
        boot_off = int(parts[0][0], 16)
        if boot_off != 0x0:
            print(f"[merge] WARNING bootloader offset {hex(boot_off)}; S3 expects 0x0")

        stage = join(proj, "flash_parts")
        shutil.rmtree(stage, ignore_errors=True)
        os.makedirs(stage)
        manifest = []
        for off, path in parts:
            shutil.copyfile(path, join(stage, basename(path)))
            manifest.append({"path": basename(path), "offset": int(off, 16)})
        with open(join(stage, "parts.json"), "w") as f:
            json.dump(manifest, f, indent=2)

        out = join(proj, "firmware_merged.bin")
        # flash mode/freq/size default to "keep": the bootloader header that
        # PlatformIO built for this board is left exactly as it is.
        cmd = [env.subst("$PYTHONEXE"), "-m", "esptool", "--chip", "esp32s3",
               "merge_bin", "-o", out]
        for off, path in parts:
            cmd += [off, path]
        print("[merge]", " ".join(cmd))
        env.Execute(" ".join('"%s"' % c if " " in str(c) else str(c) for c in cmd))
        if isfile(out):
            print("[merge] OK", out, os.path.getsize(out))
    except Exception as e:                     # never fail the firmware build
        print("[merge] non-fatal:", e)


env.AddPostAction("$BUILD_DIR/firmware.bin", after_build)

# Pinball-S3 Installer GUI

A one-stop graphical installer for flashing the LilyGo T-Display-S3 with your
chosen modes, topics, colors, intro screen, and default config.

## Setup (one time)

```
pip install pillow pyserial platformio
```

- **pillow** - renders the intro bitmaps + live preview
- **pyserial** - auto-detects the board's COM port
- **platformio** - the build/upload backend (the `pio` command)

## Run

```
cd tools
python installer.py
```

## What it does

1. **Device connection** - plug in the T-Display-S3 over USB. The dot turns
   green and shows the COM port when an ESP32-S3 is detected. (Click *Rescan*
   if you plug in after launching.)
2. **Theme colors** - pick your three primaries. Every shade on the device is
   derived from these, and the preview updates live.
3. **Intro screen** - set the three title lines, fonts, sizes, and URL.
4. **Menu modes** - check which modes to include in the build. Unchecked modes
   are compiled out entirely (smaller binary, cleaner menu).
5. **Rabbit hole topics** - check which topics to include. The device only
   cycles through the ones you pick.
6. **Default config (optional)** - pre-load WiFi, BTC wallet, and zip. These
   are seeded into the device on first boot *only if* that field hasn't been
   set up on-device yet, so they never clobber a user's own setup.
7. **Preview** - toggle between the animated intro and a mock of the menu, both
   in your chosen colors.
8. **Build & Upload** - writes the three generated headers
   (`build_config.h`, `theme_colors.h`, `intro_bitmaps.h`), then runs
   `pio run --target upload`, streaming the log on the right. A cute success
   burst pops up when it's done.

## Notes

- "Write config files only" generates the headers without uploading, handy if
  you want to build from PowerShell yourself afterward.
- If PlatformIO isn't found, install it with `pip install platformio` and make
  sure `pio --version` works in your terminal.

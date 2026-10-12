# Mantis Pinball on GitHub - setup

## First time
1. Create a **public** repo (GitHub Pages on a free account needs public).
2. Unzip this into the repo root.
3. Rename `github/` to `.github/` (it ships without the dot so it unzips clean).
4. Settings -> Pages -> Source: **GitHub Actions**.
5. Commit and push. The Actions tab builds firmware + content and deploys the
   site. The install page is at `https://<you>.github.io/<repo>/`.

## After that - zip drop (same as Synapse)
Drop a payload `.zip` in the repo root and push. CI unpacks it over the repo,
deletes the zip, commits `[skip ci]`, then builds and deploys. A payload's
`src/` replaces `src/` entirely. Workflow folders inside a payload are skipped
(the Actions token may not push workflow changes) - update those by hand.

## Choosing modes / defaults
Edit `config.json` and commit. CI turns it into `src/build_config.h`
(`tools/headless_config.py`). `menu_order` is the on-device menu order.
WiFi SSID/password are blank on purpose: the site and source are public.
Set WiFi on the device with SETUP instead.

## What CI checks before it builds
- config tables consistent, AppState/registry enums aligned (no duplicates)
- splash patcher: synthetic image patch + independent checksum/SHA-256 verify
- splash patcher identity: untouched designer changes zero bytes
- mantis psychic effect validator

## Files
- `github/workflows/build.yml`  the pipeline
- `merge_bin.py`  ESP32-S3 post-build: parts + merged image (bootloader @ 0x0)
- `tools/site_build.py`  stages firmware parts, content image, theme, mode list
- `website/`  the install site (ESP Web Tools + splash designer)
- `tools/installer.py`  the old tkinter installer, still works locally

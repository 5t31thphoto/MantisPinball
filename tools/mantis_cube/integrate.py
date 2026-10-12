"""
MantisCubeSolver — installer integration.

Wires the solver into installer.py. Three edits are required and the panel is
only one of them; without the other two the app never reaches the device menu:

  1. MENU_MODES        so the mode gets a checkbox and a build flag
  2. flag_meta         so _write_build_config() emits its PINBALL_MENU_ITEMS
                       entry, which is what main.cpp expands into the menu
  3. _build_ui()       so the panel appears in the left control rail

Idempotent: running twice is a no-op. Makes a .bak first.

    python -m mantis_cube.integrate /path/to/tools/installer.py
    python -m mantis_cube.integrate /path/to/tools/installer.py --check
    python -m mantis_cube.integrate /path/to/tools/installer.py --revert
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

MODE_FLAG = "ENABLE_RUBIKS"
MODE_LABEL = "Rubik Solver"
STATE_ENUM = "STATE_RUBIKS"
DEVICE_LABEL = "RUBIK SOLVER"

MARK = "# --- MantisCubeSolver ---"


EDITS = [
    # (name, anchor, insertion, already-present test)
    (
        "MENU_MODES entry",
        '    ("ENABLE_HAM",     "Ham Radio"),\n',
        '    ("ENABLE_HAM",     "Ham Radio"),\n'
        f'    ("{MODE_FLAG}",  "{MODE_LABEL}"),   {MARK}\n',
        MODE_FLAG,
    ),
    (
        "flag_meta entry",
        '            "ENABLE_HAM":     ("STATE_HAM",     "HAM RADIO"),\n',
        '            "ENABLE_HAM":     ("STATE_HAM",     "HAM RADIO"),\n'
        f'            "{MODE_FLAG}":  ("{STATE_ENUM}",  "{DEVICE_LABEL}"),'
        f'   {MARK}\n',
        f'"{MODE_FLAG}":',
    ),
    (
        "panel mount",
        "        self._build_sdk_panel()\n",
        "        self._build_sdk_panel()\n"
        f"        self._build_cube_panel()          {MARK}\n",
        "_build_cube_panel()",
    ),
]

PANEL_METHOD = f'''
    # ------------------------------------------------------ {MARK[6:-4]}
    def _build_cube_panel(self):
        """Mount the cube solver build panel.

        Kept in a try/except so a missing or broken mantis_cube package
        degrades to a one-line notice instead of preventing the installer
        from opening - the rest of the app does not depend on it.
        """
        try:
            from mantis_cube.panel import CubeSolverPanel
        except Exception as exc:
            box = self._section("CUBE SOLVER")
            tk.Label(box, text=f"mantis_cube unavailable: {{exc}}",
                     fg="#caa", bg=DARK, font=("TkDefaultFont", 8),
                     wraplength=360, justify="left").pack(anchor="w",
                                                          padx=6, pady=4)
            return
        box = self._section("CUBE SOLVER")
        self.cube_panel = CubeSolverPanel(
            box,
            log=self._log,
            root=self.root,
            out_dir=SRC_DIR / "rubiks",
            theme={{"DARK": DARK, "PANEL": PANEL,
                   "ACCENT": ACCENT, "TEXT": TEXT}},
        )
'''

METHOD_ANCHOR = "    def _sdk_manifest_path(self):\n"


def check(path: Path) -> list:
    src = path.read_text(encoding="utf-8")
    missing = []
    for name, _anchor, _ins, probe in EDITS:
        if probe not in src:
            missing.append(name)
    if "_build_cube_panel" not in src or "def _build_cube_panel" not in src:
        if "panel method" not in missing:
            missing.append("panel method")
    return missing


def verify_consistency(src: str, log=print) -> None:
    """Every MENU_MODES flag needs a flag_meta entry.

    _write_build_config() indexes flag_meta by flag, so a missing entry is a
    KeyError at upload time - after the user has already configured
    everything. Checked here rather than discovered then.
    """
    import re
    menu = re.search(r"MENU_MODES = \[(.*?)\n\]", src, re.S)
    meta = re.search(r"flag_meta = \{(.*?)\n        \}", src, re.S)
    if not menu or not meta:
        log("  (could not locate MENU_MODES / flag_meta to cross-check)")
        return
    flags = re.findall(r'\("(\w+)"', menu.group(1))
    keys = set(re.findall(r'"(\w+)":', meta.group(1)))
    missing = [f for f in flags if f not in keys]
    if missing:
        raise SystemExit(
            "these MENU_MODES flags have no flag_meta entry and would raise "
            "KeyError during upload:\n  " + "\n  ".join(missing))
    log(f"  consistency: {len(flags)} modes, all mapped")


def apply(path: Path, log=print) -> bool:
    src = path.read_text(encoding="utf-8")
    original = src
    applied = []

    for name, anchor, insertion, probe in EDITS:
        if probe in src:
            log(f"  {name}: already present")
            continue
        if anchor not in src:
            raise SystemExit(
                f"could not find the anchor for {name}.\n"
                f"Expected to find this line:\n  {anchor.strip()}\n"
                f"Your installer.py may have diverged; apply this edit by hand.")
        src = src.replace(anchor, insertion, 1)
        applied.append(name)
        log(f"  {name}: added")

    if "def _build_cube_panel" not in src:
        if METHOD_ANCHOR not in src:
            raise SystemExit(
                "could not find the anchor for the panel method "
                "(_sdk_manifest_path). Apply by hand.")
        src = src.replace(METHOD_ANCHOR, PANEL_METHOD + "\n" + METHOD_ANCHOR, 1)
        applied.append("panel method")
        log("  panel method: added")

    if src == original:
        log("nothing to do - installer already integrated")
        return False

    verify_consistency(src, log=log)

    backup = path.with_suffix(path.suffix + ".bak")
    shutil.copy2(path, backup)
    path.write_text(src, encoding="utf-8")
    log(f"\npatched {path}")
    log(f"backup at {backup}")
    log("\nStill required on the firmware side:")
    log(f"  * define {STATE_ENUM} in the mode enum")
    log(f"  * guard the mode registration with #if {MODE_FLAG}")
    log("  * add src/rubiks/*.cpp to the build")
    return True


def revert(path: Path, log=print) -> None:
    backup = path.with_suffix(path.suffix + ".bak")
    if not backup.exists():
        raise SystemExit(f"no backup at {backup}")
    shutil.copy2(backup, path)
    log(f"restored {path} from {backup}")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mantis_cube.integrate")
    ap.add_argument("installer", type=Path)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--revert", action="store_true")
    args = ap.parse_args(argv)

    if not args.installer.exists():
        raise SystemExit(f"not found: {args.installer}")

    if args.revert:
        revert(args.installer)
        return
    if args.check:
        missing = check(args.installer)
        if missing:
            print("NOT integrated. Missing:")
            for m in missing:
                print(f"  - {m}")
            sys.exit(1)
        print("installer is fully integrated")
        return

    print(f"Integrating cube solver into {args.installer}")
    apply(args.installer)


if __name__ == "__main__":
    main()

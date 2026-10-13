#!/usr/bin/env python3
# ============================================================================
#  installer.py - Pinball-S3 one-stop Installer GUI
#
#  Open the app, plug in the T-Display-S3, and:
#    1. See the board auto-detected on its COM port (live indicator)
#    2. Customize the intro screen + theme colors (reuses intro_designer logic)
#    3. Check which menu modes to include in the build
#    4. Check which rabbit-hole topics to include
#    5. Optionally enter default WiFi / wallet / zip config
#    6. Live device-GUI preview that reflects your colors + enabled modes
#    7. Hit UPLOAD -> watch progress + a live build log -> cute success screen
#
#  On upload it writes:
#    src/build_config.h    (modes + topics + default config)
#    src/theme_colors.h    (14 derived shades + INTRO_URL)
#    src/intro_bitmaps.h   (packed title bitmaps)
#  then runs `pio run --target upload` and streams the output.
#
#  Build isolation:
#    The installer can create a project-local Python venv (.installer-venv) with
#    a PINNED PlatformIO, and pins PlatformIO's core dir to .pio-core via
#    PLATFORMIO_CORE_DIR. This keeps this project's Arduino-core / toolchain
#    versions from colliding with unrelated PlatformIO projects on the same
#    machine. Click "Set up isolated build env" in the UI (recommended).
#
#  Requirements:  pip install pillow pyserial
#                 (PlatformIO is installed automatically into the isolated env;
#                  a system PlatformIO is used only as a fallback)
# ============================================================================

import os
import sys
import glob
import json
import queue
import threading
import subprocess
import tkinter as tk
from pathlib import Path
from tkinter import colorchooser, filedialog, messagebox, simpledialog, ttk

try:
    from PIL import Image, ImageTk
except ImportError:
    sys.stderr.write("Pillow is required.  pip install pillow\n")
    sys.exit(1)

# Reuse the proven logic from the intro designer (same folder)
sys.path.insert(0, str(Path(__file__).resolve().parent))
import intro_designer as ID  # noqa: E402

# pyserial is optional - the app still runs without it (just no auto-detect)
try:
    import serial.tools.list_ports as list_ports
    HAVE_SERIAL = True
except ImportError:
    HAVE_SERIAL = False

ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "src"

# ============================================================================
#  Project isolation
# ----------------------------------------------------------------------------
#  To keep this project's PlatformIO toolchain from colliding with unrelated
#  PlatformIO projects (different Arduino-core / platform versions in the shared
#  ~/.platformio), we run everything against:
#    * a project-local Python venv  (ROOT/.installer-venv) with a pinned
#      PlatformIO installed into it, and
#    * a project-local PlatformIO core dir (ROOT/.pio-core) via
#      PLATFORMIO_CORE_DIR, so platforms/packages/toolchains install HERE and
#      never touch (or get mutated by) the global ~/.platformio.
#  This is off by default's opposite: it's the recommended, safe path. The user
#  can still fall back to a system pio if they insist.
# ============================================================================
PINNED_PIO = "platformio==6.1.15"          # matches espressif32@6.5.0 toolchain
VENV_DIR   = ROOT / ".installer-venv"
CORE_DIR   = ROOT / ".pio-core"            # isolated PLATFORMIO_CORE_DIR

def _venv_python():
    if os.name == "nt":
        return VENV_DIR / "Scripts" / "python.exe"
    return VENV_DIR / "bin" / "python"

def _venv_pio():
    if os.name == "nt":
        return VENV_DIR / "Scripts" / "platformio.exe"
    return VENV_DIR / "bin" / "platformio"

def isolated_env():
    """Environment that pins PlatformIO's core dir into this project."""
    env = dict(os.environ)
    env["PLATFORMIO_CORE_DIR"] = str(CORE_DIR)
    return env


W, H = 170, 320
PREVIEW_SCALE = 2

# ---- ESP32-S3 USB identifiers --------------------------------------------------
# The T-Display-S3 uses the native USB-CDC of the ESP32-S3 (VID 0x303A) or,
# on some units, a CH9102/CP210x bridge. We match any of these.
ESP_VIDS = {0x303A, 0x1A86, 0x10C4, 0x0403}

# ---- Menu modes: (build flag, on-device label) ---------------------------------
# Order mirrors the firmware default menu exactly. Lightning + Wildfires are
# "Setup" is always present in-game and isn't a toggle.
MENU_MODES = [
    ("ENABLE_PINBALL", "Pinball Blaster"),
    ("ENABLE_POCKET",  "Corner Pocket"),
    ("ENABLE_SLOTS",   "Jacks or Better"),
    ("ENABLE_VERSUS",  "Versus Pinball"),
    ("ENABLE_SHARE",   "Pinball Sharing (AP)"),
    ("ENABLE_PARTY",   "Party Game (charades)"),
    ("ENABLE_MANTISCHAT", "Mantis Chat (ESP-NOW mesh)"),
    ("ENABLE_DANCEPARTY", "Dance Party (dances with Synapse)"),
    ("ENABLE_QUESTS",  "Quests (to-do RPG)"),
    ("ENABLE_PET",     "Mantis Pet"),
    ("ENABLE_CYOA",    "Story Mode (CYOA)"),
    ("ENABLE_MANTISPSYCHIC", "Mantis Psychic (mentalism)"),
    ("ENABLE_ROADHUNT", "Road Hunt (trip bingo)"),
    ("ENABLE_SIGHTINGS", "Sightings (life list)"),
    ("ENABLE_MANTISCANNON", "Mantis Cannon"),
    ("ENABLE_MANTISSKATE", "Mantis Skate"),
    ("ENABLE_MANTISFISH", "Mantis Fishing"),
    ("ENABLE_DUNERIDER", "Dune Rider"),
    ("ENABLE_MANTISPUTT", "Mantis Putt (minigolf)"),
    ("ENABLE_MANTISDROP", "Mantis Drop (stacker)"),
    ("ENABLE_MANTISVADERS", "Mantis Vaders (pinball invaders)"),
    ("ENABLE_AQUARIUM","Aquarium Sim"),
    ("ENABLE_FIREWORKS", "Fireworks (stim)"),
    ("ENABLE_FRUIT",   "Fruit Frenzy (gesture sensor)"),
    ("ENABLE_DICE",    "Dice Roller"),
    ("ENABLE_CLICKER", "Click Counter"),
    ("ENABLE_RUBIKS", "Cube Solver (Rubik's)"),
    ("ENABLE_MOON",    "Moon Phase"),
    ("ENABLE_GUIDEBOOKS","GuideBooks (all offline guides)"),
    ("ENABLE_RABBIT",  "Rabbit Hole"),
    ("ENABLE_WEATHER", "Weather Report"),
    ("ENABLE_SPACE",   "Space Weather"),
    ("ENABLE_FLIGHT",  "Flights Overhead"),
    ("ENABLE_NEO",     "Asteroids (NEO)"),
    ("ENABLE_HAM",     "Ham Radio"),
    ("ENABLE_CANDLES", "Crypto Candles"),
    ("ENABLE_MINER",   "BTC Miner"),
]

# ---- Timezones: (label, base UTC offset minutes, US-DST?, std label, dst label)
# Covers common zones; the firmware applies US DST rules when the DST flag is 1.
TIMEZONES = [
    ("Hawaii (UTC-10)",            -600, 0, "HST", "HST"),
    ("Alaska (UTC-9)",             -540, 1, "AKST", "AKDT"),
    ("Pacific (UTC-8)",            -480, 1, "PST", "PDT"),
    ("Mountain (UTC-7)",           -420, 1, "MST", "MDT"),
    ("Arizona (UTC-7, no DST)",    -420, 0, "MST", "MST"),
    ("Central (UTC-6)",            -360, 1, "CST", "CDT"),
    ("Eastern (UTC-5)",            -300, 1, "EST", "EDT"),
    ("Atlantic (UTC-4)",           -240, 1, "AST", "ADT"),
    ("UTC / GMT",                     0, 0, "UTC", "UTC"),
    ("London (UTC+0)",                0, 1, "GMT", "BST"),
    ("Central Europe (UTC+1)",       60, 1, "CET", "CEST"),
    ("Eastern Europe (UTC+2)",      120, 1, "EET", "EEST"),
    ("India (UTC+5:30)",            330, 0, "IST", "IST"),
    ("China (UTC+8)",               480, 0, "CST", "CST"),
    ("Japan (UTC+9)",               540, 0, "JST", "JST"),
    ("Sydney (UTC+10)",             600, 0, "AEST", "AEDT"),
]

GUIDES = [
    (0, "Scout Skills"),
    (1, "Survival Guide"),
    (2, "First Aid"),
    (3, "Campfire Cooking"),
    (4, "Dutch Oven"),
    (5, "Fishing"),
    (6, "Gold & Rocks"),
    (7, "Foraging"),
    (8, "Orienteering"),
]

RABBIT_TOPICS = [
    (0,  "Hacker News"),
    (1,  "Retro Gaming"),
    (2,  "Speculative Cosmology"),
    (3,  "Arduino Projects"),
    (4,  "Handheld Gaming"),
    (5,  "Cyber Devices"),
    (6,  "Crypto Tech"),
    (7,  "Science Papers"),
    (8,  "Weird History"),
    (9,  "Retro Computing"),
]

DARK   = "#1b1b1f"
PANEL  = "#26262c"
ACCENT = "#5D005D"
TEXT   = "#e8e8e8"
SUBTLE = "#8a8a92"


# ============================================================================
def bootstrap_venv(log=None):
    """Create the project-local venv and install the pinned PlatformIO into it.
    Returns the path to the venv's platformio, or None on failure. Idempotent:
    if the venv already has the pinned pio, it's a no-op."""
    def _say(m):
        if log: log(m)
    pio = _venv_pio()
    py  = _venv_python()
    # Already bootstrapped?
    if pio.exists():
        return pio
    try:
        import venv as _venv
        _say(f"Creating isolated build environment in {VENV_DIR} ...\n")
        _venv.EnvBuilder(with_pip=True).create(str(VENV_DIR))
    except Exception as e:
        _say(f"venv creation failed: {e}\n")
        return None
    if not py.exists():
        _say("venv python missing after creation\n")
        return None
    try:
        _say(f"Installing {PINNED_PIO} into the isolated environment "
             "(first run only, may take a few minutes) ...\n")
        r = subprocess.run([str(py), "-m", "pip", "install", "--upgrade", "pip"],
                           capture_output=True, text=True, timeout=300)
        r = subprocess.run([str(py), "-m", "pip", "install", PINNED_PIO],
                           capture_output=True, text=True, timeout=900)
        if r.returncode != 0:
            _say("pip install of PlatformIO failed:\n" + (r.stderr or "")[-800:] + "\n")
            return None
    except (OSError, subprocess.TimeoutExpired) as e:
        _say(f"PlatformIO install failed: {e}\n")
        return None
    return pio if pio.exists() else None


# ============================================================================
#  PlatformIO discovery
# ============================================================================
def find_pio(prefer_isolated=True):
    """Return a runnable pio command (list form) or None.

    Prefers the project-local isolated venv (so unrelated PlatformIO projects
    and their differing core/platform versions can't interfere). Falls back to
    a system pio only if the isolated one isn't present yet."""
    if prefer_isolated:
        vp = _venv_pio()
        if vp.exists():
            return [str(vp)]
    candidates = [
        [sys.executable, "-m", "platformio"],
        ["pio"],
        ["platformio"],
    ]
    # Common install locations
    home = Path.home()
    extra = [
        home / ".platformio" / "penv" / "Scripts" / "pio.exe",   # Windows
        home / ".platformio" / "penv" / "bin" / "pio",            # *nix
    ]
    for e in extra:
        if e.exists():
            candidates.append([str(e)])
    for c in candidates:
        try:
            r = subprocess.run(c + ["--version"], capture_output=True,
                               timeout=10, env=isolated_env())
            if r.returncode == 0:
                return c
        except (OSError, subprocess.TimeoutExpired):
            continue
    return None


# ============================================================================
#  Main application
# ============================================================================
class Installer:
    def __init__(self, root):
        self.root = root
        root.title("Pinball-S3 Installer")
        root.geometry("1180x800")
        root.configure(bg=DARK)
        root.minsize(1080, 720)

        self.default_ttf = ID.find_default_font()

        # ---- Intro / theme state (mirrors intro_designer) ----
        self.lines = [
            {"var": "KYLER", "text": tk.StringVar(value="KYLER"),
             "ttf": tk.StringVar(value=self.default_ttf or ""),
             "size": tk.IntVar(value=28)},
            {"var": "MICHAELSON", "text": tk.StringVar(value="MICHAELSON"),
             "ttf": tk.StringVar(value=self.default_ttf or ""),
             "size": tk.IntVar(value=14)},
            {"var": "STUDIOS", "text": tk.StringVar(value="STUDIOS"),
             "ttf": tk.StringVar(value=self.default_ttf or ""),
             "size": tk.IntVar(value=18)},
        ]
        self.url_var = tk.StringVar(value="kylermichaelson.carrd.co")
        self.color_teal = "#007373"
        self.color_purple = "#5D005D"
        self.color_lime = "#00FF00"

        # ---- Mode / topic selections ----
        # mode_order is the live, reorderable list of flags. mode_vars holds the
        # include/exclude checkbox state. The on-device menu order follows
        # mode_order (filtered to enabled modes).
        self.mode_order = [flag for flag, _ in MENU_MODES]
        self.mode_label = {flag: label for flag, label in MENU_MODES}
        self.mode_vars = {flag: tk.BooleanVar(value=True)
                          for flag, _ in MENU_MODES}
        self.topic_vars = {bit: tk.BooleanVar(value=True)
                           for bit, _ in RABBIT_TOPICS}
        self.guide_vars = {bit: tk.BooleanVar(value=True)
                           for bit, _ in GUIDES}

        # ---- Default config ----
        self.cfg_ssid = tk.StringVar(value="")
        self.cfg_pass = tk.StringVar(value="")
        self.cfg_btc = tk.StringVar(value="")
        self.cfg_bch = tk.StringVar(value="")
        self.cfg_dgb = tk.StringVar(value="")
        self.cfg_pool = tk.StringVar(value="")
        self.cfg_zip = tk.StringVar(value="")
        self.cfg_tz_idx = tk.IntVar(value=3)   # default: Mountain
        self.flash_content = tk.BooleanVar(value=True)  # also flash .pak packs

        # ---- Runtime ----
        self.bitmaps_cache = {}
        self.bitmaps_dirty = True
        self.frame_idx = 0
        self.preview_imgtk = None
        self.port_var = tk.StringVar(value="(scanning...)")
        self.detected_port = None
        self.pio = find_pio()
        self.log_q = queue.Queue()
        self.uploading = False
        self.preview_mode = tk.StringVar(value="intro")  # intro | menu

        self._build_ui()
        self._render_bitmaps()
        self._tick_preview()
        self._scan_ports()
        self._drain_log()

    # ------------------------------------------------------------------ UI
    def _build_ui(self):
        # Tabbed layout: main installer + a background-image converter tab.
        self.nb = ttk.Notebook(self.root)
        self.nb.pack(fill="both", expand=True)
        installer_tab = tk.Frame(self.nb, bg=DARK)
        self.nb.add(installer_tab, text="  Installer  ")
        bg_tab = tk.Frame(self.nb, bg=DARK)
        self.nb.add(bg_tab, text="  Backgrounds  ")
        self._build_bg_tab(bg_tab)
        rw_tab = tk.Frame(self.nb, bg=DARK)
        self.nb.add(rw_tab, text="  Quest Rewards  ")
        self._build_rewards_tab(rw_tab)

        # Three columns: controls (scroll) | preview | log
        outer = tk.Frame(installer_tab, bg=DARK)
        outer.pack(fill="both", expand=True)

        # --- LEFT: scrollable controls ---
        left_wrap = tk.Frame(outer, bg=DARK, width=420)
        left_wrap.pack(side="left", fill="y")
        left_wrap.pack_propagate(False)

        canvas = tk.Canvas(left_wrap, bg=DARK, highlightthickness=0, width=400)
        sb = ttk.Scrollbar(left_wrap, orient="vertical", command=canvas.yview)
        self.ctrl = tk.Frame(canvas, bg=DARK)
        self.ctrl.bind("<Configure>",
                       lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=self.ctrl, anchor="nw")
        canvas.configure(yscrollcommand=sb.set)
        canvas.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        # Mouse wheel scroll
        canvas.bind_all("<MouseWheel>",
                        lambda e: canvas.yview_scroll(int(-e.delta / 120), "units"))

        self._build_connection_panel()
        self._build_color_panel()
        self._build_intro_panel()
        self._build_modes_panel()
        self._build_sdk_panel()
        self._build_topics_panel()
        self._build_guides_panel()
        self._build_config_panel()
        self._build_action_panel()

        # --- MIDDLE: device preview ---
        mid = tk.Frame(outer, bg=DARK)
        mid.pack(side="left", fill="y", padx=14, pady=12)
        head = tk.Frame(mid, bg=DARK)
        head.pack(anchor="w")
        tk.Label(head, text="DEVICE PREVIEW", fg=TEXT, bg=DARK,
                 font=("TkDefaultFont", 11, "bold")).pack(side="left")
        toggle = tk.Frame(mid, bg=DARK)
        toggle.pack(anchor="w", pady=(4, 6))
        for label, val in [("Intro", "intro"), ("Menu", "menu")]:
            tk.Radiobutton(toggle, text=label, value=val,
                           variable=self.preview_mode, fg=TEXT, bg=DARK,
                           selectcolor=PANEL, activebackground=DARK,
                           activeforeground=TEXT,
                           command=self._restart_preview).pack(side="left")
        self.canvas = tk.Canvas(mid, width=W * PREVIEW_SCALE,
                                height=H * PREVIEW_SCALE, bg="black",
                                highlightthickness=1,
                                highlightbackground="#444")
        self.canvas.pack()
        tk.Button(mid, text="Replay intro", command=self._restart_preview,
                  bg=PANEL, fg=TEXT, relief="flat").pack(pady=6, fill="x")

        # --- RIGHT: build log ---
        right = tk.Frame(outer, bg=DARK)
        right.pack(side="left", fill="both", expand=True, padx=(0, 12), pady=12)
        tk.Label(right, text="BUILD / UPLOAD LOG", fg=TEXT, bg=DARK,
                 font=("TkDefaultFont", 11, "bold")).pack(anchor="w")
        logwrap = tk.Frame(right, bg=DARK)
        logwrap.pack(fill="both", expand=True, pady=6)
        self.logbox = tk.Text(logwrap, bg="#0c0c10", fg="#9fe89f",
                              insertbackground="#9fe89f", wrap="word",
                              font=("Consolas", 9), relief="flat",
                              state="disabled")
        logsb = ttk.Scrollbar(logwrap, command=self.logbox.yview)
        self.logbox.configure(yscrollcommand=logsb.set)
        self.logbox.pack(side="left", fill="both", expand=True)
        logsb.pack(side="right", fill="y")
        # Colored tags
        self.logbox.tag_config("err", foreground="#ff8080")
        self.logbox.tag_config("ok", foreground="#80ff80")
        self.logbox.tag_config("info", foreground="#80c0ff")

    # ======================================================================
    #  BACKGROUNDS TAB - convert JPG/PNG into the compact .aqbg format the
    #  aquarium loads from the content partition. Format is a small palette +
    #  RLE indices, so flat art is only a few KB. Palette slots 0 and 1 are the
    #  "glow" colors the in-game black light illuminates.
    # ======================================================================
    BG_W = 170
    BG_H = 300          # the aquarium water area (full screen minus HUD bands)
    BG_PALETTE = 16     # max colors

    # ================================================================ QUEST REWARDS TAB
    # Full control over the quest reward economy plus a to-do builder. Everything
    # here writes DATA FILES the firmware reads at runtime:
    #    data/content/rewards.cfg  -> reward_config.cpp (tuning, no recompile)
    #    data/content/todos.txt    -> imported by quests_mode on next boot,
    #                                 in the SAME line format the captive portal
    #                                 posts, so the portal keeps working.
    QR_CATS  = ["General", "Physical", "Mental", "Creative", "Kindness"]
    QR_TIERS = ["Low", "Mid", "High", "Special"]
    QR_ATTRS = ["None", "Lucky", "Keen", "Heavy", "Wild", "Swift", "Brave", "Wise"]
    QR_TAGS  = [("Pinball", 1), ("Pet", 2), ("CYOA", 4), ("Aquarium", 8),
                ("Fishing", 16), ("Any", 31)]
    QR_DAYS  = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]
    QR_ITEM_COUNT = 30
    QR_MAX_TASKS = 6

    def _qr_content_dir(self):
        d = ROOT / "data" / "content"
        d.mkdir(parents=True, exist_ok=True)
        return d

    def _build_rewards_tab(self, parent):
        wrap = tk.Frame(parent, bg=DARK)
        wrap.pack(fill="both", expand=True, padx=12, pady=10)
        tk.Label(wrap, text="Quest Reward Designer", fg=TEXT, bg=DARK,
                 font=("Arial", 15, "bold")).pack(anchor="w")
        tk.Label(wrap,
                 text=("Tune how quest mode pays out, retag the reward items, and build the weekly to-do list.\n"
                       "These write data files the firmware reads at boot - no recompile needed. The captive\n"
                       "portal setup screen still works; a to-do file is imported once, then the portal owns it."),
                 fg=SUBTLE, bg=DARK, justify="left").pack(anchor="w", pady=(2, 10))

        sub = ttk.Notebook(wrap)
        sub.pack(fill="both", expand=True)
        f_pay = tk.Frame(sub, bg=DARK); sub.add(f_pay, text="  Payouts  ")
        f_itm = tk.Frame(sub, bg=DARK); sub.add(f_itm, text="  Reward Items  ")
        f_tod = tk.Frame(sub, bg=DARK); sub.add(f_tod, text="  To-Do Builder  ")
        self._build_qr_payouts(f_pay)
        self._build_qr_items(f_itm)
        self._build_qr_todos(f_tod)

        bar = tk.Frame(wrap, bg=DARK); bar.pack(fill="x", pady=(10, 0))
        tk.Button(bar, text="Write reward config", command=self._qr_write_rewards,
                  bg=ACCENT, fg="#000", relief="flat", padx=12, pady=4).pack(side="left")
        tk.Button(bar, text="Write to-do list", command=self._qr_write_todos,
                  bg=ACCENT, fg="#000", relief="flat", padx=12, pady=4).pack(side="left", padx=8)
        tk.Button(bar, text="Load existing", command=self._qr_load_existing,
                  bg=PANEL, fg=TEXT, relief="flat", padx=12, pady=4).pack(side="left")
        tk.Button(bar, text="Restore defaults", command=self._qr_defaults,
                  bg=PANEL, fg=TEXT, relief="flat", padx=12, pady=4).pack(side="left", padx=8)
        self.qr_status = tk.Label(bar, text="", fg=SUBTLE, bg=DARK)
        self.qr_status.pack(side="left", padx=12)

    # ---- payouts: per-difficulty XP, die, and tier odds ----
    def _build_qr_payouts(self, parent):
        info = tk.Label(parent, fg=SUBTLE, bg=DARK, justify="left",
                        text=("For each difficulty (1-5 stars): XP paid, the random spread on top, the chance of a\n"
                              "die drop and its size, and the odds of each item tier. Tier odds are percentages and\n"
                              "are normalised to 100 when written."))
        info.pack(anchor="w", padx=10, pady=(8, 6))
        hdr = ["Diff", "XP base", "XP spread", "Die %", "Die sides",
               "No item %", "Low %", "Mid %", "High %", "Special %"]
        grid = tk.Frame(parent, bg=DARK); grid.pack(anchor="w", padx=10)
        for c, h in enumerate(hdr):
            tk.Label(grid, text=h, fg=TEXT, bg=DARK, font=("Arial", 9, "bold"),
                     width=10 if c else 5).grid(row=0, column=c, padx=2, pady=3)
        self.qr_pay = []
        for d in range(1, 6):
            row = {}
            tk.Label(grid, text=f"{d}*", fg=ACCENT, bg=DARK, width=5).grid(row=d, column=0)
            for c, key in enumerate(["xp", "spread", "diepct", "diesides",
                                     "t_none", "t_low", "t_mid", "t_high", "t_spec"], start=1):
                v = tk.StringVar()
                e = tk.Entry(grid, textvariable=v, width=10, bg=PANEL, fg=TEXT,
                             insertbackground=TEXT, relief="flat", justify="center")
                e.grid(row=d, column=c, padx=2, pady=2)
                row[key] = v
            self.qr_pay.append(row)

        tk.Label(parent, text="Die attribute pool per task category",
                 fg=TEXT, bg=DARK, font=("Arial", 11, "bold")).pack(anchor="w", padx=10, pady=(14, 4))
        tk.Label(parent, fg=SUBTLE, bg=DARK, justify="left",
                 text="Which die attributes a category can roll. Tick at least one, or the built-in flavour is used."
                 ).pack(anchor="w", padx=10)
        cg = tk.Frame(parent, bg=DARK); cg.pack(anchor="w", padx=10, pady=6)
        self.qr_catattr = []
        for ci, cname in enumerate(self.QR_CATS):
            tk.Label(cg, text=cname, fg=TEXT, bg=DARK, width=10, anchor="w").grid(row=ci, column=0, pady=2)
            vars_ = []
            for ai, aname in enumerate(self.QR_ATTRS):
                bv = tk.BooleanVar(value=False)
                tk.Checkbutton(cg, text=aname, variable=bv, fg=SUBTLE, bg=DARK,
                               selectcolor=PANEL, activebackground=DARK,
                               activeforeground=TEXT).grid(row=ci, column=ai + 1, sticky="w")
                vars_.append(bv)
            self.qr_catattr.append(vars_)
        self._qr_defaults_payouts()

    # ---- reward items: retier / retag / rename ----
    def _build_qr_items(self, parent):
        tk.Label(parent, fg=SUBTLE, bg=DARK, justify="left",
                 text=("Re-tier and re-tag the 30 reward items. Tier controls which difficulties can drop it; the\n"
                       "tags control which task categories favour it and which game modes it belongs to. This is how\n"
                       "you make a 'new' reward without a firmware rebuild - repurpose a slot and rename it.\n"
                       "NOTE: the name is used for the config and installer display; in-game item names still come\n"
                       "from the firmware table, so tell me if you want those made data-driven too.")
                 ).pack(anchor="w", padx=10, pady=(8, 6))
        canvas = tk.Canvas(parent, bg=DARK, highlightthickness=0, height=380)
        sb = tk.Scrollbar(parent, orient="vertical", command=canvas.yview)
        inner = tk.Frame(canvas, bg=DARK)
        canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=sb.set)
        canvas.pack(side="left", fill="both", expand=True, padx=(10, 0))
        sb.pack(side="right", fill="y")
        inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        for c, h in enumerate(["#", "Name (label)", "Tier", "Modes / tags"]):
            tk.Label(inner, text=h, fg=TEXT, bg=DARK, font=("Arial", 9, "bold")).grid(
                row=0, column=c, padx=4, pady=3, sticky="w")
        self.qr_items = []
        for i in range(self.QR_ITEM_COUNT):
            tk.Label(inner, text=str(i), fg=ACCENT, bg=DARK, width=3).grid(row=i + 1, column=0)
            nv = tk.StringVar()
            tk.Entry(inner, textvariable=nv, width=20, bg=PANEL, fg=TEXT,
                     insertbackground=TEXT, relief="flat").grid(row=i + 1, column=1, padx=4, pady=1)
            tv = tk.StringVar(value=self.QR_TIERS[0])
            ttk.Combobox(inner, textvariable=tv, values=self.QR_TIERS, width=8,
                         state="readonly").grid(row=i + 1, column=2, padx=4)
            tagf = tk.Frame(inner, bg=DARK); tagf.grid(row=i + 1, column=3, sticky="w")
            tvars = []
            for tn, _tb in self.QR_TAGS[:-1]:
                bv = tk.BooleanVar(value=False)
                tk.Checkbutton(tagf, text=tn, variable=bv, fg=SUBTLE, bg=DARK,
                               selectcolor=PANEL, activebackground=DARK).pack(side="left")
                tvars.append(bv)
            self.qr_items.append({"name": nv, "tier": tv, "tags": tvars})

    # ---- to-do builder ----
    def _build_qr_todos(self, parent):
        tk.Label(parent, fg=SUBTLE, bg=DARK, justify="left",
                 text=("Build the weekly recurring to-do list here instead of the captive portal. Each day takes up\n"
                       "to six tasks; a blank label means an empty slot. Category and difficulty drive the reward.\n"
                       "The file is imported ONCE on the next boot, then the portal owns the schedule again.")
                 ).pack(anchor="w", padx=10, pady=(8, 4))
        top = tk.Frame(parent, bg=DARK); top.pack(anchor="w", padx=10, pady=2)
        tk.Label(top, text="Week starts on", fg=TEXT, bg=DARK).pack(side="left")
        self.qr_startdow = tk.StringVar(value="Sun")
        ttk.Combobox(top, textvariable=self.qr_startdow, values=self.QR_DAYS,
                     width=6, state="readonly").pack(side="left", padx=6)
        tk.Label(top, text="(used only when the clock isn't set)", fg=SUBTLE, bg=DARK).pack(side="left")

        canvas = tk.Canvas(parent, bg=DARK, highlightthickness=0, height=360)
        sb = tk.Scrollbar(parent, orient="vertical", command=canvas.yview)
        inner = tk.Frame(canvas, bg=DARK)
        canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=sb.set)
        canvas.pack(side="left", fill="both", expand=True, padx=(10, 0), pady=6)
        sb.pack(side="right", fill="y")
        inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        self.qr_tasks = []
        r = 0
        for d, dn in enumerate(self.QR_DAYS):
            tk.Label(inner, text=dn, fg=ACCENT, bg=DARK,
                     font=("Arial", 11, "bold")).grid(row=r, column=0, sticky="w", pady=(8, 2))
            r += 1
            day = []
            for s in range(self.QR_MAX_TASKS):
                lv = tk.StringVar(); cv = tk.StringVar(value=self.QR_CATS[0]); dv = tk.StringVar(value="1")
                tk.Entry(inner, textvariable=lv, width=28, bg=PANEL, fg=TEXT,
                         insertbackground=TEXT, relief="flat").grid(row=r, column=0, padx=(14, 4), pady=1)
                ttk.Combobox(inner, textvariable=cv, values=self.QR_CATS, width=10,
                             state="readonly").grid(row=r, column=1, padx=2)
                ttk.Combobox(inner, textvariable=dv, values=["1", "2", "3", "4", "5"], width=4,
                             state="readonly").grid(row=r, column=2, padx=2)
                day.append({"label": lv, "cat": cv, "diff": dv})
                r += 1
            self.qr_tasks.append(day)

    # ---- defaults mirroring the firmware's built-in economy ----
    def _qr_defaults_payouts(self):
        # These reproduce quest_profile.cpp's hardcoded behaviour, so "write" with
        # no edits is a no-op change in feel.
        base = [
            # xp, spread, diepct, diesides, none, low, mid, high, special
            (10, 5, 0, 6, 75, 25, 0, 0, 0),
            (20, 10, 0, 6, 47, 45, 8, 0, 0),
            (30, 15, 18, 8, 0, 30, 62, 8, 0),
            (40, 20, 24, 10, 0, 18, 50, 22, 10),
            (50, 25, 30, 12, 0, 0, 0, 75, 25),
        ]
        for i, row in enumerate(self.qr_pay):
            xp, sp, dp, ds, n, lo, mi, hi, spc = base[i]
            for k, v in zip(["xp", "spread", "diepct", "diesides",
                             "t_none", "t_low", "t_mid", "t_high", "t_spec"],
                            [xp, sp, dp, ds, n, lo, mi, hi, spc]):
                row[k].set(str(v))
        # category -> attribute pools, matching pickDieForCategory
        defaults = {0: ["Keen", "None"], 1: ["Swift", "Brave"], 2: ["Wise", "Keen"],
                    3: ["Wild"], 4: ["Lucky"]}
        for ci, vars_ in enumerate(self.qr_catattr):
            want = defaults.get(ci, [])
            for ai, bv in enumerate(vars_):
                bv.set(self.QR_ATTRS[ai] in want)

    def _qr_defaults(self):
        self._qr_defaults_payouts()
        for it in self.qr_items:
            it["name"].set(""); it["tier"].set(self.QR_TIERS[0])
            for bv in it["tags"]:
                bv.set(False)
        for day in self.qr_tasks:
            for t in day:
                t["label"].set(""); t["cat"].set(self.QR_CATS[0]); t["diff"].set("1")
        self.qr_status.config(text="defaults restored (nothing written yet)")

    # ---- writers ----
    def _qr_write_rewards(self):
        lines = ["@REWARDCFG 1",
                 "# Written by the installer's Quest Reward Designer.",
                 "# D|diff|xpBase|xpSpread|diePct|dieSides",
                 "# T|diff|none|low|mid|high|special   (percent, normalised to 100)",
                 "# C|cat|attr,attr,...                (die attribute pool)",
                 "# I|itemId|tier|tagMask|name"]
        try:
            for i, row in enumerate(self.qr_pay, start=1):
                g = lambda k: int(float(row[k].get() or 0))
                lines.append(f"D|{i}|{g('xp')}|{g('spread')}|{g('diepct')}|{g('diesides')}")
                odds = [g("t_none"), g("t_low"), g("t_mid"), g("t_high"), g("t_spec")]
                tot = sum(odds)
                if tot <= 0:
                    odds = [100, 0, 0, 0, 0]
                else:                      # normalise to exactly 100
                    odds = [int(round(o * 100.0 / tot)) for o in odds]
                    odds[-1] += 100 - sum(odds)
                lines.append(f"T|{i}|" + "|".join(str(o) for o in odds))
            for ci, vars_ in enumerate(self.qr_catattr):
                picked = [str(ai) for ai, bv in enumerate(vars_) if bv.get()]
                if picked:
                    lines.append(f"C|{ci}|" + ",".join(picked))
            for i, it in enumerate(self.qr_items):
                tags = 0
                for bit, bv in zip([1, 2, 4, 8, 16], it["tags"]):
                    if bv.get():
                        tags |= bit
                nm = it["name"].get().strip()
                if tags == 0 and not nm:
                    continue                # untouched slot: let the firmware decide
                tier = self.QR_TIERS.index(it["tier"].get())
                lines.append(f"I|{i}|{tier}|{tags}|{nm}")
        except ValueError:
            messagebox.showerror("Quest rewards", "Every payout field must be a number.")
            return
        path = self._qr_content_dir() / "rewards.cfg"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        self.qr_status.config(text=f"wrote {path.name} ({len(lines)} lines)")
        self._log(f"[rewards] wrote {path}")

    def _qr_write_todos(self):
        dow = self.QR_DAYS.index(self.qr_startdow.get())
        out = [str(dow)]
        count = 0
        for d, day in enumerate(self.qr_tasks):
            for s, t in enumerate(day):
                lbl = t["label"].get().strip()
                if not lbl:
                    continue
                cat = self.QR_CATS.index(t["cat"].get())
                dif = int(t["diff"].get())
                lbl = lbl.replace("|", "-")[:27]     # '|' is the field separator
                out.append(f"{d}|{s}|{cat}|{dif}|{lbl}")
                count += 1
        if count == 0:
            messagebox.showinfo("To-do list", "No tasks entered - nothing written.")
            return
        path = self._qr_content_dir() / "todos.txt"
        path.write_text("\n".join(out) + "\n", encoding="utf-8")
        self.qr_status.config(text=f"wrote {path.name} ({count} tasks)")
        self._log(f"[rewards] wrote {path} with {count} tasks")

    def _qr_load_existing(self):
        p = self._qr_content_dir() / "rewards.cfg"
        if not p.exists():
            self.qr_status.config(text="no rewards.cfg to load")
            return
        try:
            for raw in p.read_text(encoding="utf-8").splitlines():
                raw = raw.strip()
                if not raw or raw.startswith("#") or raw.startswith("@"):
                    continue
                f = raw.split("|")
                if f[0] == "D" and len(f) >= 6:
                    d = int(f[1])
                    if 1 <= d <= 5:
                        r = self.qr_pay[d - 1]
                        r["xp"].set(f[2]); r["spread"].set(f[3])
                        r["diepct"].set(f[4]); r["diesides"].set(f[5])
                elif f[0] == "T" and len(f) >= 7:
                    d = int(f[1])
                    if 1 <= d <= 5:
                        r = self.qr_pay[d - 1]
                        for k, v in zip(["t_none", "t_low", "t_mid", "t_high", "t_spec"], f[2:7]):
                            r[k].set(v)
                elif f[0] == "C" and len(f) >= 3:
                    ci = int(f[1])
                    if 0 <= ci < len(self.qr_catattr):
                        want = {int(x) for x in f[2].split(",") if x.strip().isdigit()}
                        for ai, bv in enumerate(self.qr_catattr[ci]):
                            bv.set(ai in want)
                elif f[0] == "I" and len(f) >= 4:
                    i = int(f[1])
                    if 0 <= i < len(self.qr_items):
                        it = self.qr_items[i]
                        it["tier"].set(self.QR_TIERS[min(3, max(0, int(f[2])))])
                        mask = int(f[3])
                        for bit, bv in zip([1, 2, 4, 8, 16], it["tags"]):
                            bv.set(bool(mask & bit))
                        if len(f) >= 5:
                            it["name"].set(f[4])
            self.qr_status.config(text="loaded rewards.cfg")
        except Exception as e:
            self.qr_status.config(text=f"load failed: {e}")

    def _build_bg_tab(self, parent):
        wrap = tk.Frame(parent, bg=DARK)
        wrap.pack(fill="both", expand=True, padx=16, pady=16)

        tk.Label(wrap, text="Aquarium Background Converter", fg=TEXT, bg=DARK,
                 font=("Arial", 15, "bold")).pack(anchor="w")
        tk.Label(wrap,
                 text=("Turn a JPG/PNG into a tiny .aqbg file for the tank background.\n"
                       "Images are resized to 170x300 and reduced to a 16-color palette + RLE.\n"
                       "Pick up to TWO 'glow' colors - those pixels light up under the in-game black light."),
                 fg=SUBTLE, bg=DARK, justify="left").pack(anchor="w", pady=(2, 12))

        row = tk.Frame(wrap, bg=DARK); row.pack(fill="x", pady=4)
        tk.Button(row, text="Choose image...", command=self._bg_pick,
                  bg=ACCENT, fg="#000", relief="flat", padx=12, pady=4).pack(side="left")
        self.bg_path_lbl = tk.Label(row, text="(no file)", fg=SUBTLE, bg=DARK)
        self.bg_path_lbl.pack(side="left", padx=10)

        # preview + palette swatches
        body = tk.Frame(wrap, bg=DARK); body.pack(fill="both", expand=True, pady=10)
        self.bg_preview = tk.Label(body, bg="#001a1a", width=170, height=300)
        self.bg_preview.pack(side="left", padx=(0, 16))

        pal = tk.Frame(body, bg=DARK); pal.pack(side="left", anchor="n")
        tk.Label(pal, text="Palette (click up to 2 = glow):", fg=TEXT, bg=DARK).pack(anchor="w")
        self.bg_swatch_holder = tk.Frame(pal, bg=DARK); self.bg_swatch_holder.pack(anchor="w", pady=6)
        self.bg_glow = []          # indices chosen as glow
        self.bg_palette_cols = []  # list of (r,g,b)

        srow = tk.Frame(wrap, bg=DARK); srow.pack(fill="x", pady=6)
        tk.Label(srow, text="Name:", fg=TEXT, bg=DARK).pack(side="left")
        self.bg_name = tk.Entry(srow, width=14); self.bg_name.pack(side="left", padx=6)
        self.bg_name.insert(0, "reef")

        tk.Label(srow, text="Where:", fg=TEXT, bg=DARK).pack(side="left", padx=(10, 2))
        self.bg_category = tk.StringVar(value="shop")
        tk.Radiobutton(srow, text="Shop", variable=self.bg_category, value="shop",
                       fg=TEXT, bg=DARK, selectcolor=PANEL,
                       activebackground=DARK, command=self._bg_cat_changed).pack(side="left")
        tk.Radiobutton(srow, text="Quest reward", variable=self.bg_category, value="quest",
                       fg=TEXT, bg=DARK, selectcolor=PANEL,
                       activebackground=DARK, command=self._bg_cat_changed).pack(side="left")

        self.bg_cost_lbl = tk.Label(srow, text="Cost:", fg=TEXT, bg=DARK)
        self.bg_cost_lbl.pack(side="left", padx=(10, 2))
        self.bg_cost = tk.Entry(srow, width=6); self.bg_cost.pack(side="left")
        self.bg_cost.insert(0, "40")

        srow2 = tk.Frame(wrap, bg=DARK); srow2.pack(fill="x", pady=2)
        tk.Button(srow2, text="Save background", command=self._bg_save,
                  bg=ACCENT, fg="#000", relief="flat", padx=12, pady=4).pack(side="left")
        tk.Label(srow2, text="(writes the .aqbg + updates the in-game list automatically)",
                 fg=SUBTLE, bg=DARK).pack(side="left", padx=8)
        self.bg_status = tk.Label(wrap, text="", fg=SUBTLE, bg=DARK); self.bg_status.pack(anchor="w")

        # show what's already registered (full viewer with thumbnails + edit)
        tk.Label(wrap, text="Backgrounds going into the game:", fg=TEXT, bg=DARK,
                 font=("Arial", 12, "bold")).pack(anchor="w", pady=(12, 2))
        viewer_wrap = tk.Frame(wrap, bg=DARK); viewer_wrap.pack(fill="both", expand=True)
        vcanvas = tk.Canvas(viewer_wrap, bg=DARK, highlightthickness=0, height=260)
        vscroll = tk.Scrollbar(viewer_wrap, orient="vertical", command=vcanvas.yview)
        self.bg_viewer = tk.Frame(vcanvas, bg=DARK)
        self.bg_viewer.bind("<Configure>",
                            lambda e: vcanvas.configure(scrollregion=vcanvas.bbox("all")))
        vcanvas.create_window((0, 0), window=self.bg_viewer, anchor="nw")
        vcanvas.configure(yscrollcommand=vscroll.set)
        vcanvas.pack(side="left", fill="both", expand=True)
        vscroll.pack(side="right", fill="y")
        self._bg_refresh_list()

        self._bg_img = None        # quantized PIL image (P mode)

    def _bg_cat_changed(self):
        # quest-reward backgrounds have no coin cost
        if self.bg_category.get() == "quest":
            self.bg_cost.delete(0, "end"); self.bg_cost.insert(0, "0")
            self.bg_cost.config(state="disabled")
            self.bg_cost_lbl.config(fg=SUBTLE)
        else:
            self.bg_cost.config(state="normal")
            self.bg_cost_lbl.config(fg=TEXT)

    def _bg_manifest_path(self):
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        return os.path.join(root, "data", "content", "backgrounds.idx")

    def _bg_read_manifest(self):
        path = self._bg_manifest_path()
        rows = []
        if os.path.exists(path):
            with open(path, "r") as fh:
                for line in fh:
                    line = line.strip()
                    if not line:
                        continue
                    parts = line.split("|")
                    if len(parts) == 4:
                        rows.append(parts)   # [stem, name, cat, cost]
        return rows

    def _bg_write_manifest(self, rows):
        path = self._bg_manifest_path()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            for r in rows:
                fh.write("|".join(str(x) for x in r) + "\n")

    def _bg_decode_aqbg(self, path):
        """Decode a .aqbg file back into a PIL image for preview. Returns None on error."""
        try:
            data = open(path, "rb").read()
            if data[:4] != b"AQBG":
                return None
            W = data[5]; H = (data[6] << 8) | data[7]
            palCount = data[8]; glowCount = data[9]
            pos = 10
            pal = []
            for _ in range(palCount):
                lo = data[pos]; hi = data[pos+1]; pos += 2
                c = lo | (hi << 8)
                r = ((c >> 11) & 0x1F) << 3
                g = ((c >> 5) & 0x3F) << 2
                b = (c & 0x1F) << 3
                pal.append((r, g, b))
            img = Image.new("RGB", (W, H))
            px = img.load()
            i = pos; idx = 0; total = W * H
            while i + 1 < len(data) and idx < total:
                run = data[i]; pi = data[i+1] & 0x0F; i += 2
                for _ in range(run):
                    if idx >= total:
                        break
                    x = idx % W; y = idx // W
                    px[x, y] = pal[pi] if pi < len(pal) else (0, 0, 0)
                    idx += 1
            return img, glowCount
        except Exception:
            return None

    def _bg_refresh_list(self):
        # Clear the viewer area and repopulate with a card per registered bg.
        for w in self.bg_viewer.winfo_children():
            w.destroy()
        rows = self._bg_read_manifest()
        if not rows:
            tk.Label(self.bg_viewer, text="No backgrounds registered yet.",
                     fg=SUBTLE, bg=DARK).pack(anchor="w", padx=6, pady=6)
            return
        content_dir = os.path.dirname(self._bg_manifest_path())
        self._bg_thumbs = getattr(self, "_bg_thumbs", [])
        self._bg_thumbs.clear()
        for stem, name, cat, cost in rows:
            card = tk.Frame(self.bg_viewer, bg=PANEL, bd=1, relief="solid")
            card.pack(fill="x", padx=4, pady=3)
            # thumbnail
            aqbg_path = os.path.join(content_dir, f"{stem}.aqbg")
            thumb_lbl = tk.Label(card, bg="#001a1a", width=34, height=60)
            if os.path.exists(aqbg_path):
                dec = self._bg_decode_aqbg(aqbg_path)
                if dec:
                    img, _gc = dec
                    th = img.resize((34, 60))
                    tkimg = ImageTk.PhotoImage(th)
                    self._bg_thumbs.append(tkimg)
                    thumb_lbl.config(image=tkimg)
            thumb_lbl.pack(side="left", padx=4, pady=4)
            # info
            info = tk.Frame(card, bg=PANEL); info.pack(side="left", fill="x", expand=True, padx=6)
            tk.Label(info, text=name, fg=TEXT, bg=PANEL,
                     font=("Arial", 11, "bold")).pack(anchor="w")
            tag = f"Shop  -  {cost} coins" if cat == "shop" else "Quest reward"
            tk.Label(info, text=tag, fg=SUBTLE, bg=PANEL).pack(anchor="w")
            tk.Label(info, text=f"file: {stem}.aqbg", fg=SUBTLE, bg=PANEL,
                     font=("Arial", 8)).pack(anchor="w")
            # actions
            act = tk.Frame(card, bg=PANEL); act.pack(side="right", padx=6)
            tk.Button(act, text="Edit vars", command=lambda s=stem: self._bg_edit(s),
                      bg=PANEL, fg=ACCENT, relief="flat").pack(pady=1)
            tk.Button(act, text="Replace", command=lambda s=stem: self._bg_replace(s),
                      bg=PANEL, fg=TEXT, relief="flat").pack(pady=1)
            tk.Button(act, text="Delete", command=lambda s=stem: self._bg_delete(s),
                      bg=PANEL, fg="#ff8080", relief="flat").pack(pady=1)

    def _bg_edit(self, stem):
        # load this bg's variables into the editor fields for re-saving
        rows = self._bg_read_manifest()
        for s, name, cat, cost in rows:
            if s == stem:
                self.bg_name.delete(0, "end"); self.bg_name.insert(0, s)
                self.bg_category.set(cat)
                self._bg_cat_changed()
                if cat == "shop":
                    self.bg_cost.delete(0, "end"); self.bg_cost.insert(0, str(cost))
                self.bg_status.config(
                    text=f"Editing '{name}'. Change Where/Cost above, then Save "
                         f"(re-pick an image only if you want to replace the art).")
                return

    def _bg_replace(self, stem):
        # pre-fill the name so a new Save overwrites this background's art
        self.bg_name.delete(0, "end"); self.bg_name.insert(0, stem)
        self.bg_status.config(text=f"Pick a new image, then Save to replace '{stem}'.")
        self._bg_pick()

    def _bg_delete(self, stem):
        rows = [r for r in self._bg_read_manifest() if r[0] != stem]
        self._bg_write_manifest(rows)
        # remove the .aqbg file too
        try:
            os.remove(os.path.join(os.path.dirname(self._bg_manifest_path()), f"{stem}.aqbg"))
        except OSError:
            pass
        self._bg_refresh_list()
        self.bg_status.config(text=f"Deleted '{stem}'.")

    def _bg_pick(self):
        from tkinter import filedialog
        p = filedialog.askopenfilename(filetypes=[("Images", "*.jpg *.jpeg *.png *.bmp")])
        if not p: return
        self.bg_path_lbl.config(text=os.path.basename(p))
        try:
            img = Image.open(p).convert("RGB").resize((self.BG_W, self.BG_H))
            # quantize to a small palette
            q = img.quantize(colors=self.BG_PALETTE, method=Image.MEDIANCUT)
            self._bg_img = q
            pal = q.getpalette()[: self.BG_PALETTE * 3]
            self.bg_palette_cols = [(pal[i], pal[i+1], pal[i+2]) for i in range(0, len(pal), 3)]
            self.bg_glow = []
            self._bg_render_preview()
            self._bg_render_swatches()
            self.bg_status.config(text="Loaded. Click swatches to mark glow colors.")
        except Exception as e:
            self.bg_status.config(text=f"Error: {e}")

    def _bg_render_preview(self):
        if not self._bg_img: return
        disp = self._bg_img.convert("RGB").resize((self.BG_W, self.BG_H))
        self._bg_tkimg = ImageTk.PhotoImage(disp)
        self.bg_preview.config(image=self._bg_tkimg, width=self.BG_W, height=self.BG_H)

    def _bg_render_swatches(self):
        for w in self.bg_swatch_holder.winfo_children():
            w.destroy()
        for i, (r, g, b) in enumerate(self.bg_palette_cols):
            hexc = f"#{r:02x}{g:02x}{b:02x}"
            glow = i in self.bg_glow
            f = tk.Frame(self.bg_swatch_holder, width=30, height=22, bg=hexc,
                         highlightbackground=("#ffff00" if glow else "#444"),
                         highlightthickness=(3 if glow else 1))
            f.grid(row=i // 4, column=i % 4, padx=3, pady=3)
            f.bind("<Button-1>", lambda e, idx=i: self._bg_toggle_glow(idx))

    def _bg_toggle_glow(self, idx):
        if idx in self.bg_glow:
            self.bg_glow.remove(idx)
        elif len(self.bg_glow) < 2:
            self.bg_glow.append(idx)
        self._bg_render_swatches()

    @staticmethod
    def _rgb565(r, g, b):
        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

    def _bg_save(self):
        if not self._bg_img:
            self.bg_status.config(text="Load an image first.")
            return
        name = (self.bg_name.get().strip() or "bg").lower()
        # Reorder palette so the two glow colors occupy slots 0 and 1.
        order = list(self.bg_glow) + [i for i in range(len(self.bg_palette_cols)) if i not in self.bg_glow]
        remap = {old: new for new, old in enumerate(order)}
        pal565 = [self._rgb565(*self.bg_palette_cols[old]) for old in order]
        # build remapped index image
        idxs = list(self._bg_img.getdata())   # palette indices 0..15
        idxs = [remap.get(v, 0) for v in idxs]
        # RLE encode: (count, index) pairs, count 1..255
        rle = bytearray()
        i = 0
        n = len(idxs)
        while i < n:
            v = idxs[i]; run = 1
            while i + run < n and idxs[i + run] == v and run < 255:
                run += 1
            rle.append(run); rle.append(v & 0x0F)
            i += run
        # header: magic, ver, W, H, palcount, glowcount
        out = bytearray()
        out += b"AQBG"
        out.append(1)                       # version
        out.append(self.BG_W & 0xFF)
        out.append((self.BG_H >> 8) & 0xFF); out.append(self.BG_H & 0xFF)
        out.append(len(pal565))             # palette count
        out.append(len(self.bg_glow))       # number of glow colors (0..2, in slots 0..1)
        for c in pal565:
            out.append(c & 0xFF); out.append((c >> 8) & 0xFF)
        out += rle
        # write to data/content/<name>.aqbg
        try:
            root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
            dest_dir = os.path.join(root, "data", "content")
            os.makedirs(dest_dir, exist_ok=True)
            dest = os.path.join(dest_dir, f"{name}.aqbg")
            with open(dest, "wb") as fh:
                fh.write(out)

            # Register (or update) it in the manifest the firmware reads, so it
            # shows up in-game automatically - no source editing, no rebuild of
            # the menu table. category = shop|quest, plus a coin cost for shop.
            cat = self.bg_category.get()
            try:
                cost = int(self.bg_cost.get()) if cat == "shop" else 0
            except ValueError:
                cost = 0
            disp = name.capitalize()
            rows = self._bg_read_manifest()
            rows = [r for r in rows if r[0] != name]   # replace any existing entry
            rows.append([name, disp, cat, cost])
            self._bg_write_manifest(rows)
            self._bg_refresh_list()

            kb = len(out) / 1024.0
            where = f"shop ({cost}c)" if cat == "shop" else "quest reward"
            self.bg_status.config(
                text=f"Saved {name}.aqbg ({kb:.1f} KB) as a {where} background, "
                     f"{len(self.bg_glow)} glow colors. Flash with: pio run --target uploadfs")
        except Exception as e:
            self.bg_status.config(text=f"Save error: {e}")

    def _section(self, title):
        box = tk.LabelFrame(self.ctrl, text=title, fg=TEXT, bg=DARK,
                            font=("TkDefaultFont", 9, "bold"), bd=1,
                            relief="groove", labelanchor="nw")
        box.pack(fill="x", padx=10, pady=6)
        return box

    def _build_connection_panel(self):
        box = self._section("1 - DEVICE CONNECTION")
        row = tk.Frame(box, bg=DARK); row.pack(fill="x", padx=8, pady=6)
        self.conn_dot = tk.Canvas(row, width=16, height=16, bg=DARK,
                                  highlightthickness=0)
        self.conn_dot.pack(side="left")
        self._set_dot("#777")
        tk.Label(row, textvariable=self.port_var, fg=TEXT, bg=DARK,
                 anchor="w").pack(side="left", padx=6)
        tk.Button(box, text="Rescan ports", command=self._scan_ports,
                  bg=PANEL, fg=TEXT, relief="flat").pack(fill="x", padx=8,
                                                         pady=(0, 6))
        if not HAVE_SERIAL:
            tk.Label(box, text="(pip install pyserial for auto-detect)",
                     fg="#caa", bg=DARK, font=("TkDefaultFont", 8)).pack(
                         padx=8, pady=(0, 4))
        pio_txt = "PlatformIO: found" if self.pio else "PlatformIO: NOT FOUND"
        tk.Label(box, text=pio_txt, fg=("#9e9" if self.pio else "#f88"),
                 bg=DARK, font=("TkDefaultFont", 8)).pack(padx=8, pady=(0, 6),
                                                          anchor="w")
        # Isolation status: is the project-local venv + core dir in use?
        isolated = _venv_pio().exists()
        iso_txt = ("Build env: ISOLATED (project-local)" if isolated
                   else "Build env: shared/system (not isolated)")
        self.iso_lbl = tk.Label(box, text=iso_txt,
                                fg=("#9e9" if isolated else "#ec8"),
                                bg=DARK, font=("TkDefaultFont", 8))
        self.iso_lbl.pack(padx=8, pady=(0, 2), anchor="w")
        tk.Label(box,
                 text=("Isolates PlatformIO + toolchains into this project so "
                       "other\nPlatformIO projects can't change its versions."),
                 fg=SUBTLE, bg=DARK, font=("TkDefaultFont", 8),
                 justify="left").pack(padx=8, anchor="w")
        tk.Button(box,
                  text=("Re-create isolated build env" if isolated
                        else "Set up isolated build env (recommended)"),
                  command=self._setup_isolated_env,
                  bg=PANEL, fg=ACCENT, relief="flat").pack(fill="x", padx=8,
                                                           pady=(2, 6))

    def _setup_isolated_env(self):
        # Build the venv + install pinned PlatformIO on a worker thread, logging
        # progress; then re-point self.pio at the isolated pio.
        def worker():
            def log(m): self.log_q.put(("info", m))
            self.log_q.put(("info", "\n=== Setting up isolated build environment ===\n"))
            pio = bootstrap_venv(log=log)
            if pio:
                self.pio = [str(pio)]
                self.log_q.put(("ok", "Isolated build environment ready.\n"))
                self.log_q.put(("__iso_done__", True))
            else:
                self.log_q.put(("err", "Could not set up the isolated environment.\n"))
                self.log_q.put(("__iso_done__", False))
        threading.Thread(target=worker, daemon=True).start()

    def _set_dot(self, color):
        self.conn_dot.delete("all")
        self.conn_dot.create_oval(2, 2, 14, 14, fill=color, outline="")

    def _build_color_panel(self):
        box = self._section("2 - THEME COLORS")
        self.swatches = {}
        for label, attr in [("Primary (teal)", "color_teal"),
                            ("Secondary (purple)", "color_purple"),
                            ("Accent (lime)", "color_lime")]:
            row = tk.Frame(box, bg=DARK); row.pack(fill="x", padx=8, pady=3)
            tk.Label(row, text=label, fg=TEXT, bg=DARK, width=16,
                     anchor="w").pack(side="left")
            sw = tk.Frame(row, width=26, height=20, bg=getattr(self, attr),
                          highlightthickness=1, highlightbackground="white")
            sw.pack(side="left", padx=4); sw.pack_propagate(False)
            self.swatches[attr] = sw
            tk.Button(row, text="Pick", command=lambda a=attr: self._pick_color(a),
                      bg=PANEL, fg=TEXT, relief="flat").pack(side="left")

    def _build_intro_panel(self):
        box = self._section("3 - INTRO SCREEN")
        for i, line in enumerate(self.lines):
            sub = tk.Frame(box, bg=DARK); sub.pack(fill="x", padx=8, pady=2)
            tk.Label(sub, text=f"Line {i+1}", fg=TEXT, bg=DARK, width=6,
                     anchor="w").pack(side="left")
            e = tk.Entry(sub, textvariable=line["text"], width=18)
            e.pack(side="left")
            line["text"].trace_add("write", lambda *_: self._mark_dirty())
            tk.Spinbox(sub, from_=8, to=60, width=4,
                       textvariable=line["size"]).pack(side="left", padx=4)
            line["size"].trace_add("write", lambda *_: self._mark_dirty())
            tk.Button(sub, text="font", width=4,
                      command=lambda L=line: self._pick_ttf(L),
                      bg=PANEL, fg=TEXT, relief="flat").pack(side="left")
        urow = tk.Frame(box, bg=DARK); urow.pack(fill="x", padx=8, pady=4)
        tk.Label(urow, text="URL", fg=TEXT, bg=DARK, width=6,
                 anchor="w").pack(side="left")
        tk.Entry(urow, textvariable=self.url_var, width=26).pack(side="left")

    def _build_modes_panel(self):
        box = self._section("4 - MENU MODES  (check = include, arrows = reorder)")
        tk.Label(box, text="Order here = order on the device menu.",
                 fg="#aaa", bg=DARK, font=("TkDefaultFont", 8)).pack(
                     padx=8, pady=(2, 2), anchor="w")
        self.modes_holder = tk.Frame(box, bg=DARK)
        self.modes_holder.pack(fill="x", padx=4)
        self._render_modes_list()

    def _render_modes_list(self):
        # Rebuild the rows to reflect the current mode_order
        for w in self.modes_holder.winfo_children():
            w.destroy()
        for pos, flag in enumerate(self.mode_order):
            row = tk.Frame(self.modes_holder, bg=PANEL)
            row.pack(fill="x", padx=4, pady=1)
            tk.Checkbutton(row, variable=self.mode_vars[flag],
                           bg=PANEL, fg=TEXT, selectcolor=DARK,
                           activebackground=PANEL, activeforeground=TEXT,
                           command=self._mark_dirty).pack(side="left")
            tk.Label(row, text=self.mode_label[flag], fg=TEXT, bg=PANEL,
                     anchor="w", width=18).pack(side="left", fill="x", expand=True)
            # Up / down reorder buttons
            up = tk.Button(row, text="\u25B2", width=2, relief="flat",
                           bg=DARK, fg=TEXT,
                           command=lambda p=pos: self._move_mode(p, -1))
            up.pack(side="right", padx=1)
            dn = tk.Button(row, text="\u25BC", width=2, relief="flat",
                           bg=DARK, fg=TEXT,
                           command=lambda p=pos: self._move_mode(p, 1))
            dn.pack(side="right", padx=1)
            if pos == 0:
                up.config(state="disabled")
            if pos == len(self.mode_order) - 1:
                dn.config(state="disabled")

    def _move_mode(self, pos, delta):
        new = pos + delta
        if 0 <= new < len(self.mode_order):
            self.mode_order[pos], self.mode_order[new] = \
                self.mode_order[new], self.mode_order[pos]
            self._render_modes_list()
            self._mark_dirty()

    def _build_topics_panel(self):
        box = self._section("5 - RABBIT HOLE TOPICS")
        brow = tk.Frame(box, bg=DARK); brow.pack(fill="x", padx=8, pady=2)
        tk.Button(brow, text="All", command=lambda: self._set_all_topics(True),
                  bg=PANEL, fg=TEXT, relief="flat", width=6).pack(side="left")
        tk.Button(brow, text="None", command=lambda: self._set_all_topics(False),
                  bg=PANEL, fg=TEXT, relief="flat", width=6).pack(side="left",
                                                                  padx=4)
        for bit, label in RABBIT_TOPICS:
            tk.Checkbutton(box, text=label, variable=self.topic_vars[bit],
                           fg=TEXT, bg=DARK, selectcolor=PANEL,
                           activebackground=DARK, activeforeground=TEXT,
                           anchor="w").pack(fill="x", padx=8, anchor="w")

    def _build_guides_panel(self):
        box = self._section("5b - GUIDEBOOKS GUIDES")
        brow = tk.Frame(box, bg=DARK); brow.pack(fill="x", padx=8, pady=(0, 4))
        tk.Label(brow, text="Included in the GuideBooks app:",
                 fg=SUBTLE, bg=DARK).pack(side="left")
        tk.Button(brow, text="All", command=lambda: self._set_all_guides(True),
                  bg=PANEL, fg=TEXT, relief="flat", width=6).pack(side="right")
        tk.Button(brow, text="None", command=lambda: self._set_all_guides(False),
                  bg=PANEL, fg=TEXT, relief="flat", width=6).pack(side="right",
                                                                  padx=4)
        for bit, label in GUIDES:
            tk.Checkbutton(box, text=label, variable=self.guide_vars[bit],
                           fg=TEXT, bg=DARK, selectcolor=PANEL,
                           activebackground=DARK, activeforeground=TEXT,
                           anchor="w").pack(fill="x", padx=8, anchor="w")

    def _build_config_panel(self):
        box = self._section("6 - DEFAULT CONFIG (optional)")
        for label, var, show in [("WiFi SSID", self.cfg_ssid, None),
                                 ("WiFi Pass", self.cfg_pass, "*"),
                                 ("BTC Addr",  self.cfg_btc, None),
                                 ("BCH Addr",  self.cfg_bch, None),
                                 ("DGB Addr",  self.cfg_dgb, None),
                                 ("Pool Host", self.cfg_pool, None),
                                 ("Zip Code",  self.cfg_zip, None)]:
            row = tk.Frame(box, bg=DARK); row.pack(fill="x", padx=8, pady=2)
            tk.Label(row, text=label, fg=TEXT, bg=DARK, width=9,
                     anchor="w").pack(side="left")
            tk.Entry(row, textvariable=var, width=24,
                     show=show).pack(side="left")

        # Timezone dropdown (for the BTC miner clock + future clock modes)
        trow = tk.Frame(box, bg=DARK); trow.pack(fill="x", padx=8, pady=(6, 2))
        tk.Label(trow, text="Timezone", fg=TEXT, bg=DARK, width=9,
                 anchor="w").pack(side="left")
        tz_names = [tz[0] for tz in TIMEZONES]
        self.tz_combo = ttk.Combobox(trow, values=tz_names, width=22,
                                     state="readonly")
        self.tz_combo.current(self.cfg_tz_idx.get())
        self.tz_combo.pack(side="left")
        self.tz_combo.bind("<<ComboboxSelected>>",
                           lambda e: self.cfg_tz_idx.set(self.tz_combo.current()))

        tk.Label(box, text="Blank fields = set up on the device later.",
                 fg="#aaa", bg=DARK, font=("TkDefaultFont", 8)).pack(
                     padx=8, pady=(2, 4), anchor="w")

    def _build_sdk_panel(self):
        box = self._section("APP SDK / STUDIO")

        tk.Label(box, text="Firmware reflection + app scaffolding",
                 fg=TEXT, bg=DARK, font=("TkDefaultFont", 8)).pack(anchor="w", padx=4)

        row = tk.Frame(box, bg=DARK); row.pack(fill="x", pady=4)
        tk.Button(row, text="New App\u2026", command=self._sdk_new_app,
                  bg=PANEL, fg=TEXT, relief="flat", padx=10).pack(side="left", padx=2)
        tk.Button(row, text="Refresh API", command=self._sdk_refresh_api,
                  bg=PANEL, fg=TEXT, relief="flat", padx=10).pack(side="left", padx=2)
        tk.Button(row, text="API Browser", command=self._sdk_show_api,
                  bg=PANEL, fg=TEXT, relief="flat", padx=10).pack(side="left", padx=2)

        # Live summary of what the firmware reports it can do.
        self.sdk_summary = tk.Label(box, text="", fg=ACCENT, bg=DARK,
                                    justify="left", font=("TkDefaultFont", 8))
        self.sdk_summary.pack(anchor="w", padx=4, pady=2)
        self._sdk_load_summary()

    def _sdk_manifest_path(self):
        return ROOT / "tools" / "firmware_api.json"

    def _sdk_load_summary(self):
        p = self._sdk_manifest_path()
        if not p.exists():
            self.sdk_summary.config(text="No manifest yet \u2014 click Refresh API.")
            return
        try:
            m = json.loads(p.read_text(encoding="utf-8"))
            apps = m.get("apps", [])
            cats = {}
            for a in apps:
                cats[a.get("category", "?")] = cats.get(a.get("category", "?"), 0) + 1
            scaff = sum(1 for a in apps if a.get("scaffoldable"))
            cat_str = ", ".join(f"{k}:{v}" for k, v in sorted(cats.items()))
            self.sdk_summary.config(
                text=f"{len(apps)} apps ({scaff} editable)\n{cat_str}")
        except Exception as e:
            self.sdk_summary.config(text=f"manifest error: {e}")

    def _sdk_refresh_api(self):
        gen = ROOT / "tools" / "gen_api.py"
        try:
            r = subprocess.run([sys.executable, str(gen)],
                               capture_output=True, text=True, cwd=str(ROOT))
            self._log(r.stdout or "", None)
            if r.returncode != 0:
                self._log(r.stderr or "gen_api failed\n", "err")
        except Exception as e:
            self._log(f"refresh failed: {e}\n", "err")
        self._sdk_load_summary()

    def _sdk_new_app(self):
        appid = simpledialog.askstring("New App", "App id (lowercase, e.g. moonphase):",
                                       parent=self.root)
        if not appid:
            return
        appid = appid.strip().lower()
        if not appid.isidentifier():
            messagebox.showerror("New App", "Id must be letters/digits/underscore.")
            return
        name = simpledialog.askstring("New App", "Display name (e.g. MOON PHASE):",
                                      parent=self.root) or appid.upper()
        cat = simpledialog.askstring(
            "New App", "Category (game/livedata/creative/guide/system):",
            parent=self.root) or "system"
        gen = ROOT / "tools" / "new_app.py"
        try:
            r = subprocess.run([sys.executable, str(gen), appid, name, cat],
                               capture_output=True, text=True, cwd=str(ROOT))
            self._log(r.stdout or "", None)
            if r.returncode != 0:
                self._log(r.stderr or "new_app failed\n", "err")
                messagebox.showerror("New App", r.stderr or r.stdout or "failed")
            else:
                messagebox.showinfo(
                    "New App",
                    f"Created src/{appid}/. See the log for the registration "
                    "steps, then click Refresh API.")
        except Exception as e:
            self._log(f"new_app failed: {e}\n", "err")

    def _sdk_show_api(self):
        p = self._sdk_manifest_path()
        if not p.exists():
            messagebox.showinfo("API Browser", "Click Refresh API first.")
            return
        try:
            m = json.loads(p.read_text(encoding="utf-8"))
        except Exception as e:
            messagebox.showerror("API Browser", str(e)); return
        win = tk.Toplevel(self.root); win.title("Firmware API Browser"); win.configure(bg=DARK)
        txt = tk.Text(win, bg=PANEL, fg=TEXT, width=64, height=34,
                      relief="flat", wrap="word")
        txt.pack(fill="both", expand=True, padx=8, pady=8)
        dev = m.get("device", {})
        txt.insert("end", f"DEVICE: {dev.get('board','?')}  {dev.get('display','?')}\n")
        txt.insert("end", f"flash {dev.get('flash','?')}  psram {dev.get('psram')}\n\n")
        txt.insert("end", "APPS:\n")
        for a in m.get("apps", []):
            tag = "" if a.get("scaffoldable") else "  (built-in)"
            txt.insert("end", f"  \u2022 {a['name']}  [{a['category']}]{tag}\n")
        txt.insert("end", "\nAPI SURFACE:\n")
        for group, calls in m.get("api", {}).items():
            txt.insert("end", f"\n{group}.\n")
            for c in calls:
                txt.insert("end", f"   {c['sig']}\n      {c['doc']}\n")
        txt.config(state="disabled")

    def _build_action_panel(self):
        box = tk.Frame(self.ctrl, bg=DARK)
        box.pack(fill="x", padx=10, pady=10)
        tk.Checkbutton(box, text="Also flash offline content (guides, stories + aquarium backgrounds)",
                       variable=self.flash_content, fg=TEXT, bg=DARK,
                       selectcolor=PANEL, activebackground=DARK,
                       activeforeground=TEXT, anchor="w").pack(fill="x", pady=2)
        self.progress = ttk.Progressbar(box, mode="determinate", maximum=100)
        self.progress.pack(fill="x", pady=4)
        self.upload_btn = tk.Button(
            box, text="⚡  BUILD & UPLOAD", bg=ACCENT, fg="white",
            font=("TkDefaultFont", 12, "bold"), relief="flat",
            command=self._on_upload, height=2)
        self.upload_btn.pack(fill="x", pady=4)
        tk.Button(box, text="Write config files only (no upload)",
                  command=self._write_files_only, bg=PANEL, fg=TEXT,
                  relief="flat").pack(fill="x", pady=2)
        self.status = tk.Label(box, text="", fg="#9fe89f", bg=DARK,
                               wraplength=380, justify="left", anchor="w")
        self.status.pack(fill="x", pady=4)

    # ----------------------------------------------------------- callbacks
    def _mark_dirty(self):
        self.bitmaps_dirty = True

    def _pick_color(self, attr):
        col = colorchooser.askcolor(color=getattr(self, attr),
                                    title="Pick color")
        if col and col[1]:
            setattr(self, attr, col[1].upper())
            self.swatches[attr].configure(bg=col[1])
            self._mark_dirty()

    def _pick_ttf(self, line):
        p = filedialog.askopenfilename(
            title="Pick TTF font",
            filetypes=[("TrueType", "*.ttf *.otf"), ("All", "*.*")])
        if p:
            line["ttf"].set(p)
            self._mark_dirty()

    def _set_all_topics(self, val):
        for v in self.topic_vars.values():
            v.set(val)

    # ----------------------------------------------------------- port scan
    def _scan_ports(self):
        if not HAVE_SERIAL:
            self.port_var.set("install pyserial to auto-detect")
            self._set_dot("#cc0")
            return
        found = None
        for p in list_ports.comports():
            vid = getattr(p, "vid", None)
            if vid in ESP_VIDS:
                found = p
                break
        if found is None:
            # Fall back to any port that looks like a USB serial device
            for p in list_ports.comports():
                if "USB" in (p.description or "").upper() or \
                   "CDC" in (p.description or "").upper():
                    found = p
                    break
        if found:
            self.detected_port = found.device
            self.port_var.set(f"ESP32-S3 on {found.device}")
            self._set_dot("#3c3")
        else:
            self.detected_port = None
            self.port_var.set("no device detected - plug in USB")
            self._set_dot("#c33")
        # Re-scan every 2.5s
        self.root.after(2500, self._scan_ports)

    # ----------------------------------------------------- bitmap + preview
    def _render_bitmaps(self):
        if not self.bitmaps_dirty:
            return
        self.bitmaps_cache = {}
        for line in self.lines:
            ttf = line["ttf"].get() or self.default_ttf
            txt = line["text"].get() or " "
            sz = max(6, int(line["size"].get() or 14))
            try:
                img = ID.render_text_bitmap(ttf, txt, sz)
            except Exception:
                img = Image.new("1", (1, 1), 0)
            self.bitmaps_cache[line["var"]] = img
        self.bitmaps_dirty = False

    def _shades(self):
        return ID.derive_shades(ID.hex_to_rgb(self.color_teal),
                                ID.hex_to_rgb(self.color_purple),
                                ID.hex_to_rgb(self.color_lime))

    def _enabled_mode_labels(self):
        return [self.mode_label[flag] for flag in self.mode_order
                if self.mode_vars[flag].get()]

    def _render_menu_preview(self, shades):
        """Render a mock of the on-device menu using the chosen colors."""
        img = Image.new("RGB", (W, H), shades["TEAL_DEEP"])
        from PIL import ImageDraw, ImageFont
        d = ImageDraw.Draw(img)
        try:
            font = ImageFont.truetype(ID.find_default_font() or "", 13)
            small = ImageFont.truetype(ID.find_default_font() or "", 10)
        except Exception:
            font = ImageFont.load_default(); small = font
        # Header band
        d.rectangle([0, 0, W, 28], fill=shades["TEAL_DARK"])
        d.text((W // 2, 14), "MAIN MENU", font=font, anchor="mm",
               fill=shades["LIME"])
        labels = self._enabled_mode_labels()
        if not labels:
            d.text((W // 2, H // 2), "(no modes selected)", font=small,
                   anchor="mm", fill=shades["PURPLE_LIGHT"])
        else:
            y = 44
            for i, label in enumerate(labels):
                sel = (i == 0)
                if sel:
                    d.rectangle([6, y - 2, W - 6, y + 20],
                                fill=shades["PURPLE"])
                    d.rectangle([6, y - 2, W - 6, y + 20], outline=shades["LIME"])
                d.text((W // 2, y + 9), label, font=font, anchor="mm",
                       fill=shades["LIME_PALE"] if sel else shades["TEAL_LIGHT"])
                y += 28
        # Footer hint
        d.rectangle([0, H - 24, W, H], fill=shades["TEAL_DARK"])
        d.text((W // 2, H - 12), "L/R move   hold = select", font=small,
               anchor="mm", fill=shades["LIME"])
        # Corner ticks in lime
        for (x0, y0, x1, y1) in [
            (0, 0, 14, 1), (0, 0, 1, 14), (W - 14, 0, W, 1), (W - 1, 0, W, 14),
            (0, H - 1, 14, H), (0, H - 14, 1, H),
            (W - 14, H - 1, W, H), (W - 1, H - 14, W, H)]:
            d.rectangle([x0, y0, x1, y1], fill=shades["LIME"])
        return img

    def _tick_preview(self):
        self._render_bitmaps()
        shades = self._shades()
        try:
            if self.preview_mode.get() == "menu":
                img = self._render_menu_preview(shades)
            else:
                img = ID.render_intro_frame(self.frame_idx, shades,
                                            self.bitmaps_cache,
                                            self.url_var.get())
        except Exception as e:
            img = Image.new("RGB", (W, H), (20, 20, 20))
            from PIL import ImageDraw
            ImageDraw.Draw(img).text((6, 6), f"preview err:\n{e}",
                                     fill=(255, 120, 120))
        big = img.resize((W * PREVIEW_SCALE, H * PREVIEW_SCALE), Image.NEAREST)
        self.preview_imgtk = ImageTk.PhotoImage(big)
        self.canvas.create_image(0, 0, anchor="nw", image=self.preview_imgtk)
        if self.preview_mode.get() == "intro":
            self.frame_idx = (self.frame_idx + 1) % 80
        self.root.after(60, self._tick_preview)

    def _restart_preview(self):
        self.frame_idx = 0

    # ----------------------------------------------------------- file write
    def _guide_mask(self):
        mask = 0
        for bit, _ in GUIDES:
            if self.guide_vars[bit].get():
                mask |= (1 << bit)
        return mask

    def _set_all_guides(self, val):
        for v in self.guide_vars.values():
            v.set(val)

    def _topic_mask(self):
        mask = 0
        for bit, _ in RABBIT_TOPICS:
            if self.topic_vars[bit].get():
                mask |= (1 << bit)
        return mask

    def _c_escape(self, s):
        return s.replace("\\", "\\\\").replace('"', '\\"')

    def _write_build_config(self):
        mask = self._topic_mask()
        tz = TIMEZONES[self.cfg_tz_idx.get()]
        # Map each mode flag to its (state enum, on-device label) for the macro
        flag_meta = {
            "ENABLE_PINBALL": ("STATE_PLAYING", "PINBALL BLASTER"),
            "ENABLE_POCKET":  ("STATE_POCKET",  "CORNER POCKET"),
            "ENABLE_SLOTS":   ("STATE_SLOTS",   "JACKS OR BETTER"),
            "ENABLE_MINER":   ("STATE_MINER",   "BTC MINER"),
            "ENABLE_WEATHER": ("STATE_WEATHER", "WEATHER REPORT"),
            "ENABLE_SPACE":   ("STATE_SPACE",   "SPACE WEATHER"),
            "ENABLE_RABBIT":  ("STATE_RABBIT",  "RABBIT HOLE"),
            "ENABLE_CYOA":    ("STATE_CYOA",    "STORY MODE"),
            "ENABLE_ROADHUNT": ("STATE_ROADHUNT", "ROAD HUNT"),
            "ENABLE_SIGHTINGS": ("STATE_SIGHTINGS", "SIGHTINGS"),
            "ENABLE_MANTISCANNON": ("STATE_MANTISCANNON", "MANTIS CANNON"),
            "ENABLE_MANTISSKATE": ("STATE_MANTISSKATE", "MANTIS SKATE"),
            "ENABLE_MANTISFISH": ("STATE_MANTISFISH", "MANTIS FISHING"),
            "ENABLE_DUNERIDER": ("STATE_DUNERIDER", "DUNE RIDER"),
            "ENABLE_MANTISPUTT": ("STATE_MANTISPUTT", "MANTIS PUTT"),
            "ENABLE_MANTISDROP": ("STATE_MANTISDROP", "MANTIS DROP"),
            "ENABLE_MANTISVADERS": ("STATE_MANTISVADERS", "MANTIS VADERS"),
            "ENABLE_MANTISPSYCHIC": ("STATE_MANTISPSYCHIC", "MANTIS PSYCHIC"),
            "ENABLE_RUBIKS": ("STATE_RUBIKS", "CUBE SOLVER"),
            "ENABLE_FIREWORKS": ("STATE_FIREWORKS", "FIREWORKS"),
            "ENABLE_MANTISCHAT": ("STATE_MANTISCHAT", "MANTIS CHAT"),
            "ENABLE_DANCEPARTY": ("STATE_DANCEPARTY", "DANCE PARTY"),
            "ENABLE_QUESTS":  ("STATE_QUESTS",  "QUESTS"),
            "ENABLE_SHARE":   ("STATE_SHARE",   "PINBALL SHARING"),
            "ENABLE_PARTY":   ("STATE_PARTY",   "PARTY GAME"),
            "ENABLE_PET":     ("STATE_PET",     "MANTIS PET"),
            "ENABLE_MOON":    ("STATE_MOON",    "MOON"),
            "ENABLE_NEO":     ("STATE_NEO",     "ASTEROIDS"),
            # Lightning + Wildfires combined into one Storms/Fires app.
            "ENABLE_CLICKER": ("STATE_CLICKER", "CLICK COUNTER"),
            "ENABLE_DICE":    ("STATE_DICE",    "DICE"),
            "ENABLE_AQUARIUM":("STATE_AQUARIUM", "AQUARIUM"),
            "ENABLE_FRUIT":   ("STATE_FRUIT",   "FRUIT FRENZY"),
            "ENABLE_HAM":     ("STATE_HAM",     "HAM RADIO"),
            "ENABLE_APOD":    ("STATE_APOD",    "NASA APOD"),
            "ENABLE_FLIGHT":  ("STATE_FLIGHT",  "FLIGHTS OVERHEAD"),
            "ENABLE_CANDLES": ("STATE_CANDLES", "CRYPTO CANDLES"),
            "ENABLE_VERSUS":  ("STATE_VERSUS",  "VERSUS PINBALL"),
            "ENABLE_GUIDEBOOKS":("STATE_GUIDEBOOKS","GUIDEBOOKS"),
        }
        lines = [
            "// build_config.h - GENERATED by installer.py. Do not edit by hand.",
            "#pragma once", ""]
        lines.append("// ---- Menu modes ----")
        for flag, _ in MENU_MODES:
            val = 1 if self.mode_vars[flag].get() else 0
            lines.append(f"#define {flag} {val}")
            # The combined Storms/Fires app is backed by both sub-modes, so the
        lines.append("")

        # Menu order: emit the enabled modes, in the user's chosen order, as a
        # macro that main.cpp expands into the MENU_ITEMS[] array. This is what
        # makes the installer's drag-to-reorder actually change the device menu.
        lines.append("// ---- Menu order (installer-defined) ----")
        lines.append("#define PINBALL_MENU_ORDER_DEFINED 1")
        entries = []
        for flag in self.mode_order:
            if self.mode_vars[flag].get():
                state, label = flag_meta[flag]
                entries.append(f'    {{ "{label}", {state} }},')
        # Setup is always present, pinned to the very bottom of the menu.
        entries.append('    { "SETUP", STATE_SETUP },')
        # Join with line-continuations. Each entry already ends in a comma, so
        # the macro expands to a valid (trailing-comma-OK) array initializer.
        macro = " \\\n".join(entries)
        lines.append("#define PINBALL_MENU_ITEMS \\")
        lines.append(macro)
        lines.append("")

        lines.append("// ---- Rabbit hole topic mask ----")
        lines.append(f"#define RABBIT_TOPIC_MASK 0x{mask:04X}u")
        lines.append("")
        lines.append("// ---- GuideBooks guide mask ----")
        gmask = self._guide_mask()
        lines.append(f"#define GUIDE_MASK 0x{gmask:04X}u")
        lines.append("")
        lines.append("// ---- Default config (seeded into NVS if empty) ----")
        lines.append(f'#define DEFAULT_WIFI_SSID "{self._c_escape(self.cfg_ssid.get())}"')
        lines.append(f'#define DEFAULT_WIFI_PASS "{self._c_escape(self.cfg_pass.get())}"')
        lines.append(f'#define DEFAULT_BTC_ADDR "{self._c_escape(self.cfg_btc.get())}"')
        lines.append(f'#define DEFAULT_BCH_ADDR "{self._c_escape(self.cfg_bch.get())}"')
        lines.append(f'#define DEFAULT_DGB_ADDR "{self._c_escape(self.cfg_dgb.get())}"')
        lines.append(f'#define DEFAULT_BTC_POOL "{self._c_escape(self.cfg_pool.get())}"')
        lines.append(f'#define DEFAULT_WX_ZIP "{self._c_escape(self.cfg_zip.get())}"')
        lines.append("")
        lines.append("// ---- Timezone (clock display) ----")
        lines.append(f"#define DEFAULT_TZ_OFFSET ({tz[1]})")
        lines.append(f"#define DEFAULT_TZ_DST {tz[2]}")
        lines.append(f'#define DEFAULT_TZ_LABEL "{tz[3]}"')
        lines.append(f'#define DEFAULT_TZ_LABEL_DST "{tz[4]}"')
        lines.append("")
        (SRC_DIR / "build_config.h").write_text("\n".join(lines))

    def _write_theme_and_intro(self):
        shades = self._shades()
        ID.write_theme_header(SRC_DIR / "theme_colors.h", shades,
                              self.url_var.get())
        packed_lines = []
        for line in self.lines:
            img = self.bitmaps_cache.get(line["var"])
            if img is None:
                img = Image.new("1", (1, 1), 0)
            packed, w, h, bpr = ID.pack_bitmap(img)
            packed_lines.append((line["var"], line["text"].get(),
                                 packed, w, h, bpr))
        ID.write_intro_bitmaps_header(SRC_DIR / "intro_bitmaps.h", packed_lines)

    def _write_all_files(self):
        self._render_bitmaps()
        self._write_build_config()
        self._write_theme_and_intro()

    def _write_files_only(self):
        try:
            self._write_all_files()
            self._log("Wrote build_config.h, theme_colors.h, intro_bitmaps.h\n",
                      "ok")
            self.status.config(text="Config files written to src/.",
                               fg="#9fe89f")
        except Exception as e:
            messagebox.showerror("Write failed", str(e))

    # ----------------------------------------------------------- upload
    def _on_upload(self):
        if self.uploading:
            return
        if not self.pio:
            messagebox.showerror(
                "PlatformIO not found",
                "Could not find the 'pio' command.\n\n"
                "Install PlatformIO Core and make sure it's on your PATH:\n"
                "  pip install platformio")
            return
        if not any(v.get() for v in self.mode_vars.values()):
            messagebox.showerror("No modes selected",
                                 "Enable at least one menu mode before building.")
            return
        try:
            self._write_all_files()
        except Exception as e:
            messagebox.showerror("Write failed", str(e))
            return
        self._log("\n=== Wrote config files ===\n", "info")
        self._log(f"Modes: {', '.join(self._enabled_mode_labels())}\n")
        self._log(f"Topic mask: 0x{self._topic_mask():04X}\n")
        self.uploading = True
        self.upload_btn.config(state="disabled", text="UPLOADING...")
        self.progress.config(mode="indeterminate")
        self.progress.start(12)
        self.status.config(text="Building & uploading...", fg="#cce")
        t = threading.Thread(target=self._upload_worker, daemon=True)
        t.start()

    def _upload_worker(self):
        # Two stages: (1) flash the LittleFS content packs (uploadfs) if the
        # checkbox is set, then (2) flash the firmware (upload).
        stages = []
        if self.flash_content.get():
            stages.append((list(self.pio) + ["run", "--target", "uploadfs"],
                           "Flashing content packs (filesystem)"))
        up = list(self.pio) + ["run", "--target", "upload"]
        if self.detected_port:
            up += ["--upload-port", self.detected_port]
        stages.append((up, "Flashing firmware"))

        for cmd, label in stages:
            self.log_q.put(("info", f"\n=== {label} ===\n$ {' '.join(cmd)}\n\n"))
            try:
                proc = subprocess.Popen(
                    cmd, cwd=str(ROOT), stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True, bufsize=1,
                    universal_newlines=True, env=isolated_env())
                for raw in iter(proc.stdout.readline, ""):
                    tag = None
                    low = raw.lower()
                    if "error" in low or "failed" in low:
                        tag = "err"
                    elif "success" in low or "[success]" in low:
                        tag = "ok"
                    self.log_q.put((tag, raw))
                proc.stdout.close()
                rc = proc.wait()
                if rc != 0:
                    self.log_q.put(("__done__", rc))
                    return
            except Exception as e:
                self.log_q.put(("err", f"\nLauncher error: {e}\n"))
                self.log_q.put(("__done__", 1))
                return
        self.log_q.put(("__done__", 0))

    def _drain_log(self):
        try:
            while True:
                tag, payload = self.log_q.get_nowait()
                if tag == "__done__":
                    self._upload_finished(payload)
                elif tag == "__iso_done__":
                    # refresh the isolation status label
                    if payload and hasattr(self, "iso_lbl"):
                        self.iso_lbl.config(text="Build env: ISOLATED (project-local)",
                                            fg="#9e9")
                else:
                    self._log(payload, tag)
        except queue.Empty:
            pass
        self.root.after(80, self._drain_log)

    def _upload_finished(self, rc):
        self.uploading = False
        self.progress.stop()
        self.progress.config(mode="determinate")
        self.upload_btn.config(state="normal", text="⚡  BUILD & UPLOAD")
        if rc == 0:
            self.progress["value"] = 100
            self._log("\n=== UPLOAD COMPLETE ===\n", "ok")
            self.status.config(text="Done! Your device is flashed. 🎉",
                               fg="#9fe89f")
            self._success_popup()
        else:
            self.progress["value"] = 0
            self._log(f"\n=== FAILED (exit {rc}) ===\n", "err")
            self.status.config(
                text="Upload failed - check the log on the right.",
                fg="#ff9090")

    def _success_popup(self):
        win = tk.Toplevel(self.root)
        win.title("Success!")
        win.configure(bg=DARK)
        win.geometry("340x240")
        win.transient(self.root)
        cv = tk.Canvas(win, width=340, height=180, bg=DARK,
                       highlightthickness=0)
        cv.pack()
        shades = self._shades()
        teal = self.color_teal; lime = self.color_lime; purple = self.color_purple
        # A little celebratory burst in the user's colors
        cx, cy = 170, 90
        for i, r in enumerate(range(70, 10, -14)):
            col = [lime, teal, purple, lime][i % 4]
            cv.create_oval(cx - r, cy - r, cx + r, cy + r, outline=col, width=3)
        cv.create_text(cx, cy, text="✓", font=("TkDefaultFont", 40, "bold"),
                       fill=lime)
        cv.create_text(cx, 150, text="FLASHED & READY", fill=lime,
                       font=("TkDefaultFont", 14, "bold"))
        tk.Button(win, text="Nice!", command=win.destroy, bg=ACCENT,
                  fg="white", relief="flat",
                  font=("TkDefaultFont", 11, "bold")).pack(pady=10, padx=40,
                                                           fill="x")

    # ----------------------------------------------------------- log helper
    def _log(self, text, tag=None):
        self.logbox.config(state="normal")
        if tag:
            self.logbox.insert("end", text, tag)
        else:
            self.logbox.insert("end", text)
        self.logbox.see("end")
        self.logbox.config(state="disabled")


def main():
    root = tk.Tk()
    # Use a dark-ish ttk theme where available
    try:
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TProgressbar", troughcolor=PANEL,
                        background="#7a2a7a")
    except Exception:
        pass
    Installer(root)
    root.mainloop()


if __name__ == "__main__":
    main()

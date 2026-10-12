#!/usr/bin/env python3
"""gen_api.py - Firmware reflection: parse the App registry and emit a manifest.

This reads src/sdk/app_registry.cpp (the single source of truth for Apps) and
writes tools/firmware_api.json, which the studio/installer reads so its app
list, scaffolder, and docs stay in sync with the firmware automatically. It
also parses the firmware API headers to expose the available subsystem calls
(screen/storage/network style) for autocomplete and docs.

No external dependencies; pure stdlib so it runs anywhere the installer does.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
REGISTRY = os.path.join(SRC, "sdk", "app_registry.cpp")
OUT = os.path.join(ROOT, "tools", "firmware_api.json")

CAT_NAMES = {
    "APPCAT_GAME": "Game", "APPCAT_LIVEDATA": "Live Data", "APPCAT_CREATIVE": "Creative",
    "APPCAT_GUIDE": "Guide", "APPCAT_SYSTEM": "System",
}

# Match a registry row:
#   { "weather", "WEATHER REPORT", APPCAT_LIVEDATA, A_WEATHER, weatherEnter, ... , 0 },
ROW_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(APPCAT_\w+)\s*,\s*(A_\w+)\s*,'
    r'\s*([\w:]+|nullptr)\s*,\s*([\w:]+|nullptr)\s*,'
    r'\s*([\w:]+|nullptr)\s*,\s*([\w:]+|nullptr)\s*,\s*(\d)\s*\}'
)

def parse_registry():
    apps = []
    if not os.path.exists(REGISTRY):
        return apps
    text = open(REGISTRY, encoding="utf-8", errors="replace").read()
    for m in ROW_RE.finditer(text):
        appid, name, cat, state, en, ti, dr, sh, inline = m.groups()
        apps.append({
            "id": appid,
            "name": name,
            "category": CAT_NAMES.get(cat, "Other"),
            "state": state,
            "lifecycle": {
                "enter": None if en == "nullptr" else en,
                "tick": None if ti == "nullptr" else ti,
                "draw": None if dr == "nullptr" else dr,
                "shutdown": None if sh == "nullptr" else sh,
            },
            "inlineDispatch": inline == "1",
            "scaffoldable": inline == "0",
        })
    return apps

# Subsystem API surface the studio exposes for autocomplete/docs. This is the
# curated, firmware-specific API (not generic Arduino). Grows as the SDK does.
def api_surface():
    return {
        "screen": [
            {"sig": "fb.fillSprite(color)", "doc": "Clear the frame buffer to a color."},
            {"sig": "fb.pushSprite(0,0)", "doc": "Blit the frame buffer to the display."},
            {"sig": "fb.drawString(text,x,y,font)", "doc": "Draw text at x,y."},
            {"sig": "fb.fillRoundRect(x,y,w,h,r,color)", "doc": "Filled rounded rectangle."},
            {"sig": "fb.setTextColor(color)", "doc": "Set text color (theme_colors.h palette)."},
            {"sig": "fb.setTextDatum(datum)", "doc": "Text anchor (ML_DATUM, MC_DATUM, TR_DATUM...)."},
        ],
        "storage": [
            {"sig": "Preferences p; p.begin(ns,ro)", "doc": "Open an NVS namespace."},
            {"sig": "p.putBytes(key,&v,sizeof v)", "doc": "Persist a POD blob."},
            {"sig": "p.getBytes(key,&v,sizeof v)", "doc": "Load a POD blob."},
            {"sig": "LittleFS.open(path,\"r\")", "doc": "Open a content pack from flash."},
        ],
        "network": [
            {"sig": "WiFiClientSecure c; c.connect(host,443)", "doc": "TLS fetch (one at a time; ~40KB heap)."},
            {"sig": "WiFi.softAP(name)", "doc": "Start a captive-portal AP (see weather/share)."},
            {"sig": "esp_now_send(peer,buf,len)", "doc": "ESP-NOW peer message (see versus/pet)."},
        ],
        "profile": [
            {"sig": "qpAddXp(n)", "doc": "Grant player XP (quest profile)."},
            {"sig": "qpAddItem(id,n) / qpUseItem(id)", "doc": "Shared inventory."},
            {"sig": "qpGrantPetXp(n)", "doc": "Deposit pet XP (consumed by Pet Mode)."},
            {"sig": "qpGrantPerk(PERK_*)", "doc": "Queue a pinball perk for next game."},
        ],
        "lifecycle": [
            {"sig": "void xEnter()", "doc": "Called once when the app opens."},
            {"sig": "void xTick()", "doc": "Called every frame: read btnL/btnR edges, update state."},
            {"sig": "void xDraw()", "doc": "Called every frame: render to fb, then fb.pushSprite(0,0)."},
            {"sig": "void xShutdown()", "doc": "Called once when leaving; free resources, save state."},
        ],
    }

def device():
    return {
        "board": "LilyGo T-Display-S3",
        "mcu": "ESP32-S3",
        "display": "170x320 ST7789",
        "psram": False,
        "flash": "16MB",
        "ram_note": "~320KB; frame buffer sprite ~108KB; budget RAM carefully",
        "buttons": {"left": "GPIO0", "right": "GPIO14"},
        "global_gestures": {
            "hold_both_3s": "sleep (reserved, never override)",
            "double_click_both": "menu (reserved, never override)",
            "single_tap_L": "per-app (convention: scroll/advance)",
            "single_tap_R": "per-app (convention: select/confirm)"
        }
    }

def main():
    manifest = {
        "schema": 1,
        "generatedBy": "tools/gen_api.py",
        "device": device(),
        "apps": parse_registry(),
        "api": api_surface(),
    }
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    print(f"wrote {OUT}  ({len(manifest['apps'])} apps)")

if __name__ == "__main__":
    main()

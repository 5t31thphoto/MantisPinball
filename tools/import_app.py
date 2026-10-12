#!/usr/bin/env python3
"""import_app.py - Import an LLM-generated app into the firmware + manifest.

Takes a text file (or stdin) containing the model's output in the format the
gen_prompt.py prompt requests:

    ```cpp
    // FILE: src/<id>/<id>_mode.h
    ...
    ```
    ```cpp
    // FILE: src/<id>/<id>_mode.cpp
    ...
    ```
    ```json
    { "id": "...", "name": "...", "category": "..." }
    ```

It extracts each FILE: block, writes the files under src/, performs the 7
registration edits (AppState enum, include, fallback menu, dispatch, exit guard,
shutdown guard, build flag, registry row), and refreshes firmware_api.json.

Safe by design: it validates braces balance and that the four lifecycle
functions exist before writing anything, and refuses to overwrite an existing
app dir unless --force is given.

Usage:
  python3 tools/import_app.py path/to/model_output.txt
  python3 tools/import_app.py --stdin  < model_output.txt
  python3 tools/import_app.py output.txt --dry-run
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
MAIN = os.path.join(SRC, "main.cpp")
BUILDCFG = os.path.join(SRC, "build_config.h")
REGISTRY = os.path.join(SRC, "sdk", "app_registry.cpp")

CAT_CONST = {"game": "CAT_GAME", "livedata": "CAT_LIVEDATA",
             "creative": "CAT_CREATIVE", "guide": "CAT_GUIDE", "system": "CAT_SYSTEM"}

FILE_RE = re.compile(r"//\s*FILE:\s*(\S+)\s*\n(.*?)(?=\n```|\Z)", re.DOTALL)
JSON_RE = re.compile(r"```json\s*(\{.*?\})\s*```", re.DOTALL)


def braces_balanced(code):
    d = 0; instr = False; sc = None; inc = False; inb = False; i = 0
    while i < len(code):
        c = code[i]
        if inc:
            if c == "\n": inc = False
            i += 1; continue
        if inb:
            if c == "*" and i + 1 < len(code) and code[i+1] == "/": inb = False; i += 2; continue
            i += 1; continue
        if instr:
            if c == "\\": i += 2; continue
            if c == sc: instr = False
            i += 1; continue
        if c == "/" and i + 1 < len(code) and code[i+1] == "/": inc = True; i += 2; continue
        if c == "/" and i + 1 < len(code) and code[i+1] == "*": inb = True; i += 2; continue
        if c in ('"', "'"): instr = True; sc = c; i += 1; continue
        if c == "{": d += 1
        elif c == "}": d -= 1
        i += 1
    return d == 0


def extract(text):
    files = {}
    for m in FILE_RE.finditer(text):
        path = m.group(1).strip()
        body = m.group(2)
        # strip a leading ``` if the FILE: line was inside a fence already handled
        files[path] = body.rstrip() + "\n"
    meta = {}
    jm = JSON_RE.search(text)
    if jm:
        try:
            meta = json.loads(jm.group(1))
        except Exception:
            meta = {}
    return files, meta


def edit_main(appid, name, flag, state):
    src = open(MAIN, encoding="utf-8").read()
    A = "A_" + appid.upper()  # not needed in main, only registry
    ok = {}

    inc = f'#include "{appid}/{appid}_mode.h"'
    if inc not in src:
        src = src.replace('#include "sdk/app_sdk.h"',
                          f'#include "sdk/app_sdk.h"\n{inc}', 1)
    ok["include"] = inc in src

    if f"{state}," not in src:
        src = src.replace("    STATE_GAMEOVER",
                          f"    {state},\n    STATE_GAMEOVER", 1)
    ok["enum"] = f"{state}," in src

    menu_row = '#if ' + flag + '\n    { "' + name + '", ' + state + ' },\n#endif'
    if menu_row not in src:
        # insert before the closing of the fallback MENU_ITEMS array
        anchor = "#endif\n};\n#endif  // PINBALL_MENU_ORDER_DEFINED"
        repl = menu_row + "\n#endif\n};\n#endif  // PINBALL_MENU_ORDER_DEFINED"
        src = src.replace(anchor, repl, 1)
    ok["menu"] = flag in src and state in src

    sel = f"case {state}:"
    if f"{appid}Enter();" not in src:
        # add to the select dispatch (after the last case X: xEnter(); break;)
        src = re.sub(r"(\n( +)case STATE_PET:\s+petEnter\(\);\s+break;)",
                     rf"\1\n\2case {state}: {appid}Enter(); break;", src, count=1)
    ok["select"] = f"{appid}Enter();" in src

    if f"{appid}Tick();" not in src:
        src = re.sub(r"(\n( +)case STATE_PET:\n\2    petTick\(\);\n\2    petDraw\(\);\n\2    break;)",
                     rf"\1\n\n\2case {state}:\n\2    {appid}Tick();\n\2    {appid}Draw();\n\2    break;",
                     src, count=1)
    ok["tickdraw"] = f"{appid}Tick();" in src

    if f"appState == {state})" not in src:
        src = src.replace("         appState == STATE_PET)) {",
                          f"         appState == STATE_PET    ||\n         appState == {state})) {{", 1)
    ok["exit"] = f"appState == {state})" in src

    if f"if (appState == {state})" not in src:
        src = src.replace("        if (appState == STATE_PET)      petShutdown();",
                          f"        if (appState == STATE_PET)      petShutdown();\n        if (appState == {state})      {appid}Shutdown();", 1)
    ok["shutdown"] = f"if (appState == {state})" in src

    open(MAIN, "w", encoding="utf-8").write(src)
    return ok


def edit_build_config(flag):
    src = open(BUILDCFG, encoding="utf-8").read()
    if f"#ifndef {flag}" not in src:
        block = f"#ifndef {flag}\n#define {flag}     1\n#endif\n"
        # add after ENABLE_PET if present, else before the last #endif
        if "#ifndef ENABLE_PET" in src:
            src = re.sub(r"(#ifndef ENABLE_PET\n#define ENABLE_PET +1\n#endif\n)",
                         rf"\1{block}", src, count=1)
        else:
            src = src + "\n" + block
        open(BUILDCFG, "w", encoding="utf-8").write(src)
    return f"#ifndef {flag}" in open(BUILDCFG, encoding="utf-8").read()


def edit_registry(appid, name, cat_const):
    src = open(REGISTRY, encoding="utf-8").read()
    A = "A_" + appid.upper()
    # forward declarations
    fdecl = (f"extern void {appid}Enter();   extern void {appid}Tick();   "
             f"extern void {appid}Draw();   extern void {appid}Shutdown();")
    if fdecl not in src and f"{appid}Enter()" not in src:
        src = src.replace("extern void petEnter();      extern void petTick();      extern void petDraw();      extern void petShutdown();",
                          "extern void petEnter();      extern void petTick();      extern void petDraw();      extern void petShutdown();\n" + fdecl, 1)
    # enum constant (append before A_GAMEOVER)
    if A not in src:
        src = src.replace("A_PET, A_GAMEOVER", f"A_PET, {A}, A_GAMEOVER", 1)
    # registry row (before the closing };)
    row = (f'    {{ "{appid}", "{name}", {cat_const}, {A}, '
           f'{appid}Enter, {appid}Tick, {appid}Draw, {appid}Shutdown, 0 }},')
    if f'"{appid}"' not in src:
        src = src.replace("};\nstatic const int REGISTRY_COUNT",
                          f"{row}\n}};\nstatic const int REGISTRY_COUNT", 1)
    open(REGISTRY, "w", encoding="utf-8").write(src)
    s2 = open(REGISTRY, encoding="utf-8").read()
    return f'"{appid}"' in s2 and A in s2


def main():
    args = [a for a in sys.argv[1:]]
    dry = "--dry-run" in args
    force = "--force" in args
    args = [a for a in args if not a.startswith("--")]
    if "--stdin" in sys.argv:
        text = sys.stdin.read()
    elif args:
        text = open(args[0], encoding="utf-8").read()
    else:
        print(__doc__); sys.exit(1)

    files, meta = extract(text)
    if not files:
        print("error: no '// FILE:' blocks found in the input."); sys.exit(1)
    if not meta.get("id"):
        # derive id from a file path
        for p in files:
            m = re.search(r"src/(\w+)/", p)
            if m: meta["id"] = m.group(1); break
    appid = meta.get("id")
    name = meta.get("name", appid.upper() if appid else "NEW APP")
    cat = meta.get("category", "system").lower()
    cat_const = CAT_CONST.get(cat, "CAT_SYSTEM")
    if not appid:
        print("error: could not determine app id."); sys.exit(1)

    # Validate every .cpp/.h block balances braces before touching disk.
    for path, body in files.items():
        if path.endswith((".cpp", ".h")) and not braces_balanced(body):
            print(f"error: braces not balanced in {path}; refusing to import."); sys.exit(1)
    # Validate the four lifecycle functions exist in the cpp.
    cpp = next((b for p, b in files.items() if p.endswith("_mode.cpp")), "")
    for fn in (f"{appid}Enter", f"{appid}Tick", f"{appid}Draw", f"{appid}Shutdown"):
        if fn not in cpp:
            print(f"error: {fn}() not found in the .cpp; refusing to import."); sys.exit(1)

    appdir = os.path.join(SRC, appid)
    if os.path.exists(appdir) and not force:
        print(f"error: src/{appid}/ already exists (use --force to overwrite)."); sys.exit(1)

    print(f"App: {name}  (id {appid}, category {cat})")
    print(f"Files: {', '.join(files.keys())}")
    if dry:
        print("--dry-run: validated OK, nothing written.")
        return

    # write files
    for path, body in files.items():
        full = os.path.join(ROOT, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        open(full, "w", encoding="utf-8").write(body)
        print(f"  wrote {path}")

    flag = "ENABLE_" + appid.upper()
    state = "STATE_" + appid.upper()
    mok = edit_main(appid, name, flag, state)
    bok = edit_build_config(flag)
    rok = edit_registry(appid, name, cat_const)
    for k, v in mok.items():
        print(f"  main.cpp {k}: {'OK' if v else 'FAILED'}")
    print(f"  build_config {flag}: {'OK' if bok else 'FAILED'}")
    print(f"  registry row: {'OK' if rok else 'FAILED'}")

    # refresh manifest
    try:
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_api.py")],
                       cwd=ROOT, check=False)
    except Exception:
        pass
    print("\nImported. Review the edits, then build/flash from the installer.")


if __name__ == "__main__":
    main()

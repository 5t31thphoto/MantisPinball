#!/usr/bin/env python3
"""Compile EVERY firmware source on the host (syntax + types + declaration
order), the way the ESP32 build would see it. Catches use-before-declaration,
name clashes with Arduino macros/typedefs and missing symbols before CI does.
    python tools/tests/compile_all.py
"""
import subprocess, os, sys
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
stubs = os.path.join(root, "tools", "tests")
bad, ok = {}, 0
for dp, _, fs in os.walk(os.path.join(root, "src")):
    for f in sorted(fs):
        if not f.endswith(".cpp"): continue
        p = os.path.join(dp, f)
        r = subprocess.run(["g++", "-std=gnu++17", "-fsyntax-only", "-w", "-DARDUINO=10814", "-DESP32",
                            "-I", os.path.join(stubs, "compile_stub"), "-I", os.path.join(stubs, "stub"),
                            "-I", os.path.join(root, "src"), "-I", os.path.join(root, "src", "rubiks"), p],
                           capture_output=True, text=True)
        errs = [l for l in r.stderr.splitlines() if "error:" in l]
        if errs: bad[os.path.relpath(p, root)] = errs
        else: ok += 1
print("compile clean: %d / %d" % (ok, ok + len(bad)))
for p, e in sorted(bad.items()):
    print("  %s\n    %s" % (p, "\n    ".join(e[:3])))
sys.exit(1 if bad else 0)

#!/usr/bin/env python3
# casefix.py - the WDK's headers #include each other with the case of a Windows file system ("DriverSpecs.h" for
# driverspecs.h). For every quoted or angled include in the given header trees and sources whose exact name does
# not exist but a case-insensitive match does, put a symlink with the included spelling into <shim-dir>.
import os, re, sys
shim, dirs = sys.argv[1], sys.argv[2:]
os.makedirs(shim, exist_ok=True)
index = {}
for d in dirs:
    for root, _, files in os.walk(d):
        for f in files:
            rel = os.path.relpath(os.path.join(root, f), d)
            index.setdefault(rel.lower(), os.path.join(root, f))
inc = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
names = set()
for d in dirs:
    for root, _, files in os.walk(d):
        for f in files:
            if f.lower().endswith(('.h', '.c', '.hpp', '.inl')) or '.' not in f:
                try:
                    data = open(os.path.join(root, f), 'rb').read()
                except OSError:
                    continue
                for m in inc.finditer(data):
                    names.add(m.group(1).decode('latin-1').replace('\\', '/'))
made = 0
for n in names:
    if any(os.path.exists(os.path.join(d, n)) for d in dirs):
        continue
    t = index.get(n.lower())
    if t is None:
        continue
    p = os.path.join(shim, n)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    if not os.path.lexists(p):
        os.symlink(t, p)
        made += 1
print(f"casefix: {made} links in {shim}")

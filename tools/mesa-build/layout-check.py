#!/usr/bin/env python3
"""layout-check.py - exynos-woa: the precompiled-kernel ABI check of linux-build-g76.sh, as a tool.

    layout-check.py <host-build-dir> <target-build-dir> <zig> <zig-target>

panfrost_compile (built for the Linux host) memcpys struct bifrost_precompiled_kernel_info (and the structs inside it)
into the generated C that the Windows target compiles, so host and target must lay those structs out byte for byte
alike. This takes the real compile commands of src/panfrost/clc/pan_compile.c (host) and
src/gallium/drivers/panfrost/pan_precomp.c (target), reruns both with -fdump-record-layouts-simple and compares every
record they share. Both must compile with 0 errors. It fails if the serialized struct or any record nested in it
differs; others (pointers on a 32-bit target, struct stat, util_cpu_caps) are only counted.
"""
import json, os, re, shlex, subprocess, sys

host_dir, tgt_dir, zig, ztarget = sys.argv[1:5]


def cmd_for(build, suffix):
    cc = json.load(open(os.path.join(build, 'compile_commands.json')))
    e = [x for x in cc if x['file'].endswith(suffix)]
    if not e:
        sys.exit(f'layout-check: no {suffix} in {build}/compile_commands.json')
    out, skip = [], False
    for a in shlex.split(e[0]['command']):
        if skip:
            skip = False
            continue
        if a in ('-o', '-MQ', '-MF'):
            skip = True
            continue
        if a == '-MD':
            continue
        out.append(a)
    return out, e[0]['directory']


def dump(cmd, cwd):
    # a real -c to a throwaway object: zig cc rejects -fsyntax-only here ("FileNotFound")
    obj = os.path.join(os.path.abspath(cwd), 'layout-check.o')
    # the nonce define keeps zig's compile cache from answering without running clang (no dump on a cache hit)
    r = subprocess.run(cmd + ['-o', obj, '-Xclang', '-fdump-record-layouts-simple', f'-DLAYOUT_CHECK_NONCE={os.urandom(6).hex()}'],
                       cwd=cwd, capture_output=True, text=True)
    if os.path.exists(obj):
        os.remove(obj)
    if r.returncode or re.search(r'\d+ errors? generated', r.stderr):
        sys.exit('layout-check: compile failed:\n' + r.stderr[:3000])
    recs = {}
    for blk in r.stdout.split('*** Dumping AST Record Layout')[1:]:
        m = re.search(r'Type: (.+)', blk)
        lay = re.search(r'Layout: <ASTRecordLayout.*?>', blk, re.S)
        if m and lay:
            recs[m.group(1).strip()] = re.sub(r'\s+', ' ', lay.group(0))
    return recs


hc, hd = cmd_for(host_dir, 'src/panfrost/clc/pan_compile.c')
tc, td = cmd_for(tgt_dir, 'src/gallium/drivers/panfrost/pan_precomp.c')
A, B = dump(hc, hd), dump(tc, td)
common = sorted(set(A) & set(B))
diff = [k for k in common if A[k] != B[k]]

# The serialized set: bifrost_precompiled_kernel_info and every record nested in it, read from the host's FULL
# layout dump (-fdump-record-layouts prints nested members with their types; -simple does not). Records outside that
# set (anything holding a pointer, on a 32-bit target) may differ: they never cross from panfrost_compile to the DLL.
def nested(cmd, cwd, root):
    obj = os.path.join(os.path.abspath(cwd), 'layout-check.o')
    r = subprocess.run(cmd + ['-o', obj, '-Xclang', '-fdump-record-layouts', f'-DLAYOUT_CHECK_NONCE={os.urandom(6).hex()}'],
                       cwd=cwd, capture_output=True, text=True)
    if os.path.exists(obj):
        os.remove(obj)
    for blk in r.stdout.split('*** Dumping AST Record Layout')[1:]:
        lines = blk.strip().splitlines()
        if lines and re.search(r'\|\s+' + re.escape(root) + r'$', lines[0]):
            return {re.sub(r'(\[\d+\])+$', '', f'{k} {n}') for k, n in re.findall(r'\|\s+(struct|union) (\S+(?: \S+)*?)(?: \(|$| \w+$)', blk, re.M)} | {root}
    sys.exit(f'layout-check: {root} not in the host dump')
ser = nested(hc, hd, 'struct bifrost_precompiled_kernel_info')
missing = [k for k in ser if k not in common and not k.startswith(('struct (', 'union ('))]
bad = [k for k in diff if k in ser] + [f'{k} (not in both)' for k in missing]
print(f'layout-check: {len(common)} common records, {len(ser)} serialized ({", ".join(sorted(ser))}); {len(diff)} others differ (not serialized)')
if bad:
    for k in bad:
        print(f'--- host {k}\n{A.get(k)}\n--- target {k}\n{B.get(k)}')
    sys.exit(f'layout-check: FAIL, records differ: {bad}')
print('layout-check: OK')

# check_tramp.py - proves isphost.c's code_block[] = tramp.S assembled (notes/s10-isp.md Stage 4 option A).
# Literal slots (the words isphost.c writes at run time) are compared as "don't care".
#   python3 -I check_tramp.py        (needs clang + llvm-objcopy for aarch64)
import os
import re
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
with tempfile.TemporaryDirectory() as t:
    o, b = os.path.join(t, 'tramp.o'), os.path.join(t, 'tramp.bin')
    subprocess.check_call(['clang', '--target=aarch64-linux-gnu', '-c', os.path.join(here, 'tramp.S'), '-o', o])
    subprocess.check_call(['llvm-objcopy', '-O', 'binary', '--only-section=.text', o, b])
    asm = open(b, 'rb').read()
    nm = subprocess.check_output(['llvm-nm', o]).decode()
syms = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
src = open(os.path.join(here, 'isphost.c')).read()
body = re.search(r'static const u32 code_block\[\] = \{(.*?)\};', src, re.S).group(1)
body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
words = [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\b0\b', body)]
lit = {0x44, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c, 0x60, 0xc8, 0xcc}      # literal / pad words (byte offsets)
bad = 0
if len(asm) != 4 * len(words):
    print('size differs: assembler %d bytes, code_block %d bytes' % (len(asm), 4 * len(words)))
    bad += 1
for i, w in enumerate(words):
    a = int.from_bytes(asm[4 * i:4 * i + 4], 'little')
    if 4 * i in lit:
        continue
    if a != w:
        print('+0x%02x: assembler 0x%08x, isphost.c 0x%08x' % (4 * i, a, w))
        bad += 1
src_off = {k: int(v, 16) for k, v in re.findall(r'#define OFF_(ENTER|TRAMP|COMMON) (0x[0-9a-f]+)', src)}
for k, sym in (('ENTER', 'isp_enter'), ('TRAMP', 'isp_tramp'), ('COMMON', 'isp_common')):
    if src_off.get(k) != syms.get(sym):
        print('OFF_%s 0x%x but %s at 0x%x' % (k, src_off.get(k, -1), sym, syms.get(sym, -1)))
        bad += 1
print('code_block: %d words, %d mismatches' % (len(words), bad))
sys.exit(1 if bad else 0)

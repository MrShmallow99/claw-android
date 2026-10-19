#!/usr/bin/env python3
"""tools/disasm.py ADDR [N] [--exe path] [--config ini] -- disassemble the patched image
(the exact code the recompiler saw), N instructions from ADDR."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'recomp'))
from codegen import load_patches
from x86image import Image
args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--'))
exe = opt.get('exe', os.path.join(os.path.dirname(__file__), '..', '..', 'game', 'claw', 'CLAW.exe'))
cfg = opt.get('config', os.path.join(os.path.dirname(__file__), '..', 'games', 'claw', 'game.ini'))
img = Image(exe)
for a, d in load_patches(cfg, exe):
    img.mem[a - img.base:a - img.base + len(d)] = d
a = int(args[0], 16)
n = int(args[1]) if len(args) > 1 else 40
for _ in range(n):
    i = img.decode(a)
    if not i:
        print('%08x: ??' % a); break
    print('%08x: %-24s %s %s' % (a, i.bytes.hex(), i.mnemonic, i.op_str))
    a += i.size

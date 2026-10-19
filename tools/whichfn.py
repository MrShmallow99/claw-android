#!/usr/bin/env python3
"""tools/whichfn.py ADDR -- the generated function(s) containing ADDR, and their direct callers."""
import re, glob, sys
a = int(sys.argv[1], 16)
lab = 'L_%x:' % a
fns, callers = [], {}
for f in sorted(glob.glob('gen/code_*.c')):
    cur = None
    for line in open(f):
        m = re.match(r'void (f_[0-9a-f]+)\(CPU', line)
        if m:
            cur = m.group(1)
            if cur == 'f_%06x' % a:
                fns.append(cur)
        if lab in line or ('/* %x:' % a) in line or ('PROBE(0x%xu)' % a) in line:
            fns.append(cur)
        for c in re.findall(r'(f_[0-9a-f]+)\(cpu\)', line):
            if c != cur:
                callers.setdefault(c, set()).add(cur)
for fn in sorted(set(fns)):
    print(fn, '<- called by', ', '.join(sorted(callers.get(fn, ()))) or '(indirect only)')
if not fns:
    print('no function has a label at %x (it may be in the middle of straight-line code)' % a)

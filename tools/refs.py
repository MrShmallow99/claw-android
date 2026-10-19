"""Usage: refs.py <exe> <string>... -- list code referencing each string, with containing function."""
import sys, bisect, pickle, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'recomp'))
from x86image import Image
from discover import Discovery
img = Image(sys.argv[1]); d = Discovery(img); d.run()
E = sorted(d.entries)
data = bytes(img.mem)
for s in sys.argv[2:]:
    pos = 0
    while True:
        i = data.find(s.encode() + b'\0', pos)
        if i < 0: break
        va = img.base + i
        pos = i + 1
        for a, ins in sorted(d.insns.items()):
            if ('0x%x' % va) in ins.op_str:
                k = bisect.bisect_right(E, a) - 1
                print('%-28s str@%x  %x in f_%x: %s %s' % (s, va, a, E[k], ins.mnemonic, ins.op_str))

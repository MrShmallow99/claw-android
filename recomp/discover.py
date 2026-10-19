"""Whole-program code discovery.

Produces the set of function entry points, every decoded instruction, and
resolved jump tables.  Functions may overlap (shared tails are duplicated by
the code generator), so boundaries never need to be exact.
"""
import sys
import json
from collections import Counter
from capstone import x86_const as X
from x86image import Image, BRANCH_JCC


class Discovery:
    def __init__(self, img):
        self.img = img
        self.insns = {}          # addr -> capstone insn
        self.entries = set()     # function entry points
        self.jtables = {}        # jmp insn addr -> list of targets
        self.bad_entries = set()
        self.notes = []
        self.extra_seeds = set()

    # --- helpers -----------------------------------------------------------
    def get(self, a):
        ins = self.insns.get(a)
        if ins is None:
            ins = self.img.decode(a)
            if ins is None:
                return None
            self.insns[a] = ins
        return ins

    def jump_table(self, ins, block):
        """Resolve `jmp dword ptr [reg*4 + table]`. `block` holds preceding insns."""
        op = ins.operands[0]
        m = op.mem
        if m.base != 0 or m.scale != 4 or m.index == 0:
            return None
        table = m.disp & 0xffffffff
        fam = self.reg_family(m.index)
        count = None
        bytetab = None
        hist = block[:-1][-16:]
        i = len(hist) - 1
        while i >= 0:
            p = hist[i]
            i -= 1
            ops = p.operands
            mn = p.mnemonic
            if mn in ('movzx', 'mov') and len(ops) == 2 and ops[0].type == X.X86_OP_REG \
                    and ops[0].reg in fam and ops[1].type == X.X86_OP_MEM and ops[1].size == 1 \
                    and bytetab is None:
                mm = ops[1].mem
                bytetab = mm.disp & 0xffffffff
                fam = self.reg_family(mm.base or mm.index)
                continue
            if mn == 'xor' and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG \
                    and ops[0].reg == ops[1].reg:
                continue
            if mn == 'cmp' and ops[0].type == X.X86_OP_REG and ops[0].reg in fam:
                if ops[1].type == X.X86_OP_IMM:
                    count = ops[1].imm + 1
                elif ops[1].type == X.X86_OP_REG:
                    r2 = self.reg_family(ops[1].reg)
                    for q in reversed(hist[:i + 1]):
                        qo = q.operands
                        if q.mnemonic == 'mov' and qo[0].type == X.X86_OP_REG and qo[0].reg in r2:
                            if qo[1].type == X.X86_OP_IMM:
                                count = qo[1].imm + 1
                            break
                break
            if mn == 'and' and ops[0].type == X.X86_OP_REG and ops[0].reg in fam \
                    and ops[1].type == X.X86_OP_IMM:
                count = ops[1].imm + 1
                break
            try:
                _, written = p.regs_access()
            except Exception:
                written = []
            if any(w in fam for w in written):
                break
        if count is not None and bytetab is not None:
            vals = [self.img.u8(bytetab + k) for k in range(count)]
            count = max(vals) + 1
        if count is None or count > 1024:
            count = 0
            while count < 64 and self.img.in_text(self.img.u32(table + 4 * count)):
                count += 1
            self.notes.append('jtable %x unbounded, guessed %d' % (ins.address, count))
        targets = [self.img.u32(table + 4 * k) for k in range(count)]
        if not all(self.img.in_text(t) for t in targets):
            self.notes.append('jtable %x has non-text targets' % ins.address)
            targets = [t for t in targets if self.img.in_text(t)]
        return targets

    _fam = {}

    def reg_family(self, r):
        fam = [
            (X.X86_REG_EAX, X.X86_REG_AX, X.X86_REG_AL),
            (X.X86_REG_EBX, X.X86_REG_BX, X.X86_REG_BL),
            (X.X86_REG_ECX, X.X86_REG_CX, X.X86_REG_CL),
            (X.X86_REG_EDX, X.X86_REG_DX, X.X86_REG_DL),
            (X.X86_REG_ESI, X.X86_REG_SI),
            (X.X86_REG_EDI, X.X86_REG_DI),
            (X.X86_REG_EBP, X.X86_REG_BP),
        ]
        for f in fam:
            if r in f:
                return f
        return (r,)

    # --- traversal ---------------------------------------------------------
    def explore(self, entry, new_entries):
        """Explore code reachable from entry; record call targets into new_entries."""
        work = [entry]
        seen = set()
        while work:
            a = work.pop()
            block = []
            while a not in seen:
                seen.add(a)
                if not self.img.in_text(a):
                    self.notes.append('flow leaves text at %x' % a)
                    break
                ins = self.get(a)
                if ins is None:
                    self.notes.append('undecodable at %x (from %x)' % (a, entry))
                    break
                block.append(ins)
                mn = ins.mnemonic
                nxt = a + ins.size
                if mn == 'call':
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        new_entries.add(op.imm & 0xffffffff)
                    a = nxt
                    continue
                if mn in ('ret', 'retf', 'iret', 'iretd', 'hlt', 'int3', 'ud2'):
                    break
                if mn == 'jmp':
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        a = op.imm & 0xffffffff
                        block = []
                        continue
                    if op.type == X.X86_OP_MEM:
                        if op.mem.base == 0 and op.mem.index == 0:
                            break  # jmp [import] -- thunk
                        t = self.jtables.get(a)
                        if t is None:
                            t = self.jump_table(ins, block)
                            if t is not None:
                                self.jtables[a] = t
                        if t:
                            work.extend(t)
                    break
                if mn in BRANCH_JCC:
                    work.append(ins.operands[0].imm & 0xffffffff)
                    a = nxt
                    continue
                a = nxt
        return seen

    def code_pointer_candidates(self):
        """Code addresses referenced by immediates in code or by data words."""
        cands = Counter()
        img = self.img
        for ins in list(self.insns.values()):
            if ins.mnemonic in ('call', 'jmp') or ins.mnemonic in BRANCH_JCC:
                continue
            for op in ins.operands:
                if op.type == X.X86_OP_IMM and img.in_text(op.imm & 0xffffffff):
                    cands[op.imm & 0xffffffff] += 1
                if op.type == X.X86_OP_MEM and img.in_text(op.mem.disp & 0xffffffff) \
                        and ins.mnemonic in ('mov', 'push', 'lea'):
                    pass
        for name, va, size in img.sections:
            if name == '.text' or name == '.rsrc':
                continue
            o = va - img.base
            end = min(o + size, len(img.mem)) - 3
            for off in range(o, end, 4):
                v = int.from_bytes(img.mem[off:off + 4], 'little')
                if img.in_text(v):
                    cands[v] += 1
        return cands

    def run(self):
        img = self.img
        pending = {img.entry} | set(self.extra_seeds)
        explored = {}
        rounds = 0
        while True:
            while pending:
                e = pending.pop()
                if e in explored:
                    continue
                self.entries.add(e)
                new = set()
                explored[e] = self.explore(e, new)
                pending |= (new - explored.keys())
            rounds += 1
            if rounds > 12:
                break
            # interior-of-instruction set, to reject bogus pointers
            covered = set()
            starts = set(self.insns)
            for a, ins in self.insns.items():
                for k in range(1, ins.size):
                    covered.add(a + k)
            for c in self.code_pointer_candidates():
                if c in explored or c in covered:
                    continue
                if c in starts:
                    # points at a decoded instruction: a callback / indirect target
                    pending.add(c)
                    continue
                if c in self.bad_entries:
                    continue
                if self.plausible(c):
                    pending.add(c)
                else:
                    self.bad_entries.add(c)
            if not pending:
                break
        self.explored = explored

    def plausible(self, a):
        """Linear decode a few instructions; reject garbage."""
        n = 0
        while n < 32:
            ins = self.img.decode(a)
            if ins is None:
                return False
            if ins.mnemonic in ('ret', 'jmp'):
                return True
            if ins.mnemonic in ('int3', 'hlt', 'in', 'out', 'insb', 'insd', 'outsb', 'outsd',
                                'arpl', 'les', 'lds', 'bound', 'into', 'retf', 'iretd', 'aaa',
                                'aas', 'daa', 'das', 'aam', 'aad', 'sahf', 'salc'):
                return False
            a += ins.size
            n += 1
        return True


def main():
    img = Image(sys.argv[1])
    d = Discovery(img)
    d.run()
    mn = Counter(i.mnemonic for i in d.insns.values())
    print('entries', len(d.entries), 'insns', len(d.insns), 'jtables', len(d.jtables),
          'bad', len(d.bad_entries))
    for k, v in sorted(mn.items(), key=lambda x: -x[1]):
        print('%-12s %d' % (k, v))
    for n in d.notes[:80]:
        print('NOTE', n)
    print('notes', len(d.notes))


if __name__ == '__main__':
    main()

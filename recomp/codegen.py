"""Static recompiler: a 32-bit Windows game exe (x86-32) -> portable C.

    codegen.py game.exe gen/ [--config games/<name>/game.ini] [--comments]

Every discovered function becomes `void f_XXXXXX(CPU *cpu)`.  Guest registers
live in C locals inside a function and are spilled to the CPU struct around
calls.  Flags are C locals too (they are dead across call boundaries for
compiler-generated code), so the C compiler removes unused flag computations.
"""
import os
import sys
from capstone import x86_const as X
from x86image import Image, BRANCH_JCC
from discover import Discovery

R32 = {X.X86_REG_EAX: 'eax', X.X86_REG_ECX: 'ecx', X.X86_REG_EDX: 'edx', X.X86_REG_EBX: 'ebx',
       X.X86_REG_ESP: 'esp', X.X86_REG_EBP: 'ebp', X.X86_REG_ESI: 'esi', X.X86_REG_EDI: 'edi'}
R16 = {X.X86_REG_AX: 'eax', X.X86_REG_CX: 'ecx', X.X86_REG_DX: 'edx', X.X86_REG_BX: 'ebx',
       X.X86_REG_SP: 'esp', X.X86_REG_BP: 'ebp', X.X86_REG_SI: 'esi', X.X86_REG_DI: 'edi'}
R8L = {X.X86_REG_AL: 'eax', X.X86_REG_CL: 'ecx', X.X86_REG_DL: 'edx', X.X86_REG_BL: 'ebx'}
R8H = {X.X86_REG_AH: 'eax', X.X86_REG_CH: 'ecx', X.X86_REG_DH: 'edx', X.X86_REG_BH: 'ebx'}
MMX = {getattr(X, 'X86_REG_MM%d' % i): i for i in range(8)}
REGS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']

MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}
SIGN = {1: '0x80u', 2: '0x8000u', 4: '0x80000000u'}
CTYPE = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t'}
STYPE = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}

COND = {
    'o': 'of', 'no': '!of', 'b': 'cf', 'ae': '!cf', 'e': 'zf', 'ne': '!zf',
    'be': '(cf|zf)', 'a': '(!cf&&!zf)', 's': 'sf', 'ns': '!sf', 'p': 'pf', 'np': '!pf',
    'l': '(sf!=of)', 'ge': '(sf==of)', 'le': '(zf||sf!=of)', 'g': '(!zf&&sf==of)',
}

SPILL = 'SPILL();'
RELOAD = 'RELOAD();'


class Unsupported(Exception):
    pass


class FuncGen:
    def __init__(self, g, entry, body):
        self.g = g
        self.img = g.img
        self.entry = entry
        self.body = body          # sorted list of insn addrs
        self.bodyset = set(body)
        self.labels = set()
        self.cur = None
        self.out = []
        self.tmp = 0
        # x87 and MMX state are C locals in functions that use them (like the GPRs),
        # so the C compiler can keep them in registers across guest memory accesses.
        self.uses_fpu = self.uses_mmx = False
        for a in body:
            ins = g.disc.insns[a]
            if ins.bytes[0] in (0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf) or \
                    (ins.bytes[0] == 0x9b and len(ins.bytes) > 1):
                self.uses_fpu = True
            if ins.mnemonic == 'emms' or any(op.type == X.X86_OP_REG and op.reg in MMX for op in ins.operands):
                self.uses_mmx = True
        sp, rl = ['SPILL();'], ['RELOAD();']
        if self.uses_fpu:
            sp.append('SPILL_FPU();')
            rl.append('RELOAD_FPU();')
        if self.uses_mmx:
            sp.append('SPILL_MMX();')
            rl.append('RELOAD_MMX();')
        self.SP, self.RL = ' '.join(sp), ' '.join(rl)

    # ------------------------------------------------------------------
    def t(self):
        self.tmp += 1
        return 't%d' % self.tmp

    def emit(self, s):
        self.out.append(s)

    def addr_expr(self, op):
        m = op.mem
        parts = []
        if m.segment == X.X86_REG_FS:
            parts.append('cpu->fs_base')
        elif m.segment not in (0, X.X86_REG_DS, X.X86_REG_SS, X.X86_REG_ES, X.X86_REG_CS):
            raise Unsupported('segment')
        if m.base:
            parts.append(R32[m.base])
        if m.index:
            parts.append('%s*%d' % (R32[m.index], m.scale) if m.scale != 1 else R32[m.index])
        dyn = self.g.dyn_fields.get(self.cur.address) if self.cur is not None else None
        if dyn and dyn.get('disp'):
            a, n = dyn['disp']
            parts.append({1: '(uint32_t)(int32_t)(int8_t)RD8(0x%xu)', 2: '(uint32_t)(int32_t)(int16_t)RD16(0x%xu)',
                          4: 'RD32(0x%xu)'}[n] % a)
        elif m.disp or not parts:
            parts.append('0x%xu' % (m.disp & 0xffffffff))
        return '(uint32_t)(' + '+'.join(parts) + ')'

    def rd(self, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            r = op.reg
            if r in R32:
                return R32[r]
            if r in R16:
                return '(%s&0xffffu)' % R16[r]
            if r in R8L:
                return '(%s&0xffu)' % R8L[r]
            if r in R8H:
                return '((%s>>8)&0xffu)' % R8H[r]
            raise Unsupported('reg %d' % r)
        if op.type == X.X86_OP_IMM:
            dyn = self.g.dyn_fields.get(self.cur.address) if self.cur is not None else None
            if dyn and dyn.get('imm'):
                # the game's scripts rewrite this immediate at run time: read it from memory
                a, n = dyn['imm']
                v = {1: '(uint32_t)(int32_t)(int8_t)RD8(0x%xu)', 2: '(uint32_t)(int32_t)(int16_t)RD16(0x%xu)',
                     4: 'RD32(0x%xu)'}[n] % a
                return '(%s & %s)' % (v, MASK[size])
            return '0x%xu' % (op.imm & int(MASK[size][:-1], 16))
        if op.type == X.X86_OP_MEM:
            return 'RD%d(%s)' % (size * 8, self.addr_expr(op))
        raise Unsupported('op')

    def wr(self, op, val, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            r = op.reg
            if r in R32:
                return '%s = %s;' % (R32[r], val)
            if r in R16:
                n = R16[r]
                return '%s = (%s&0xffff0000u)|((%s)&0xffffu);' % (n, n, val)
            if r in R8L:
                n = R8L[r]
                return '%s = (%s&0xffffff00u)|((%s)&0xffu);' % (n, n, val)
            if r in R8H:
                n = R8H[r]
                return '%s = (%s&0xffff00ffu)|(((%s)&0xffu)<<8);' % (n, n, val)
            raise Unsupported('wreg')
        if op.type == X.X86_OP_MEM:
            return 'WR%d(%s, %s);' % (size * 8, self.addr_expr(op), val)
        raise Unsupported('wop')

    def szf(self, r, size):
        return 'zf = (%s)==0; sf = ((%s)&%s)!=0; pf = PARITY(%s);' % (r, r, SIGN[size], r)

    # ------------------------------------------------------------------
    def target_code(self, a):
        """Code to transfer control to static address a (jmp)."""
        if a in self.bodyset:
            self.labels.add(a)
            return 'goto L_%x;' % a
        return self.tailcall(a)

    def tailcall(self, a):
        fn = self.g.fn_for(a)
        return '{ %s %s(cpu); return; }' % (self.SP, fn)

    def call_static(self, ins, target):
        ra = ins.address + ins.size
        fn = self.g.fn_for(target)
        return 'esp -= 4; WR32(esp, 0x%xu); %s %s(cpu); %s CHECK_RA(0x%xu);' % (
            ra, self.SP, fn, self.RL, ra)

    # ------------------------------------------------------------------
    def gen(self):
        for a in self.body:
            ins = self.img_insn(a)
            self.cur = ins
            try:
                code = self.insn(ins)
            except Unsupported as e:
                code = 'UNIMPL(0x%xu, "%s %s");' % (a, ins.mnemonic, ins.op_str)
                self.g.unsupported[ins.mnemonic + ' ' + str(e)] = \
                    self.g.unsupported.get(ins.mnemonic + ' ' + str(e), 0) + 1
            if a in self.g.probes:
                code = 'PROBE(0x%xu); ' % a + code
            if a in self.g.alts:
                code = ''.join(self.alt_code(a, b) for b in self.g.alts[a]) + code
            self.out.append((a, ins, code))
            nxt = a + ins.size
            if not self.ends_flow(ins) and nxt not in self.bodyset:
                self.out.append((None, None, self.target_code(nxt)))
        lines = []
        name = self.g.fn_name(self.entry)
        lines.append('void %s(CPU *cpu) {' % name)
        lines.append('  PROLOGUE();')
        if self.uses_fpu:
            lines.append('  FPU_LOCALS();')
        if self.uses_mmx:
            lines.append('  MMX_LOCALS();')
        if self.body[0] != self.entry:
            self.labels.add(self.entry)
            lines.append('  goto L_%x;' % self.entry)
        for a, ins, code in self.out:
            if a is not None:
                if a in self.labels:
                    lines.append('L_%x:' % a)
                if self.g.comments:
                    lines.append('  /* %x: %s %s */' % (a, ins.mnemonic, ins.op_str.replace('*/', '* /')))
            lines.append('  ' + code)
        lines.append('  UNREACHABLE(0x%xu);' % self.entry)
        lines.append('}')
        return '\n'.join(lines)

    def alt_code(self, a, b):
        """`if guest memory at a holds patch bytes b: run the translated patch` (then
        continue after it, at a + len(b))."""
        parts = []
        end = a + len(b)
        last = None
        for i in self.img.md.disasm(b, a):
            self.cur = i
            try:
                parts.append(self.insn(i))
            except Unsupported:
                return ''
            last = i
            if i.address + i.size >= end:
                break
        self.cur = None
        if last is None or last.address + last.size != end:
            return ''
        if not self.ends_flow(last):
            parts.append(self.target_code(end))
        cond = 'RD8(0x%xu) == 0x%02xu' % (a, b[0]) if len(b) == 1 else \
            'memcmp(GPTR(0x%xu), "%s", %d) == 0' % (a, ''.join('\\x%02x' % x for x in b), len(b))
        return 'if (%s) { %s } ' % (cond, ' '.join(parts))

    def img_insn(self, a):
        return self.g.disc.insns[a]

    def ends_flow(self, ins):
        mn = ins.mnemonic
        return mn in ('ret', 'jmp', 'hlt', 'int3', 'ud2', 'retf', 'iretd')

    # ------------------------------------------------------------------
    def insn(self, ins):
        mn = ins.mnemonic
        ops = ins.operands
        prefix = None
        if mn.startswith('lock '):
            # guest threads are host threads, but the game only uses lock-prefixed
            # inc/dec/xadd on counters; the plain form is equivalent for one writer.
            mn = mn[5:]
        if mn.startswith('rep ') or mn.startswith('repe ') or mn.startswith('repne '):
            prefix, mn = mn.split(' ', 1)
        h = getattr(self, 'i_' + mn, None)
        if mn in BRANCH_JCC:
            return self.i_jcc(ins, mn)
        if mn.startswith('set'):
            return self.i_setcc(ins, mn[3:])
        if mn.startswith('cmov'):
            c = COND[mn[4:]]
            return 'if (%s) { %s }' % (c, self.wr(ops[0], self.rd(ops[1])))
        if ins.bytes[0] in (0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf) or \
                (ins.bytes[0] == 0x9b and len(ins.bytes) > 1):
            return self.fpu(ins)
        if h is None:
            raise Unsupported('no handler')
        if prefix:
            return h(ins, prefix)
        return h(ins)

    # --- data movement ---------------------------------------------------
    def i_mov(self, ins):
        o = ins.operands
        if o[0].type == X.X86_OP_REG and o[0].reg in MMX or o[1].type == X.X86_OP_REG and o[1].reg in MMX:
            raise Unsupported('mov mmx')
        if any(op.type == X.X86_OP_REG and op.reg in (X.X86_REG_DS, X.X86_REG_ES, X.X86_REG_FS,
                                                     X.X86_REG_GS, X.X86_REG_SS, X.X86_REG_CS)
               for op in o):
            if o[0].type == X.X86_OP_REG and o[0].reg in (X.X86_REG_DS, X.X86_REG_ES, X.X86_REG_FS, X.X86_REG_GS, X.X86_REG_SS):
                return '/* mov to segment ignored */'
            return self.wr(o[0], '0x23u', 2 if o[0].type == X.X86_OP_MEM else None)
        return self.wr(o[0], self.rd(o[1], o[0].size), o[0].size)

    def i_movzx(self, ins):
        o = ins.operands
        return self.wr(o[0], self.rd(o[1]))

    def i_movsx(self, ins):
        o = ins.operands
        return self.wr(o[0], '(uint32_t)(int32_t)(%s)(%s)' % (STYPE[o[1].size], self.rd(o[1])))

    def i_lea(self, ins):
        o = ins.operands
        m = o[1].mem
        parts = []
        if m.base:
            parts.append(R32[m.base])
        if m.index:
            parts.append('%s*%d' % (R32[m.index], m.scale))
        if m.disp or not parts:
            parts.append('0x%xu' % (m.disp & 0xffffffff))
        return self.wr(o[0], '(uint32_t)(' + '+'.join(parts) + ')')

    def i_xchg(self, ins):
        o = ins.operands
        t = self.t()
        return '{ uint32_t %s = %s; %s %s }' % (t, self.rd(o[0]), self.wr(o[0], self.rd(o[1])),
                                               self.wr(o[1], t))

    def i_push(self, ins):
        o = ins.operands[0]
        if o.type == X.X86_OP_REG and o.reg in (X.X86_REG_DS, X.X86_REG_ES, X.X86_REG_FS, X.X86_REG_GS,
                                                 X.X86_REG_SS, X.X86_REG_CS):
            return 'esp -= 4; WR32(esp, 0x23u);'
        size = o.size if o.type != X.X86_OP_IMM else 4
        if size == 2:
            return '{ uint32_t v = %s; esp -= 2; WR16(esp, v); }' % self.rd(o, 2)
        if o.type == X.X86_OP_IMM:
            return 'esp -= 4; WR32(esp, 0x%xu);' % (o.imm & 0xffffffff)
        return '{ uint32_t v = %s; esp -= 4; WR32(esp, v); }' % self.rd(o, 4)

    def i_pop(self, ins):
        o = ins.operands[0]
        if o.type == X.X86_OP_REG and o.reg in (X.X86_REG_DS, X.X86_REG_ES, X.X86_REG_FS, X.X86_REG_GS,
                                                 X.X86_REG_SS):
            return 'esp += 4;'
        if o.type == X.X86_OP_REG and o.reg == X.X86_REG_ESP:
            return 'esp = RD32(esp);'
        if o.type == X.X86_OP_MEM:
            return '{ uint32_t v = RD32(esp); esp += 4; %s }' % self.wr(o, 'v')
        return '{ uint32_t v = RD32(esp); esp += 4; %s }' % self.wr(o, 'v')

    def i_pushal(self, ins):
        return ('{ uint32_t s = esp; esp -= 32; WR32(esp+28, eax); WR32(esp+24, ecx); WR32(esp+20, edx); '
                'WR32(esp+16, ebx); WR32(esp+12, s); WR32(esp+8, ebp); WR32(esp+4, esi); WR32(esp, edi); }')

    def i_popal(self, ins):
        return ('{ edi = RD32(esp); esi = RD32(esp+4); ebp = RD32(esp+8); ebx = RD32(esp+16); '
                'edx = RD32(esp+20); ecx = RD32(esp+24); eax = RD32(esp+28); esp += 32; }')

    def i_pushfd(self, ins):
        return 'esp -= 4; WR32(esp, MAKE_EFLAGS());'

    def i_popfd(self, ins):
        return '{ uint32_t v = RD32(esp); esp += 4; LOAD_EFLAGS(v); }'

    def i_leave(self, ins):
        return 'esp = ebp; ebp = RD32(esp); esp += 4;'

    def i_cdq(self, ins):
        return 'edx = (uint32_t)((int32_t)eax >> 31);'

    def i_cwde(self, ins):
        return 'eax = (uint32_t)(int32_t)(int16_t)eax;'

    def i_cbw(self, ins):
        return 'eax = (eax & 0xffff0000u) | ((uint32_t)(int16_t)(int8_t)eax & 0xffffu);'

    def i_cwd(self, ins):
        return 'edx = (edx & 0xffff0000u) | (((int16_t)eax < 0) ? 0xffffu : 0);'

    def i_nop(self, ins):
        return ';'

    i_wait = i_nop
    i_fwait = i_nop
    i_emms = i_nop

    def i_cld(self, ins):
        return 'cpu->df = 0;'

    def i_std(self, ins):
        return 'cpu->df = 1;'

    def i_clc(self, ins):
        return 'cf = 0;'

    def i_stc(self, ins):
        return 'cf = 1;'

    def i_cmc(self, ins):
        return 'cf = !cf;'

    def i_cli(self, ins):
        return ';'

    i_sti = i_cli

    def i_sahf(self, ins):
        return 'cf = (eax>>8)&1; pf = (eax>>10)&1; zf = (eax>>14)&1; sf = (eax>>15)&1;'

    def i_lahf(self, ins):
        return 'eax = (eax & 0xffff00ffu) | ((uint32_t)((sf<<7)|(zf<<6)|(pf<<2)|2|cf) << 8);'

    def i_xlatb(self, ins):
        return 'eax = (eax & 0xffffff00u) | RD8(ebx + (eax & 0xffu));'

    def i_sldt(self, ins):
        return self.wr(ins.operands[0], '0')

    def i_int(self, ins):
        return 'GUEST_INT(0x%xu, %d);' % (ins.address, ins.operands[0].imm)

    def i_int3(self, ins):
        return 'GUEST_INT(0x%xu, 3);' % ins.address

    def i_hlt(self, ins):
        return 'GUEST_INT(0x%xu, -1);' % ins.address

    def i_cpuid(self, ins):
        return 'SPILL(); host_cpuid(cpu); RELOAD();'

    def i_rdtsc(self, ins):
        return '{ uint64_t v = host_rdtsc(); eax = (uint32_t)v; edx = (uint32_t)(v>>32); }'

    # --- ALU ---------------------------------------------------------------
    def alu(self, ins, kind):
        o = ins.operands
        size = o[0].size
        a = self.rd(o[0])
        b = self.rd(o[1], size)
        M, S = MASK[size], SIGN[size]
        c = ['{ uint32_t a = %s, b = %s, r;' % (a, b)]
        if kind == 'add':
            c.append('r = (a + b) & %s;' % M)
            c.append('cf = r < a;' if size == 4 else 'cf = ((a + b) >> %d) & 1;' % (size * 8))
            c.append('of = ((a ^ r) & (b ^ r) & %s) != 0;' % S)
        elif kind in ('sub', 'cmp'):
            c.append('r = (a - b) & %s;' % M)
            c.append('cf = a < b;')
            c.append('of = ((a ^ b) & (a ^ r) & %s) != 0;' % S)
        elif kind == 'adc':
            c.append('uint64_t w = (uint64_t)a + b + cf; r = (uint32_t)w & %s;' % M)
            c.append('cf = (w >> %d) & 1;' % (size * 8))
            c.append('of = ((a ^ r) & (b ^ r) & %s) != 0;' % S)
        elif kind == 'sbb':
            c.append('uint64_t w = (uint64_t)b + cf; r = (uint32_t)(a - w) & %s;' % M)
            c.append('cf = (uint64_t)a < w;')
            c.append('of = ((a ^ b) & (a ^ r) & %s) != 0;' % S)
        elif kind in ('and', 'test'):
            c.append('r = a & b; cf = 0; of = 0;')
        elif kind == 'or':
            c.append('r = a | b; cf = 0; of = 0;')
        elif kind == 'xor':
            c.append('r = a ^ b; cf = 0; of = 0;')
        c.append(self.szf('r', size))
        if kind not in ('cmp', 'test'):
            c.append(self.wr(o[0], 'r'))
        c.append('}')
        # xor r,r idiom
        if kind in ('xor', 'sub') and o[0].type == X.X86_OP_REG and o[1].type == X.X86_OP_REG \
                and o[0].reg == o[1].reg:
            return '%s cf = 0; of = 0; zf = 1; sf = 0; pf = 1;' % self.wr(o[0], '0')
        return ' '.join(c)

    def i_add(self, ins): return self.alu(ins, 'add')
    def i_sub(self, ins): return self.alu(ins, 'sub')
    def i_cmp(self, ins): return self.alu(ins, 'cmp')
    def i_adc(self, ins): return self.alu(ins, 'adc')
    def i_sbb(self, ins): return self.alu(ins, 'sbb')
    def i_and(self, ins): return self.alu(ins, 'and')
    def i_or(self, ins):
        if ins.operands[0].type == X.X86_OP_REG and ins.operands[0].reg in MMX:
            return self.mmx(ins)
        return self.alu(ins, 'or')
    def i_xor(self, ins): return self.alu(ins, 'xor')
    def i_test(self, ins): return self.alu(ins, 'test')

    def incdec(self, ins, d):
        o = ins.operands[0]
        size = o.size
        M, S = MASK[size], SIGN[size]
        if d > 0:
            ofc = 'of = r == %s;' % S
        else:
            ofc = 'of = a == %s;' % S
        return '{ uint32_t a = %s, r = (a %s 1) & %s; %s %s %s }' % (
            self.rd(o), '+' if d > 0 else '-', M, ofc, self.szf('r', size), self.wr(o, 'r'))

    def i_inc(self, ins): return self.incdec(ins, 1)
    def i_dec(self, ins): return self.incdec(ins, -1)

    def i_neg(self, ins):
        o = ins.operands[0]
        size = o.size
        return '{ uint32_t a = %s, r = (0u - a) & %s; cf = a != 0; of = a == %s; %s %s }' % (
            self.rd(o), MASK[size], SIGN[size], self.szf('r', size), self.wr(o, 'r'))

    def i_not(self, ins):
        o = ins.operands[0]
        return self.wr(o, '(~%s) & %s' % (self.rd(o), MASK[o.size]))

    def shift(self, ins, kind):
        o = ins.operands
        size = o[0].size
        bits = size * 8
        M, S = MASK[size], SIGN[size]
        if len(o) == 1:
            cnt = '1'
        else:
            cnt = '(%s & 31)' % self.rd(o[1], 1)
        c = ['{ uint32_t a = %s, c = %s, r;' % (self.rd(o[0]), cnt), 'if (c) {']
        if kind == 'shl':
            c.append('r = (uint32_t)(((uint64_t)a << c) & %s); cf = (uint32_t)(((uint64_t)a << c) >> %d) & 1; of = ((r & %s) != 0) ^ cf;' % (M, bits, S))
        elif kind == 'shr':
            c.append('r = (uint32_t)((uint64_t)a >> c); cf = (uint32_t)(((uint64_t)a >> (c - 1)) & 1); of = (a & %s) != 0;' % S)
        elif kind == 'sar':
            c.append('int64_t sa = (int64_t)(%s)a; r = (uint32_t)(sa >> c) & %s; cf = (uint32_t)((sa >> (c - 1)) & 1); of = 0;' % (STYPE[size], M))
        elif kind == 'rol':
            c.append('uint32_t k = c %% %d; r = k ? ((a << k) | (a >> (%d - k))) & %s : a; cf = r & 1; of = ((r & %s) != 0) ^ cf;' % (bits, bits, M, S))
        elif kind == 'ror':
            c.append('uint32_t k = c %% %d; r = k ? ((a >> k) | (a << (%d - k))) & %s : a; cf = (r & %s) != 0; of = ((r ^ (r << 1)) & %s) != 0;' % (bits, bits, M, S, S))
        elif kind == 'rcr':
            c.append('uint32_t k = c %% %d; r = a; while (k--) { uint32_t nc = r & 1; r = (r >> 1) | ((uint32_t)cf << %d); cf = nc; } of = ((r ^ (r << 1)) & %s) != 0;' % (bits + 1, bits - 1, S))
        elif kind == 'rcl':
            c.append('uint32_t k = c %% %d; r = a; while (k--) { uint32_t nc = (r & %s) != 0; r = ((r << 1) | cf) & %s; cf = nc; } of = ((r & %s) != 0) ^ cf;' % (bits + 1, S, M, S))
        if kind in ('shl', 'shr', 'sar'):
            c.append(self.szf('r', size))
        c.append(self.wr(o[0], 'r'))
        c.append('} }')
        return ' '.join(c)

    def i_shl(self, ins): return self.shift(ins, 'shl')
    i_sal = i_shl
    def i_shr(self, ins): return self.shift(ins, 'shr')
    def i_sar(self, ins): return self.shift(ins, 'sar')
    def i_rol(self, ins): return self.shift(ins, 'rol')
    def i_ror(self, ins): return self.shift(ins, 'ror')
    def i_rcr(self, ins): return self.shift(ins, 'rcr')
    def i_rcl(self, ins): return self.shift(ins, 'rcl')

    def i_shld(self, ins):
        o = ins.operands
        cnt = '(%s & 31)' % self.rd(o[2], 1) if len(o) == 3 else '1'
        return ('{ uint32_t a = %s, b = %s, c = %s, r; if (c) { r = (a << c) | (b >> (32 - c)); '
                'cf = (a >> (32 - c)) & 1; of = ((r ^ a) & 0x80000000u) != 0; %s %s } }') % (
            self.rd(o[0]), self.rd(o[1]), cnt, self.szf('r', 4), self.wr(o[0], 'r'))

    def i_shrd(self, ins):
        o = ins.operands
        cnt = '(%s & 31)' % self.rd(o[2], 1) if len(o) == 3 else '1'
        return ('{ uint32_t a = %s, b = %s, c = %s, r; if (c) { r = (a >> c) | (b << (32 - c)); '
                'cf = (a >> (c - 1)) & 1; of = ((r ^ a) & 0x80000000u) != 0; %s %s } }') % (
            self.rd(o[0]), self.rd(o[1]), cnt, self.szf('r', 4), self.wr(o[0], 'r'))

    def i_imul(self, ins):
        o = ins.operands
        if len(o) == 1:
            size = o[0].size
            if size == 4:
                return ('{ int64_t p = (int64_t)(int32_t)eax * (int32_t)%s; eax = (uint32_t)p; '
                        'edx = (uint32_t)(p >> 32); cf = of = p != (int32_t)p; }') % self.rd(o[0])
            if size == 2:
                return ('{ int32_t p = (int16_t)eax * (int16_t)%s; eax = (eax & 0xffff0000u) | (p & 0xffff); '
                        'edx = (edx & 0xffff0000u) | ((p >> 16) & 0xffff); cf = of = p != (int16_t)p; }') % self.rd(o[0])
            return ('{ int32_t p = (int8_t)eax * (int8_t)%s; eax = (eax & 0xffff0000u) | (p & 0xffff); '
                    'cf = of = p != (int8_t)p; }') % self.rd(o[0])
        if len(o) == 2:
            a, b = self.rd(o[0]), self.rd(o[1])
        else:
            a, b = self.rd(o[1]), self.rd(o[2], o[0].size)
        size = o[0].size
        if size == 4:
            return '{ int64_t p = (int64_t)(int32_t)%s * (int32_t)%s; cf = of = p != (int32_t)p; %s }' % (
                a, b, self.wr(o[0], '(uint32_t)p'))
        return '{ int32_t p = (int32_t)(int16_t)%s * (int16_t)%s; cf = of = p != (int16_t)p; %s }' % (
            a, b, self.wr(o[0], '(uint32_t)p'))

    def i_mul(self, ins):
        o = ins.operands[0]
        if o.size == 4:
            return ('{ uint64_t p = (uint64_t)eax * %s; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); '
                    'cf = of = edx != 0; }') % self.rd(o)
        if o.size == 2:
            return ('{ uint32_t p = (eax & 0xffff) * %s; eax = (eax & 0xffff0000u) | (p & 0xffff); '
                    'edx = (edx & 0xffff0000u) | (p >> 16); cf = of = (p >> 16) != 0; }') % self.rd(o)
        return ('{ uint32_t p = (eax & 0xff) * %s; eax = (eax & 0xffff0000u) | (p & 0xffff); '
                'cf = of = (p >> 8) != 0; }') % self.rd(o)

    def i_div(self, ins):
        o = ins.operands[0]
        if o.size == 4:
            return ('{ uint32_t d = %s; if (!d) DIV0(0x%xu); uint64_t n = ((uint64_t)edx << 32) | eax; '
                    'uint64_t q = n / d; if (q >> 32) DIV0(0x%xu); edx = (uint32_t)(n %% d); eax = (uint32_t)q; }') % (
                self.rd(o), ins.address, ins.address)
        if o.size == 2:
            return ('{ uint32_t d = %s; if (!d) DIV0(0x%xu); uint32_t n = ((edx & 0xffff) << 16) | (eax & 0xffff); '
                    'uint32_t q = n / d; edx = (edx & 0xffff0000u) | (n %% d); eax = (eax & 0xffff0000u) | (q & 0xffff); }') % (
                self.rd(o), ins.address)
        return ('{ uint32_t d = %s; if (!d) DIV0(0x%xu); uint32_t n = eax & 0xffff; '
                'uint32_t q = n / d, r = n %% d; eax = (eax & 0xffff0000u) | (r << 8) | (q & 0xff); }') % (
            self.rd(o), ins.address)

    def i_idiv(self, ins):
        o = ins.operands[0]
        if o.size == 4:
            return ('{ int32_t d = (int32_t)%s; if (!d) DIV0(0x%xu); int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); '
                    'int64_t q = n / d; edx = (uint32_t)(int32_t)(n %% d); eax = (uint32_t)q; }') % (
                self.rd(o), ins.address)
        if o.size == 2:
            return ('{ int32_t d = (int16_t)%s; if (!d) DIV0(0x%xu); int32_t n = (int32_t)(((edx & 0xffff) << 16) | (eax & 0xffff)); '
                    'int32_t q = n / d, r = n %% d; edx = (edx & 0xffff0000u) | (r & 0xffff); eax = (eax & 0xffff0000u) | (q & 0xffff); }') % (
                self.rd(o), ins.address)
        return ('{ int32_t d = (int8_t)%s; if (!d) DIV0(0x%xu); int32_t n = (int16_t)eax; '
                'int32_t q = n / d, r = n %% d; eax = (eax & 0xffff0000u) | ((r & 0xff) << 8) | (q & 0xff); }') % (
            self.rd(o), ins.address)

    def i_bswap(self, ins):
        o = ins.operands[0]
        return self.wr(o, '__builtin_bswap32(%s)' % self.rd(o))

    def bitop(self, ins, kind):
        o = ins.operands
        upd = {'bt': None, 'bts': 'v | m', 'btr': 'v & ~m', 'btc': 'v ^ m'}[kind]
        if o[0].type == X.X86_OP_MEM and o[1].type == X.X86_OP_REG:
            # bit string: the register offset may address beyond the dword
            code = '{ int32_t b = (int32_t)%s; uint32_t ad = %s + (uint32_t)((b >> 5) * 4); ' \
                   'uint32_t v = RD32(ad), m = 1u << (b & 31); cf = (v & m) != 0;' % (
                       self.rd(o[1], 4), self.addr_expr(o[0]))
            if upd:
                code += ' WR32(ad, %s);' % upd
            return code + ' }'
        bits = o[0].size * 8
        code = '{ uint32_t v = %s, m = 1u << (%s & %d); cf = (v & m) != 0;' % (
            self.rd(o[0]), self.rd(o[1], 1), bits - 1)
        if upd:
            code += ' ' + self.wr(o[0], upd)
        return code + ' }'

    def i_bt(self, ins): return self.bitop(ins, 'bt')
    def i_bts(self, ins): return self.bitop(ins, 'bts')
    def i_btr(self, ins): return self.bitop(ins, 'btr')
    def i_btc(self, ins): return self.bitop(ins, 'btc')

    def i_bsr(self, ins):
        o = ins.operands
        return '{ uint32_t s = %s; zf = s == 0; if (s) { %s } }' % (self.rd(o[1]), self.wr(o[0], '31 - __builtin_clz(s)'))

    def i_bsf(self, ins):
        o = ins.operands
        return '{ uint32_t s = %s; zf = s == 0; if (s) { %s } }' % (self.rd(o[1]), self.wr(o[0], '__builtin_ctz(s)'))

    # --- control flow ----------------------------------------------------------
    def i_jcc(self, ins, mn):
        tgt = ins.operands[0].imm & 0xffffffff
        if mn == 'jecxz':
            cond = 'ecx == 0'
        elif mn == 'loop':
            return 'ecx--; if (ecx) %s' % self.target_code(tgt)
        elif mn == 'loope':
            return 'ecx--; if (ecx && zf) %s' % self.target_code(tgt)
        elif mn == 'loopne':
            return 'ecx--; if (ecx && !zf) %s' % self.target_code(tgt)
        else:
            cond = COND[mn[1:]]
        return 'if (%s) %s' % (cond, self.target_code(tgt))

    def i_setcc(self, ins, cc):
        return self.wr(ins.operands[0], '(%s) ? 1u : 0u' % COND[cc])

    def i_jmp(self, ins):
        o = ins.operands[0]
        if o.type == X.X86_OP_IMM:
            return self.target_code(o.imm & 0xffffffff)
        if o.type == X.X86_OP_MEM and o.mem.base == 0 and o.mem.index == 0 and \
                (o.mem.disp & 0xffffffff) in self.img.imports:
            dll, nm = self.img.imports[o.mem.disp & 0xffffffff]
            return '{ %s %s(cpu); return; }' % (self.SP, self.g.import_fn(dll, nm))
        tbl = self.g.disc.jtables.get(ins.address)
        if tbl:
            cases = []
            for tg in sorted(set(tbl)):
                cases.append('case 0x%xu: %s' % (tg, self.target_code(tg)))
            return '{ uint32_t tgt = %s; switch (tgt) { %s default: BAD_JUMP(0x%xu, tgt); } }' % (
                self.rd(o, 4), ' '.join(cases), ins.address)
        return '{ uint32_t tgt = %s; %s call_indirect(cpu, tgt); return; }' % (self.rd(o, 4), self.SP)

    def i_call(self, ins):
        o = ins.operands[0]
        ra = ins.address + ins.size
        if o.type == X.X86_OP_IMM:
            return self.call_static(ins, o.imm & 0xffffffff)
        if o.type == X.X86_OP_MEM and o.mem.base == 0 and o.mem.index == 0 and \
                (o.mem.disp & 0xffffffff) in self.img.imports:
            dll, nm = self.img.imports[o.mem.disp & 0xffffffff]
            return 'esp -= 4; WR32(esp, 0x%xu); %s %s(cpu); %s' % (
                ra, self.SP, self.g.import_fn(dll, nm), self.RL)
        return '{ uint32_t tgt = %s; esp -= 4; WR32(esp, 0x%xu); %s call_indirect(cpu, tgt); %s CHECK_RA(0x%xu); }' % (
            self.rd(o, 4), ra, self.SP, self.RL, ra)

    def i_ret(self, ins):
        o = ins.operands
        n = o[0].imm if o else 0
        return 'cpu->ret_addr = RD32(esp); esp += %d; %s return;' % (4 + n, self.SP)

    # --- strings -----------------------------------------------------------------
    def string_op(self, ins, prefix, kind, size):
        step = 'cpu->df ? -%d : %d' % (size, size)
        T = CTYPE[size]
        if kind == 'movs':
            body = 'WR%d(edi, RD%d(esi)); esi += d; edi += d;' % (size * 8, size * 8)
        elif kind == 'stos':
            body = 'WR%d(edi, (%s)eax); edi += d;' % (size * 8, T)
        elif kind == 'lods':
            body = '%s esi += d;' % self.wr_reg_sized('eax', size, 'RD%d(esi)' % (size * 8))
        elif kind == 'scas':
            body = ('{ uint32_t a = eax & %s, b = RD%d(edi), r = (a - b) & %s; cf = a < b; '
                    'of = ((a ^ b) & (a ^ r) & %s) != 0; %s } edi += d;') % (
                MASK[size], size * 8, MASK[size], SIGN[size], self.szf('r', size))
        elif kind == 'cmps':
            body = ('{ uint32_t a = RD%d(esi), b = RD%d(edi), r = (a - b) & %s; cf = a < b; '
                    'of = ((a ^ b) & (a ^ r) & %s) != 0; %s } esi += d; edi += d;') % (
                size * 8, size * 8, MASK[size], SIGN[size], self.szf('r', size))
        if prefix is None:
            return '{ int32_t d = %s; %s }' % (step, body)
        if kind in ('movs', 'stos') and prefix == 'rep':
            fast = {'movs': 'REP_MOVS', 'stos': 'REP_STOS'}[kind]
            return '%s%d();' % (fast, size)
        if kind in ('scas', 'cmps'):
            stop = '!zf' if prefix in ('repe', 'rep') else 'zf'
            return '{ int32_t d = %s; while (ecx) { %s ecx--; if (%s) break; } }' % (step, body, stop)
        return '{ int32_t d = %s; while (ecx) { %s ecx--; } }' % (step, body)

    def wr_reg_sized(self, r, size, v):
        if size == 4:
            return '%s = %s;' % (r, v)
        return '%s = (%s & ~%s) | (%s);' % (r, r, MASK[size], v)

    def _str(kind, size):
        def f(self, ins, prefix=None):
            return self.string_op(ins, prefix, kind, size)
        return f

    i_movsb = _str('movs', 1)
    i_movsw = _str('movs', 2)
    i_movsd = _str('movs', 4)
    i_stosb = _str('stos', 1)
    i_stosw = _str('stos', 2)
    i_stosd = _str('stos', 4)
    i_lodsb = _str('lods', 1)
    i_lodsw = _str('lods', 2)
    i_lodsd = _str('lods', 4)
    i_scasb = _str('scas', 1)
    i_scasw = _str('scas', 2)
    i_scasd = _str('scas', 4)
    i_cmpsb = _str('cmps', 1)
    i_cmpsw = _str('cmps', 2)
    i_cmpsd = _str('cmps', 4)

    # --- MMX ---------------------------------------------------------------------
    def mmrd(self, op):
        if op.type == X.X86_OP_REG and op.reg in MMX:
            return 'M_mm[%d]' % MMX[op.reg]
        if op.type == X.X86_OP_MEM:
            return 'RD64(%s)' % self.addr_expr(op)
        raise Unsupported('mmx operand')

    def mmwr(self, op, v):
        if op.type == X.X86_OP_REG and op.reg in MMX:
            return 'M_mm[%d] = %s;' % (MMX[op.reg], v)
        if op.type == X.X86_OP_MEM:
            return 'WR64(%s, %s);' % (self.addr_expr(op), v)
        raise Unsupported('mmx dest')

    def mmx(self, ins):
        mn = ins.mnemonic
        o = ins.operands
        if mn == 'movd':
            if o[0].type == X.X86_OP_REG and o[0].reg in MMX:
                return 'M_mm[%d] = (uint64_t)%s;' % (MMX[o[0].reg], self.rd(o[1], 4))
            return self.wr(o[0], '(uint32_t)M_mm[%d]' % MMX[o[1].reg], 4)
        if mn == 'movq':
            return self.mmwr(o[0], self.mmrd(o[1]))
        d = 'M_mm[%d]' % MMX[o[0].reg]
        if o[1].type == X.X86_OP_IMM:
            s = '(uint64_t)%du' % o[1].imm
        else:
            s = self.mmrd(o[1])
        fn = {
            'por': 'mmx_or', 'pand': 'mmx_and', 'pandn': 'mmx_andn', 'pxor': 'mmx_xor',
            'psllq': 'mmx_psllq', 'psrlq': 'mmx_psrlq', 'psrlw': 'mmx_psrlw', 'psllw': 'mmx_psllw',
            'psraw': 'mmx_psraw', 'psrld': 'mmx_psrld', 'pslld': 'mmx_pslld', 'psrad': 'mmx_psrad',
            'paddw': 'mmx_paddw', 'paddd': 'mmx_paddd', 'paddb': 'mmx_paddb', 'psubw': 'mmx_psubw',
            'psubd': 'mmx_psubd', 'psubb': 'mmx_psubb', 'pmullw': 'mmx_pmullw', 'pmulhw': 'mmx_pmulhw',
            'punpckhbw': 'mmx_punpckhbw', 'punpcklbw': 'mmx_punpcklbw', 'punpckhwd': 'mmx_punpckhwd',
            'punpcklwd': 'mmx_punpcklwd', 'punpckhdq': 'mmx_punpckhdq', 'punpckldq': 'mmx_punpckldq',
            'packuswb': 'mmx_packuswb', 'packsswb': 'mmx_packsswb', 'packssdw': 'mmx_packssdw',
            'paddusb': 'mmx_paddusb', 'paddusw': 'mmx_paddusw', 'psubusb': 'mmx_psubusb',
            'psubusw': 'mmx_psubusw', 'paddsw': 'mmx_paddsw', 'psubsw': 'mmx_psubsw', 'pmaddwd': 'mmx_pmaddwd',
        }.get(mn)
        if fn is None:
            raise Unsupported('mmx op')
        return '%s = %s(%s, %s);' % (d, fn, d, s)

    # --- x87 -----------------------------------------------------------------------
    def fpu(self, ins):
        b = list(ins.bytes)
        i = 0
        while b[i] in (0x66, 0x67, 0xf2, 0xf3, 0x2e, 0x3e, 0x26, 0x36, 0x64, 0x65, 0x9b):
            i += 1
        op = b[i]
        modrm = b[i + 1]
        mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
        if mod != 3:
            memop = [x for x in ins.operands if x.type == X.X86_OP_MEM]
            if not memop:
                raise Unsupported('fpu no mem')
            A = self.addr_expr(memop[0])
            return self.fpu_mem(op, reg, A)
        return self.fpu_reg(op, reg, rm)

    ARITH = ['add', 'mul', 'com', 'comp', 'sub', 'subr', 'div', 'divr']

    def farith(self, opn, dst, src):
        if opn == 'add':
            return '%s = %s + %s;' % (dst, dst, src)
        if opn == 'mul':
            return '%s = %s * %s;' % (dst, dst, src)
        if opn == 'sub':
            return '%s = %s - %s;' % (dst, dst, src)
        if opn == 'subr':
            return '%s = %s - %s;' % (dst, src, dst)
        if opn == 'div':
            return '%s = %s / %s;' % (dst, dst, src)
        if opn == 'divr':
            return '%s = %s / %s;' % (dst, src, dst)
        if opn == 'com':
            return 'FCOM(%s, %s);' % (dst, src)
        if opn == 'comp':
            return 'FCOM(%s, %s); FPOP();' % (dst, src)
        raise Unsupported('farith')

    def fpu_mem(self, op, reg, A):
        if op in (0xd8, 0xdc, 0xda, 0xde):
            src = {0xd8: 'RDF32(%s)', 0xdc: 'RDF64(%s)', 0xda: '(double)(int32_t)RD32(%s)',
                   0xde: '(double)(int16_t)RD16(%s)'}[op] % A
            return '{ double s = %s; %s }' % (src, self.farith(self.ARITH[reg], 'ST(0)', 's'))
        if op == 0xd9:
            if reg == 0:
                return 'FPUSH(RDF32(%s));' % A
            if reg == 2:
                return 'WRF32(%s, ST(0));' % A
            if reg == 3:
                return 'WRF32(%s, ST(0)); FPOP();' % A
            if reg == 4:
                return 'SPILL_FPU(); fpu_ldenv(cpu, %s); RELOAD_FPU();' % A
            if reg == 5:
                return 'cpu->fcw = RD16(%s);' % A
            if reg == 6:
                return 'SPILL_FPU(); fpu_stenv(cpu, %s);' % A
            if reg == 7:
                return 'WR16(%s, cpu->fcw);' % A
        if op == 0xdb:
            if reg == 0:
                return 'FPUSH((double)(int32_t)RD32(%s));' % A
            if reg == 2:
                return 'WR32(%s, (uint32_t)fpu_toint32(cpu, ST(0)));' % A
            if reg == 3:
                return 'WR32(%s, (uint32_t)fpu_toint32(cpu, ST(0))); FPOP();' % A
            if reg == 5:
                return 'FPUSH(fpu_ld80(%s));' % A
            if reg == 7:
                return 'fpu_st80(%s, ST(0)); FPOP();' % A
        if op == 0xdd:
            if reg == 0:
                return 'FPUSH(RDF64(%s));' % A
            if reg == 2:
                return 'WRF64(%s, ST(0));' % A
            if reg == 3:
                return 'WRF64(%s, ST(0)); FPOP();' % A
            if reg == 7:
                return 'WR16(%s, FSTSW());' % A
            if reg == 4:
                return 'SPILL_FPU(); fpu_frstor(cpu, %s); RELOAD_FPU();' % A
            if reg == 6:
                return 'SPILL_FPU(); fpu_fnsave(cpu, %s); RELOAD_FPU();' % A
        if op == 0xdf:
            if reg == 0:
                return 'FPUSH((double)(int16_t)RD16(%s));' % A
            if reg == 2:
                return 'WR16(%s, (uint16_t)fpu_toint16(cpu, ST(0)));' % A
            if reg == 3:
                return 'WR16(%s, (uint16_t)fpu_toint16(cpu, ST(0))); FPOP();' % A
            if reg == 5:
                return 'FPUSH((double)(int64_t)RD64(%s));' % A
            if reg == 7:
                return 'WR64(%s, (uint64_t)fpu_toint64(cpu, ST(0))); FPOP();' % A
        raise Unsupported('fpu mem %x/%d' % (op, reg))

    def fpu_reg(self, op, reg, rm):
        S = 'ST(%d)' % rm
        if op == 0xd8:
            return self.farith(self.ARITH[reg], 'ST(0)', S)
        if op == 0xdc:
            m = {0: 'add', 1: 'mul', 2: 'com', 3: 'comp', 4: 'subr', 5: 'sub', 6: 'divr', 7: 'div'}[reg]
            if m in ('com', 'comp'):
                return self.farith(m, 'ST(0)', S)
            return self.farith(m, S, 'ST(0)')
        if op == 0xde:
            if reg == 3 and rm == 1:
                return 'FCOM(ST(0), ST(1)); FPOP(); FPOP();'
            m = {0: 'add', 1: 'mul', 2: 'com', 3: 'comp', 4: 'subr', 5: 'sub', 6: 'divr', 7: 'div'}[reg]
            if m in ('com', 'comp'):
                return self.farith(m, 'ST(0)', S) + ' FPOP();'
            return self.farith(m, S, 'ST(0)') + ' FPOP();'
        if op == 0xd9:
            if reg == 0:
                return '{ double v = %s; FPUSH(v); }' % S
            if reg == 1:
                return '{ double v = ST(0); ST(0) = %s; %s = v; }' % (S, S)
            if reg == 2:
                return ';'
            x = 0xe0 + ((reg - 4) << 3) + rm
            tab = {
                0xe0: 'ST(0) = -ST(0);', 0xe1: 'ST(0) = fabs(ST(0));', 0xe4: 'FCOM(ST(0), 0.0);',
                0xe5: 'F_sw = fpu_fxam_sw(ST(0), F_sw);', 0xe8: 'FPUSH(1.0);', 0xe9: 'FPUSH(3.321928094887362347870);',
                0xea: 'FPUSH(1.442695040888963407360);', 0xeb: 'FPUSH(3.141592653589793238463);',
                0xec: 'FPUSH(0.301029995663981195214);', 0xed: 'FPUSH(0.693147180559945309417);',
                0xee: 'FPUSH(0.0);', 0xf0: 'ST(0) = exp2(ST(0)) - 1.0;',
                0xf1: 'ST(1) = ST(1) * log2(ST(0)); FPOP();',
                0xf2: 'ST(0) = tan(ST(0)); FPUSH(1.0);',
                0xf3: 'ST(1) = atan2(ST(1), ST(0)); FPOP();',
                0xf5: 'ST(0) = remainder(ST(0), ST(1)); F_sw &= ~0x400;',
                0xf6: 'F_top = (F_top - 1) & 7;', 0xf7: 'F_top = (F_top + 1) & 7;',
                0xf8: 'ST(0) = fmod(ST(0), ST(1)); F_sw &= ~0x400;',
                0xf9: 'ST(1) = ST(1) * log2(ST(0) + 1.0); FPOP();',
                0xfa: 'ST(0) = sqrt(ST(0));',
                0xfb: '{ double v = ST(0); ST(0) = sin(v); FPUSH(cos(v)); }',
                0xfc: 'ST(0) = fpu_round(cpu, ST(0));',
                0xfd: 'ST(0) = ldexp(ST(0), (int)trunc(ST(1)));',
                0xfe: 'ST(0) = sin(ST(0));', 0xff: 'ST(0) = cos(ST(0));',
            }
            if x in tab:
                return tab[x]
        if op == 0xdd:
            if reg == 0:
                return ';'  # ffree
            if reg == 2:
                return '%s = ST(0);' % S
            if reg == 3:
                return '%s = ST(0); FPOP();' % S
            if reg == 4:
                return 'FCOM(ST(0), %s);' % S
            if reg == 5:
                return 'FCOM(ST(0), %s); FPOP();' % S
        if op == 0xda and reg == 5 and rm == 1:
            return 'FCOM(ST(0), ST(1)); FPOP(); FPOP();'
        if op == 0xdb:
            if reg == 4 and rm == 2:
                return 'F_sw &= 0x7f00;'
            if reg == 4 and rm == 3:
                return 'SPILL_FPU(); fpu_init(cpu); RELOAD_FPU();'
            if reg in (5, 6):
                return 'FCOMI(ST(0), %s);' % S
        if op == 0xdf:
            if reg == 4 and rm == 0:
                return 'eax = (eax & 0xffff0000u) | FSTSW();'
            if reg in (5, 6):
                return 'FCOMI(ST(0), %s); FPOP();' % S
        raise Unsupported('fpu reg %x/%d/%d' % (op, reg, rm))


def _mmx_handlers():
    for mn in ('movd', 'movq', 'pand', 'pandn', 'pxor', 'psllq', 'psrlq', 'psrlw', 'psllw', 'psraw',
               'psrld', 'pslld', 'psrad', 'paddw', 'paddd', 'paddb', 'psubw', 'psubd', 'psubb',
               'pmullw', 'pmulhw', 'punpckhbw', 'punpcklbw', 'punpckhwd', 'punpcklwd', 'punpckhdq',
               'punpckldq', 'packuswb', 'packsswb', 'packssdw', 'paddusb', 'paddusw', 'psubusb',
               'psubusw', 'paddsw', 'psubsw', 'pmaddwd', 'por'):
        if mn == 'por':
            setattr(FuncGen, 'i_por', lambda self, ins: self.mmx(ins))
        else:
            setattr(FuncGen, 'i_' + mn, lambda self, ins: self.mmx(ins))
    # movsd string op vs. SSE2 movsd: SSE is not supported yet, keep string form.


_mmx_handlers()


class Generator:
    def __init__(self, exe, outdir, overrides, comments=False, patches=(), seeds=(), alts_fn=None):
        self.exe_path = exe
        self.img = Image(exe)
        # The original .text is what the runtime verifies; patches (bytes the game's
        # own startup scripts write into its code) are applied before discovery.
        self.orig_text_hash = self.text_hash()
        self.patches = list(patches)
        for a, data in self.patches:
            self.img.mem[a - self.img.base:a - self.img.base + len(data)] = data
        # run-time patches with known bytes: compiled as guarded alternatives
        self.alts = alts_fn(self) if alts_fn else {}
        alt_seeds = set()
        for P, blist in self.alts.items():
            for b in blist:
                for i in self.img.md.disasm(b, P):
                    if i.mnemonic in ('call', 'jmp') or i.mnemonic in BRANCH_JCC:
                        if i.operands and i.operands[0].type == X.X86_OP_IMM:
                            alt_seeds.add(i.operands[0].imm & 0xffffffff)
        self.disc = Discovery(self.img)
        self.disc.extra_seeds = {a for a in set(seeds) | alt_seeds if self.img.in_text(a)}
        self.disc.run()
        bad = [P for P in self.alts if P not in self.disc.insns]
        for P in bad:
            del self.alts[P]
        print('run-time code patches compiled as alternatives: %d sites' % len(self.alts))
        self.outdir = outdir
        self.overrides = overrides  # addr -> host function name
        self.comments = comments
        self.unsupported = {}
        self.imports_used = set(self.img.imports.values())
        # true functions (direct call targets + program entry) are never inlined
        self.call_targets = {self.img.entry}
        for ins in self.disc.insns.values():
            if ins.mnemonic == 'call' and ins.operands[0].type == X.X86_OP_IMM:
                self.call_targets.add(ins.operands[0].imm & 0xffffffff)
        self.call_targets |= set(overrides)
        self.probes = set()
        self.dyn_fields = {}  # insn addr -> {'imm': (addr, size), 'disp': (addr, size)}

    def text_hash(self):
        h = 2166136261
        for sec in self.img.pe.sections:
            if sec.Name.rstrip(b'\0') == b'.text':
                vs, rs = sec.Misc_VirtualSize, sec.SizeOfRawData
                n = rs if (rs < vs or vs == 0) else vs
                for b in sec.get_data()[:n]:
                    h = ((h ^ b) * 16777619) & 0xffffffff
        return h

    def set_dynamic(self, addrs):
        """Addresses the scripts write into code: the containing instruction reads that
        immediate/displacement field from memory at run time."""
        notes = []
        for A in sorted(addrs):
            ins = None
            for s0 in range(A, A - 15, -1):
                i = self.disc.insns.get(s0)
                if i is not None and s0 <= A < s0 + i.size:
                    ins = i
                    break
            if ins is None:
                continue
            f = self.dyn_fields.setdefault(ins.address, {})
            if ins.imm_size and ins.address + ins.imm_offset <= A < ins.address + ins.imm_offset + ins.imm_size:
                f['imm'] = (ins.address + ins.imm_offset, ins.imm_size)
            elif ins.disp_size and ins.address + ins.disp_offset <= A < ins.address + ins.disp_offset + ins.disp_size:
                f['disp'] = (ins.address + ins.disp_offset, ins.disp_size)
            else:
                if not f:
                    del self.dyn_fields[ins.address]
                if (A != ins.address or A not in self.disc.entries) and A not in self.alts:
                    notes.append('%x (%s %s @%x)' % (A, ins.mnemonic, ins.op_str, ins.address))
        print('dynamic code fields: %d instructions' % len(self.dyn_fields))
        if notes:
            print('script writes to opcode bytes (not dynamic): ' + ', '.join(notes[:40]))

    def fn_name(self, a):
        return 'f_%06x' % a

    def fn_for(self, a):
        if a in self.overrides:
            return self.overrides[a]
        if a not in self.disc.entries:
            self.extra_entries.add(a)
        return self.fn_name(a)

    def import_fn(self, dll, name):
        self.imports_used.add((dll, name))
        # decorated stdcall exports (_AIL_startup@0) -> AIL_startup
        import re
        cname = re.sub(r'@\d+$', '', name).lstrip('_')
        cname = re.sub(r'[^A-Za-z0-9_]', '_', cname)
        return 'imp_%s_%s' % (dll.split('.')[0].upper(), cname)

    def body_of(self, entry):
        """Instructions reachable from entry without entering other true functions."""
        seen = set()
        work = [entry]
        insns = self.disc.insns
        while work:
            a = work.pop()
            while a not in seen:
                if a != entry and a in self.call_targets:
                    break
                ins = insns.get(a)
                if ins is None:
                    break
                seen.add(a)
                mn = ins.mnemonic
                if mn in ('ret', 'hlt', 'int3', 'ud2', 'retf', 'iretd'):
                    break
                if mn == 'jmp':
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        a = op.imm & 0xffffffff
                        continue
                    t = self.disc.jtables.get(a)
                    if t:
                        work.extend(t)
                    break
                if mn in BRANCH_JCC:
                    work.append(ins.operands[0].imm & 0xffffffff)
                a += ins.size
        return sorted(seen)

    def run(self):
        os.makedirs(self.outdir, exist_ok=True)
        self.extra_entries = set()
        entries = sorted(e for e in self.disc.entries if e not in self.overrides)
        done = set()
        funcs = []
        while entries:
            for e in entries:
                if e in done:
                    continue
                done.add(e)
                body = self.body_of(e)
                if not body:
                    # a stale jump target (e.g. into bytes a code patch replaced)
                    print('empty body', hex(e))
                    funcs.append((e, 'void %s(CPU *cpu) { rt_unreachable(cpu, 0x%xu); }' % (self.fn_name(e), e)))
                    continue
                fg = FuncGen(self, e, body)
                funcs.append((e, fg.gen()))
            entries = sorted(self.extra_entries - done - set(self.overrides))
            self.extra_entries = set()
        funcs.sort()
        self.write(funcs)

    def write(self, funcs):
        nfiles = 48
        per = (len(funcs) + nfiles - 1) // nfiles
        names = [self.fn_name(e) for e, _ in funcs]
        with open(os.path.join(self.outdir, 'funcs.h'), 'w') as f:
            f.write('/* generated by recomp/codegen.py -- do not edit */\n#pragma once\n#include "cpu.h"\n')
            for n in names:
                f.write('void %s(CPU *cpu);\n' % n)
            for dll, nm in sorted(self.imports_used):
                f.write('void %s(CPU *cpu);\n' % self.import_fn(dll, nm))
            for a, n in sorted(self.overrides.items()):
                f.write('void %s(CPU *cpu);\n' % n)
        for k in range(nfiles):
            chunk = funcs[k * per:(k + 1) * per]
            with open(os.path.join(self.outdir, 'code_%02d.c' % k), 'w') as f:
                f.write('/* generated by recomp/codegen.py -- do not edit */\n#include "funcs.h"\n#include "cpu_ops.h"\n\n')
                for e, code in chunk:
                    f.write(code)
                    f.write('\n\n')
        with open(os.path.join(self.outdir, 'table.c'), 'w') as f:
            f.write('/* generated by recomp/codegen.py -- do not edit */\n#include "funcs.h"\n#include "runtime.h"\n\n')
            f.write('const FuncEntry g_func_table[] = {\n')
            allf = sorted([(e, self.fn_name(e)) for e, _ in funcs] + list(self.overrides.items()))
            for a, n in allf:
                f.write('  {0x%xu, %s},\n' % (a, n))
            f.write('};\nconst int g_func_count = %d;\n\n' % len(allf))
            f.write('const ImportEntry g_import_table[] = {\n')
            for iat, (dll, nm) in sorted(self.img.imports.items()):
                fn = self.import_fn(dll, nm)
                f.write('  {0x%xu, "%s", "%s", %s},\n' % (iat, dll, nm, fn))
            f.write('};\nconst int g_import_count = %d;\n' % len(self.img.imports))
        h = self.orig_text_hash
        with open(os.path.join(self.outdir, 'image_info.h'), 'w') as f:
            f.write('/* generated by recomp/codegen.py -- do not edit */\n#pragma once\n')
            f.write('#define EXPECTED_TEXT_HASH 0x%08xu\n' % h)
            oh = self.img.pe.OPTIONAL_HEADER
            f.write('#define EXPECTED_SIZE %du\n' % os.path.getsize(self.exe_path))
            f.write('#define IMAGE_BASE 0x%xu\n' % oh.ImageBase)
            f.write('#define IMAGE_SIZE 0x%xu\n' % oh.SizeOfImage)
            f.write('#define ENTRY_POINT 0x%xu\n' % (oh.ImageBase + oh.AddressOfEntryPoint))
            f.write('#define TEXT_LO 0x%xu\n#define TEXT_HI 0x%xu\n' % (self.img.text_lo, self.img.text_hi))
        # Startup code patches: the runtime writes the same bytes after loading the
        # verified original image, so guest memory matches the recompiled code.
        with open(os.path.join(self.outdir, 'patches.c'), 'w') as f:
            f.write('/* generated by recomp/codegen.py -- do not edit */\n#include "runtime.h"\n\n')
            f.write('const ImagePatch g_image_patches[] = {\n')
            for a, data in self.patches:
                f.write('  {0x%xu, %d, "%s"},\n' % (a, len(data), ''.join('\\x%02x' % b for b in data)))
            f.write('  {0, 0, 0}\n};\nconst int g_image_patch_count = %d;\n' % len(self.patches))
        # Imports without a host implementation link to a weak stub that stops with its name.
        with open(os.path.join(self.outdir, 'stubs.c'), 'w') as f:
            f.write('/* generated by recomp/codegen.py -- do not edit */\n#include "runtime.h"\n\n')
            for dll, nm in sorted(self.imports_used):
                fn = self.import_fn(dll, nm)
                f.write('__attribute__((weak)) void %s(CPU *cpu) { (void)cpu; rt_fatal("unimplemented import %s!%s"); }\n'
                        % (fn, dll, nm))
        with open(os.path.join(self.outdir, 'imports.txt'), 'w') as f:
            for iat, (dll, nm) in sorted(self.img.imports.items()):
                f.write('%s %s\n' % (dll, nm))


def load_overrides(config):
    """game.ini [recomp] "override = ADDRESS host_function": guest functions replaced
    by host code (only for standard routines such as the CRT memcpy/memmove)."""
    if not config:
        return {}
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
    import gameini
    out = {}
    for v in gameini.get_all(gameini.load(config), 'recomp', 'override'):
        addr, fn = v.split()[:2]
        out[int(addr, 0)] = fn
    return out


def load_dyn_lua(config, exe, g):
    """[recomp] dyn_lua = DIR_OR_FILE ...: code addresses the scripts write to (any hex
    literal inside .text that is not a function-pointer cast or a function entry)."""
    if not config:
        return set()
    import re
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
    import gameini
    root = os.path.dirname(os.path.abspath(exe))
    out = set()
    pat = re.compile(r'0x([0-9A-Fa-f]{6,8})')
    fptr = re.compile(r'ffi\.cast\(\s*["\'][^"\']*\(\s*\*[^"\']*["\']\s*,\s*$')
    for v in gameini.get_all(gameini.load(config), 'recomp', 'dyn_lua'):
        for item in v.split():
            p = os.path.join(root, item)
            files = [p] if os.path.isfile(p) else [os.path.join(d, f) for d, _, fs in os.walk(p) for f in fs
                                                    if f.lower().endswith('.lua')]
            for fn in files:
                if 'asm_crazy_patches' in fn:
                    continue
                src = open(fn, encoding='latin-1').read()
                # ffi.cast("T*", 0xA)[k] = ...: the element written is at A + k * sizeof(T)
                for m in re.finditer(r'ffi\.cast\(\s*"([a-z ]+?)\s*\*"\s*,\s*0x([0-9A-Fa-f]+)\s*\)\s*\[\s*(\d+)\s*\]\s*=', src):
                    n = {'char': 1, 'short': 2, 'int': 4, 'float': 4, 'double': 8}.get(m.group(1).strip().split()[-1], 0)
                    a = int(m.group(2), 16) + int(m.group(3)) * n
                    if n and g.img.in_text(a) and int(m.group(3)):
                        out.add(a)
                for a, n, v in var_writes(src):
                    if g.img.in_text(a):
                        out.add(a)
                for m in pat.finditer(src):
                    a = int(m.group(1), 16)
                    if not g.img.in_text(a) or a in g.disc.entries:
                        continue
                    if fptr.search(src[max(0, m.start() - 80):m.start()]):
                        continue
                    out.add(a)
    return out


def var_writes(src, with_pos=False):
    """`local p = ffi.cast("T*", 0xA)` ... `p[k] = V`: yields (address, size, value or None)
    (preceded by the source position when with_pos)."""
    import re
    size = {'char': 1, 'short': 2, 'int': 4, 'float': 4, 'double': 8}
    for m in re.finditer(r'local\s+(\w+)\s*=\s*ffi\.cast\(\s*"([a-z ]+?)\s*\*"\s*,\s*0x([0-9A-Fa-f]+)\s*\)', src):
        name, n, base = m.group(1), size.get(m.group(2).strip().split()[-1], 0), int(m.group(3), 16)
        if not n:
            continue
        for w in re.finditer(r'\b%s\s*\[\s*(\d+)\s*\]\s*=\s*(0x[0-9A-Fa-f]+|\d+)?' % re.escape(name), src[m.end():]):
            v = int(w.group(2), 0) if w.group(2) else None
            if with_pos:
                yield m.end() + w.start(), base + int(w.group(1)) * n, n, v
            else:
                yield base + int(w.group(1)) * n, n, v


def load_alt_patches(config, exe, g):
    """Code patches the scripts apply at run time (cheat toggles, custom levels), with
    literal bytes: hexStrToMem/PrivateChamAdd(0xA, "hex"), PrivateCast(0xV, "T*", 0xA),
    ffi.cast("T*", 0xA)[i] = 0xV.  Adjacent writes in one file are merged.
    Returns {addr: [bytes, ...]} (several alternatives per site are possible)."""
    if not config:
        return {}
    import re
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
    import gameini
    root = os.path.dirname(os.path.abspath(exe))
    size = {'char': 1, 'uint8_t': 1, 'unsigned char': 1, 'short': 2, 'uint16_t': 2, 'unsigned short': 2,
            'int': 4, 'unsigned int': 4, 'uint32_t': 4, 'long': 4}
    hexcall = re.compile(r'(?:hexStrToMem|PrivateChamAdd)\s*\(\s*(0x[0-9A-Fa-f]+)\s*,\s*"([0-9A-Fa-f ]+)"')
    pcast = re.compile(r'PrivateCast\s*\(\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*"([a-z ]+?)\s*\*"\s*,\s*(0x[0-9A-Fa-f]+)')
    fcast = re.compile(r'ffi\.cast\(\s*"([a-z ]+?)\s*\*"\s*,\s*(0x[0-9A-Fa-f]+)\s*\)\s*\[\s*(\d+)\s*\]\s*=\s*(0x[0-9A-Fa-f]+|\d+)\b')
    out = {}
    static = {a for a, _ in g.patches}
    for v in gameini.get_all(gameini.load(config), 'recomp', 'dyn_lua'):
        for item in v.split():
            p = os.path.join(root, item)
            files = [p] if os.path.isfile(p) else [os.path.join(d, f) for d, _, fs in os.walk(p) for f in fs
                                                    if f.lower().endswith('.lua')]
            static_files = [os.path.basename(x).lower() for line in gameini.get_all(gameini.load(config), 'recomp', 'patch_lua')
                            for x in line.split()]
            for fn in files:
                if os.path.basename(fn).lower() in static_files:
                    continue
                src = open(fn, encoding='latin-1').read()
                src = re.sub(r'--\[\[.*?\]\]', '', src, flags=re.S)
                src = re.sub(r'--[^\n]*', '', src)
                writes = []  # (source position, address, bytes)
                for m in hexcall.finditer(src):
                    writes.append((m.start(), int(m.group(1), 16), bytes(int(x, 16) for x in m.group(2).split())))
                for m in pcast.finditer(src):
                    n = size.get(m.group(2).strip(), 0)
                    if n:
                        writes.append((m.start(), int(m.group(3), 16),
                                       (int(m.group(1), 0) & ((1 << 8 * n) - 1)).to_bytes(n, 'little')))
                for m in fcast.finditer(src):
                    n = size.get(m.group(1).strip(), 0)
                    if n:
                        a = int(m.group(2), 16) + int(m.group(3)) * n
                        writes.append((m.start(), a, (int(m.group(4), 0) & ((1 << 8 * n) - 1)).to_bytes(n, 'little')))
                for pos, a, n, v in var_writes(src, with_pos=True):
                    if v is not None:
                        writes.append((pos, a, (v & ((1 << 8 * n) - 1)).to_bytes(n, 'little')))
                writes = [w for w in writes if g.img.in_text(w[1]) and w[1] not in static]
                # consecutive statements that write consecutive bytes form one patch
                writes.sort()
                merged = []
                for pos, a, b in writes:
                    if merged and merged[-1][0] + len(merged[-1][1]) == a:
                        merged[-1] = (merged[-1][0], merged[-1][1] + b)
                    else:
                        merged.append((a, b))
                for a, b in merged:
                    out.setdefault(a, [])
                    if b not in out[a]:
                        out[a].append(b)
    return out


def load_probes(config):
    """[recomp] probe = ADDR ...: log registers when these instructions run (debugging)."""
    if not config:
        return set()
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
    import gameini
    out = set()
    for v in gameini.get_all(gameini.load(config), 'recomp', 'probe'):
        out |= {int(x, 0) for x in v.split()}
    return out


def load_patches(config, exe):
    """[recomp] patch_lua = PATH: a Lua file of `hexStrToMem(0xADDR, "AA BB ..")` calls that
    the game's own scripts run at startup to patch its code (CrazyHook).  PATH is relative
    to the exe's folder."""
    if not config:
        return []
    import re
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
    import gameini
    out = []
    for v in [x for line in gameini.get_all(gameini.load(config), 'recomp', 'patch_lua') for x in line.split()]:
        path = os.path.join(os.path.dirname(os.path.abspath(exe)), v.replace('\\', '/'))
        src = open(path, encoding='latin-1').read()
        src = re.sub(r'--\[\[.*?\]\]', '', src, flags=re.S)
        src = re.sub(r'--[^\n]*', '', src)
        for m in re.finditer(r'hexStrToMem\s*\(\s*(0x[0-9A-Fa-f]+)\s*,\s*"([0-9A-Fa-f ]*)"\s*\)', src):
            out.append((int(m.group(1), 16), bytes(int(x, 16) for x in m.group(2).split())))
        for m in re.finditer(r'\bsInt\s*\(\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)', src):
            out.append((int(m.group(1), 16), (int(m.group(2), 0) & 0xffffffff).to_bytes(4, 'little')))
    return out


def load_lua_seeds(config, exe):
    """[recomp] seed_lua = DIR_OR_FILE ...: code addresses that scripts call through
    FFI function pointers (`ffi.cast("int (*)(int)", 0x420EE0)`) become function entries."""
    if not config:
        return set()
    import re
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
    import gameini
    root = os.path.dirname(os.path.abspath(exe))
    out = set()
    # [recomp] seed = ADDR ...: indirect targets discovery missed (found at runtime)
    for v in gameini.get_all(gameini.load(config), 'recomp', 'seed'):
        out |= {int(x, 0) for x in v.split()}
    pat = re.compile(r'ffi\.cast\(\s*["\'][^"\']*\(\s*\*[^"\']*["\']\s*,\s*(0x[0-9A-Fa-f]+)')
    for v in gameini.get_all(gameini.load(config), 'recomp', 'seed_lua'):
        for item in v.split():
            p = os.path.join(root, item)
            files = [p] if os.path.isfile(p) else [os.path.join(d, f) for d, _, fs in os.walk(p) for f in fs
                                                    if f.lower().endswith('.lua')]
            for fn in files:
                for m in pat.finditer(open(fn, encoding='latin-1').read()):
                    out.add(int(m.group(1), 16))
    return out


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('outdir')
    ap.add_argument('--config', help='games/<name>/game.ini ([recomp] overrides)')
    ap.add_argument('--comments', action='store_true', help='annotate each line with its x86 address')
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    g = Generator(a.exe, a.outdir, load_overrides(a.config), a.comments,
                  load_patches(a.config, a.exe), load_lua_seeds(a.config, a.exe),
                  lambda gen: load_alt_patches(a.config, a.exe, gen))
    g.probes = load_probes(a.config)
    g.set_dynamic(load_dyn_lua(a.config, a.exe, g))
    print('patches:', len(g.patches), 'lua seeds:', len(g.disc.extra_seeds))
    g.run()
    print('functions:', len(g.disc.entries), 'unsupported forms:')
    for k, v in sorted(g.unsupported.items(), key=lambda x: -x[1]):
        print('  %5d %s' % (v, k))


if __name__ == '__main__':
    main()

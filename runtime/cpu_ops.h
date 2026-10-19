/* Macros used by the generated code (gen/code_*.c). */
#pragma once
#include "cpu.h"

#define PROLOGUE() \
    uint32_t eax = cpu->eax, ecx = cpu->ecx, edx = cpu->edx, ebx = cpu->ebx; \
    uint32_t esp = cpu->esp, ebp = cpu->ebp, esi = cpu->esi, edi = cpu->edi; \
    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0; \
    (void)cf; (void)zf; (void)sf; (void)of; (void)pf

#define SPILL() do { \
    cpu->eax = eax; cpu->ecx = ecx; cpu->edx = edx; cpu->ebx = ebx; \
    cpu->esp = esp; cpu->ebp = ebp; cpu->esi = esi; cpu->edi = edi; } while (0)

#define RELOAD() do { \
    eax = cpu->eax; ecx = cpu->ecx; edx = cpu->edx; ebx = cpu->ebx; \
    esp = cpu->esp; ebp = cpu->ebp; esi = cpu->esi; edi = cpu->edi; } while (0)


#ifdef RECOMP_NO_RA_CHECK
#define CHECK_RA(ra) ((void)0)
#else
#define CHECK_RA(ra) do { if (__builtin_expect(cpu->ret_addr != (ra), 0)) { \
    rt_ra_mismatch(cpu, (ra)); RELOAD(); } } while (0)
#endif

#define UNIMPL(a, s)      do { SPILL(); rt_unimpl(cpu, (a), (s)); } while (0)
#define BAD_JUMP(a, t)    do { SPILL(); rt_bad_jump(cpu, (a), (t)); } while (0)
#define DIV0(a)           do { SPILL(); rt_div0(cpu, (a)); } while (0)
#define UNREACHABLE(f)    do { SPILL(); rt_unreachable(cpu, (f)); } while (0)
#define GUEST_INT(a, n)   do { SPILL(); rt_guest_int(cpu, (a), (n)); RELOAD(); } while (0)

#define PARITY(r) ((uint32_t)(__builtin_parity((uint32_t)(r) & 0xffu) ^ 1))

#define MAKE_EFLAGS() ((cf) | 2u | ((pf) << 2) | ((zf) << 6) | ((sf) << 7) | (cpu->df << 10) | ((of) << 11) | 0x200u)
#define LOAD_EFLAGS(v) do { uint32_t _f = (v); cf = _f & 1; pf = (_f >> 2) & 1; zf = (_f >> 6) & 1; \
    sf = (_f >> 7) & 1; cpu->df = (_f >> 10) & 1; of = (_f >> 11) & 1; } while (0)

/* ---- string instructions ------------------------------------------------ */
static inline void rep_movs(CPU *cpu, uint32_t *ecx, uint32_t *esi, uint32_t *edi, int size) {
    uint32_t n = *ecx;
    if (!n) return;
    uint32_t bytes = n * (uint32_t)size;
    if (!cpu->df && (*edi - *esi >= bytes)) {
        /* forward copy without destructive overlap */
        memmove(g_mem + *edi, g_mem + *esi, bytes);
        *esi += bytes; *edi += bytes;
    } else {
        int32_t d = cpu->df ? -size : size;
        while (n--) {
            if (size == 1) WR8(*edi, RD8(*esi));
            else if (size == 2) WR16(*edi, RD16(*esi));
            else WR32(*edi, RD32(*esi));
            *esi += d; *edi += d;
        }
    }
    *ecx = 0;
}

static inline void rep_stos(CPU *cpu, uint32_t *ecx, uint32_t *edi, uint32_t eax, int size) {
    uint32_t n = *ecx;
    if (!n) return;
    if (!cpu->df) {
        if (size == 1) { memset(g_mem + *edi, (int)(eax & 0xff), n); *edi += n; }
        else if (size == 2) { for (uint32_t i = 0; i < n; i++) WR16(*edi + i * 2, eax); *edi += n * 2; }
        else {
            if (eax == 0) memset(g_mem + *edi, 0, n * 4);
            else for (uint32_t i = 0; i < n; i++) WR32(*edi + i * 4, eax);
            *edi += n * 4;
        }
    } else {
        while (n--) {
            if (size == 1) WR8(*edi, eax); else if (size == 2) WR16(*edi, eax); else WR32(*edi, eax);
            *edi -= size;
        }
    }
    *ecx = 0;
}

#define REP_MOVS1() rep_movs(cpu, &ecx, &esi, &edi, 1)
#define REP_MOVS2() rep_movs(cpu, &ecx, &esi, &edi, 2)
#define REP_MOVS4() rep_movs(cpu, &ecx, &esi, &edi, 4)
#define REP_STOS1() rep_stos(cpu, &ecx, &edi, eax, 1)
#define REP_STOS2() rep_stos(cpu, &ecx, &edi, eax, 2)
#define REP_STOS4() rep_stos(cpu, &ecx, &edi, eax, 4)

/* ---- x87 ------------------------------------------------------------------- */
/* Functions that use the FPU keep the register stack in locals (F_fpr/F_top/F_sw)
 * and spill them to the CPU struct around calls. */
#define FPU_LOCALS() double F_fpr[8]; uint32_t F_top, F_sw; RELOAD_FPU()
#define SPILL_FPU() do { memcpy(cpu->fpr, F_fpr, sizeof F_fpr); cpu->ftop = F_top; cpu->fsw = F_sw; } while (0)
#define RELOAD_FPU() do { memcpy(F_fpr, cpu->fpr, sizeof F_fpr); F_top = cpu->ftop; F_sw = cpu->fsw; } while (0)
#define ST(i) F_fpr[(F_top + (i)) & 7]
#define FPUSH(v) do { double _v = (v); F_top = (F_top - 1) & 7; ST(0) = _v; } while (0)
#define FPOP() (F_top = (F_top + 1) & 7)
#define FSTSW() ((uint16_t)((F_sw & ~0x3800u) | ((F_top & 7) << 11)))

static inline uint32_t fpu_com_sw(uint32_t sw, double a, double b) {
    uint32_t c;
    if (a != a || b != b) c = 0x4500;       /* C3 C2 C0 */
    else if (a > b) c = 0;
    else if (a < b) c = 0x100;              /* C0 */
    else c = 0x4000;                        /* C3 */
    return (sw & ~0x4700u) | c;
}
#define FCOM(a, b) (F_sw = fpu_com_sw(F_sw, (a), (b)))

static inline uint32_t fpu_fxam_sw(double v, uint32_t sw) {
    uint32_t c;
    switch (fpclassify(v)) {
    case FP_NAN: c = 0x0100; break;
    case FP_INFINITE: c = 0x0500; break;
    case FP_ZERO: c = 0x4000; break;
    case FP_SUBNORMAL: c = 0x4400; break;
    default: c = 0x0400; break;
    }
    if (signbit(v)) c |= 0x0200;
    return (sw & ~0x4700u) | c;
}

/* MMX registers likewise live in locals in functions that use them. */
#define MMX_LOCALS() uint64_t M_mm[8]; RELOAD_MMX()
#define SPILL_MMX() memcpy(cpu->mm, M_mm, sizeof M_mm)
#define RELOAD_MMX() memcpy(M_mm, cpu->mm, sizeof M_mm)
#define FCOMI(a, b) do { double _a = (a), _b = (b); \
    if (_a != _a || _b != _b) { zf = pf = cf = 1; } \
    else { zf = _a == _b; cf = _a < _b; pf = 0; } of = sf = 0; } while (0)

/* ---- MMX --------------------------------------------------------------------- */
/* MMX on 64-bit integers using compiler vector extensions (SSE2 on x86, NEON on ARM). */
typedef int8_t   v8i8   __attribute__((vector_size(8)));
typedef uint8_t  v8u8   __attribute__((vector_size(8)));
typedef int16_t  v4i16  __attribute__((vector_size(8)));
typedef uint16_t v4u16  __attribute__((vector_size(8)));
typedef int32_t  v2i32  __attribute__((vector_size(8)));
typedef uint32_t v2u32  __attribute__((vector_size(8)));
typedef int32_t  v4i32  __attribute__((vector_size(16)));
#define MMX_AS(T, x) ({ T _t; uint64_t _x = (x); memcpy(&_t, &_x, 8); _t; })
#define MMX_Q(v) ({ uint64_t _q; __typeof__(v) _v = (v); memcpy(&_q, &_v, 8); _q; })

static inline uint64_t mmx_or(uint64_t a, uint64_t b) { return a | b; }
static inline uint64_t mmx_and(uint64_t a, uint64_t b) { return a & b; }
static inline uint64_t mmx_andn(uint64_t a, uint64_t b) { return ~a & b; }
static inline uint64_t mmx_xor(uint64_t a, uint64_t b) { return a ^ b; }
static inline uint64_t mmx_psllq(uint64_t a, uint64_t c) { return c > 63 ? 0 : a << c; }
static inline uint64_t mmx_psrlq(uint64_t a, uint64_t c) { return c > 63 ? 0 : a >> c; }
static inline uint64_t mmx_paddw(uint64_t a, uint64_t b) { return MMX_Q(MMX_AS(v4u16, a) + MMX_AS(v4u16, b)); }
static inline uint64_t mmx_paddd(uint64_t a, uint64_t b) { return MMX_Q(MMX_AS(v2u32, a) + MMX_AS(v2u32, b)); }
static inline uint64_t mmx_paddb(uint64_t a, uint64_t b) { return MMX_Q(MMX_AS(v8u8, a) + MMX_AS(v8u8, b)); }
static inline uint64_t mmx_psubw(uint64_t a, uint64_t b) { return MMX_Q(MMX_AS(v4u16, a) - MMX_AS(v4u16, b)); }
static inline uint64_t mmx_psubd(uint64_t a, uint64_t b) { return MMX_Q(MMX_AS(v2u32, a) - MMX_AS(v2u32, b)); }
static inline uint64_t mmx_psubb(uint64_t a, uint64_t b) { return MMX_Q(MMX_AS(v8u8, a) - MMX_AS(v8u8, b)); }
static inline uint64_t mmx_pmullw(uint64_t a, uint64_t b) { return MMX_Q(MMX_AS(v4u16, a) * MMX_AS(v4u16, b)); }
static inline uint64_t mmx_pmulhw(uint64_t a, uint64_t b) {
    v4i16 x = MMX_AS(v4i16, a), y = MMX_AS(v4i16, b), r;
    for (int i = 0; i < 4; i++) r[i] = (int16_t)(((int32_t)x[i] * y[i]) >> 16);
    return MMX_Q(r);
}
static inline uint64_t mmx_psrlw(uint64_t a, uint64_t c) { return c > 15 ? 0 : MMX_Q(MMX_AS(v4u16, a) >> (uint16_t)c); }
static inline uint64_t mmx_psllw(uint64_t a, uint64_t c) { return c > 15 ? 0 : MMX_Q(MMX_AS(v4u16, a) << (uint16_t)c); }
static inline uint64_t mmx_psraw(uint64_t a, uint64_t c) { return MMX_Q(MMX_AS(v4i16, a) >> (int16_t)(c > 15 ? 15 : c)); }
static inline uint64_t mmx_psrld(uint64_t a, uint64_t c) { return c > 31 ? 0 : MMX_Q(MMX_AS(v2u32, a) >> (uint32_t)c); }
static inline uint64_t mmx_pslld(uint64_t a, uint64_t c) { return c > 31 ? 0 : MMX_Q(MMX_AS(v2u32, a) << (uint32_t)c); }
static inline uint64_t mmx_psrad(uint64_t a, uint64_t c) { return MMX_Q(MMX_AS(v2i32, a) >> (int32_t)(c > 31 ? 31 : c)); }
static inline uint64_t mmx_punpckhbw(uint64_t a, uint64_t b) {
    return MMX_Q(__builtin_shufflevector(MMX_AS(v8u8, a), MMX_AS(v8u8, b), 4, 12, 5, 13, 6, 14, 7, 15));
}
static inline uint64_t mmx_punpcklbw(uint64_t a, uint64_t b) {
    return MMX_Q(__builtin_shufflevector(MMX_AS(v8u8, a), MMX_AS(v8u8, b), 0, 8, 1, 9, 2, 10, 3, 11));
}
static inline uint64_t mmx_punpckhwd(uint64_t a, uint64_t b) {
    return MMX_Q(__builtin_shufflevector(MMX_AS(v4u16, a), MMX_AS(v4u16, b), 2, 6, 3, 7));
}
static inline uint64_t mmx_punpcklwd(uint64_t a, uint64_t b) {
    return MMX_Q(__builtin_shufflevector(MMX_AS(v4u16, a), MMX_AS(v4u16, b), 0, 4, 1, 5));
}
static inline uint64_t mmx_punpckhdq(uint64_t a, uint64_t b) { return (a >> 32) | (b & 0xffffffff00000000ull); }
static inline uint64_t mmx_punpckldq(uint64_t a, uint64_t b) { return (a & 0xffffffffull) | (b << 32); }
static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline uint8_t satu8(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static inline int8_t sat8(int32_t v) { return v < -128 ? -128 : v > 127 ? 127 : (int8_t)v; }
#define MMX_LANES(name, T, n, expr) \
    static inline uint64_t name(uint64_t A, uint64_t B) { T a = MMX_AS(T, A), b = MMX_AS(T, B), r; \
        for (int i = 0; i < n; i++) r[i] = (expr); return MMX_Q(r); }
MMX_LANES(mmx_paddusb, v8u8, 8, (a[i] + b[i]) > 255 ? 255 : a[i] + b[i])
MMX_LANES(mmx_paddusw, v4u16, 4, (a[i] + b[i]) > 65535 ? 65535 : a[i] + b[i])
MMX_LANES(mmx_psubusb, v8u8, 8, a[i] > b[i] ? a[i] - b[i] : 0)
MMX_LANES(mmx_psubusw, v4u16, 4, a[i] > b[i] ? a[i] - b[i] : 0)
MMX_LANES(mmx_paddsw, v4i16, 4, sat16(a[i] + b[i]))
MMX_LANES(mmx_psubsw, v4i16, 4, sat16(a[i] - b[i]))
static inline uint64_t mmx_packuswb(uint64_t A, uint64_t B) {
    v4i16 a = MMX_AS(v4i16, A), b = MMX_AS(v4i16, B);
    v8u8 r;
    for (int i = 0; i < 4; i++) { r[i] = satu8(a[i]); r[4 + i] = satu8(b[i]); }
    return MMX_Q(r);
}
static inline uint64_t mmx_packsswb(uint64_t A, uint64_t B) {
    v4i16 a = MMX_AS(v4i16, A), b = MMX_AS(v4i16, B);
    v8i8 r;
    for (int i = 0; i < 4; i++) { r[i] = sat8(a[i]); r[4 + i] = sat8(b[i]); }
    return MMX_Q(r);
}
static inline uint64_t mmx_packssdw(uint64_t A, uint64_t B) {
    v2i32 a = MMX_AS(v2i32, A), b = MMX_AS(v2i32, B);
    v4i16 r = {sat16(a[0]), sat16(a[1]), sat16(b[0]), sat16(b[1])};
    return MMX_Q(r);
}
static inline uint64_t mmx_pmaddwd(uint64_t A, uint64_t B) {
    v4i16 a = MMX_AS(v4i16, A), b = MMX_AS(v4i16, B);
    v2i32 r = {(int32_t)a[0] * b[0] + (int32_t)a[1] * b[1], (int32_t)a[2] * b[2] + (int32_t)a[3] * b[3]};
    return MMX_Q(r);
}

/* [recomp] probe: log the registers the first times an instruction runs */
void rt_probe(uint32_t addr, uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx, uint32_t esp, uint32_t ebp,
              uint32_t esi, uint32_t edi);
#define PROBE(a) rt_probe(a, eax, ecx, edx, ebx, esp, ebp, esi, edi)

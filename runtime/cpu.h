/* Guest CPU state and guest memory access for the recompiled game exe. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t ret_addr;   /* return address popped by the last guest `ret` */
    uint32_t df;         /* direction flag */
    uint32_t fs_base;    /* guest address of this thread's TEB */
    /* x87 */
    double fpr[8];
    uint32_t ftop;
    uint32_t fsw;        /* condition codes / exception bits (TOP kept in ftop) */
    uint32_t fcw;
    /* MMX */
    uint64_t mm[8];
    /* host bookkeeping */
    int thread_index;
    int depth;
} CPU;

/* Base of the 4 GiB guest address space. */
extern uint8_t *g_mem;

typedef uint8_t  gu8;
typedef uint16_t gu16 __attribute__((aligned(1), may_alias));
typedef uint32_t gu32 __attribute__((aligned(1), may_alias));
typedef uint64_t gu64 __attribute__((aligned(1), may_alias));
typedef float    gf32 __attribute__((aligned(1), may_alias));
typedef double   gf64 __attribute__((aligned(1), may_alias));

#define GPTR(a) ((void *)(g_mem + (uint32_t)(a)))
#define RD8(a)  (*(const gu8  *)(g_mem + (uint32_t)(a)))
#define RD16(a) (*(const gu16 *)(g_mem + (uint32_t)(a)))
#define RD32(a) (*(const gu32 *)(g_mem + (uint32_t)(a)))
#define RD64(a) (*(const gu64 *)(g_mem + (uint32_t)(a)))
#define WR8(a, v)  (*(gu8  *)(g_mem + (uint32_t)(a)) = (uint8_t)(v))
#define WR16(a, v) (*(gu16 *)(g_mem + (uint32_t)(a)) = (uint16_t)(v))
#define WR32(a, v) (*(gu32 *)(g_mem + (uint32_t)(a)) = (uint32_t)(v))
#define WR64(a, v) (*(gu64 *)(g_mem + (uint32_t)(a)) = (uint64_t)(v))
#define RDF32(a) ((double)*(const gf32 *)(g_mem + (uint32_t)(a)))
#define RDF64(a) (*(const gf64 *)(g_mem + (uint32_t)(a)))
#define WRF32(a, v) (*(gf32 *)(g_mem + (uint32_t)(a)) = (float)(v))
#define WRF64(a, v) (*(gf64 *)(g_mem + (uint32_t)(a)) = (double)(v))

typedef void (*GuestFn)(CPU *cpu);

/* Fatal-path helpers (runtime/cpu.c). */
__attribute__((noreturn)) void rt_unimpl(CPU *cpu, uint32_t addr, const char *what);
__attribute__((noreturn)) void rt_bad_jump(CPU *cpu, uint32_t addr, uint32_t tgt);
__attribute__((noreturn)) void rt_div0(CPU *cpu, uint32_t addr);
__attribute__((noreturn)) void rt_unreachable(CPU *cpu, uint32_t fn);
void rt_guest_int(CPU *cpu, uint32_t addr, int n);
void rt_ra_mismatch(CPU *cpu, uint32_t expected);
void call_indirect(CPU *cpu, uint32_t target);
void host_cpuid(CPU *cpu);
uint64_t host_rdtsc(void);

/* x87 helpers (runtime/cpu.c) */
void fpu_init(CPU *cpu);
void fpu_fxam(CPU *cpu);
void fpu_ldenv(CPU *cpu, uint32_t a);
void fpu_stenv(CPU *cpu, uint32_t a);
void fpu_frstor(CPU *cpu, uint32_t a);
void fpu_fnsave(CPU *cpu, uint32_t a);
double fpu_ld80(uint32_t a);
void fpu_st80(uint32_t a, double v);
double fpu_round(CPU *cpu, double v);
int32_t fpu_toint32(CPU *cpu, double v);
int16_t fpu_toint16(CPU *cpu, double v);
int64_t fpu_toint64(CPU *cpu, double v);

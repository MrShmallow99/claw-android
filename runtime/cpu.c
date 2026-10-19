/* CPU runtime: dispatch, thunks, host->guest calls, threads, x87 helpers. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "runtime.h"
#include "image_info.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif

/* ---- logging --------------------------------------------------------------- */
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
FILE *g_logfile;

void rt_log(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&g_log_lock);
    va_start(ap, fmt);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "GamePort", fmt, ap);
#else
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
#endif
    va_end(ap);
    if (g_logfile) {
        va_start(ap, fmt);
        vfprintf(g_logfile, fmt, ap);
        fputc('\n', g_logfile);
        fflush(g_logfile);
        va_end(ap);
    }
    pthread_mutex_unlock(&g_log_lock);
}

void platform_fatal_message(const char *msg);

void rt_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("FATAL: %s", buf);
    CPU *cpu = cpu_current();
    if (cpu)
        rt_log("  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x",
               cpu->eax, cpu->ecx, cpu->edx, cpu->ebx, cpu->esp, cpu->ebp, cpu->esi, cpu->edi);
    if (cpu && cpu->esp) {
        /* show likely return addresses on the guest stack */
        char line[512];
        int n = 0;
        n += snprintf(line + n, sizeof line - n, "  stack code ptrs:");
        for (int i = 0; i < 256 && n < 480; i++) {
            uint32_t v = RD32(cpu->esp + i * 4);
            if (v >= TEXT_LO && v < TEXT_HI) n += snprintf(line + n, sizeof line - n, " %x", v);
        }
        rt_log("%s", line);
    }
    platform_fatal_message(buf);
    abort();
}

void rt_unimpl(CPU *cpu, uint32_t addr, const char *what) {
    (void)cpu;
    rt_fatal("unimplemented instruction at %08x: %s", addr, what);
}
void rt_bad_jump(CPU *cpu, uint32_t addr, uint32_t tgt) {
    (void)cpu;
    rt_fatal("jump table at %08x went to unknown target %08x", addr, tgt);
}
void rt_div0(CPU *cpu, uint32_t addr) {
    (void)cpu;
    rt_fatal("integer divide error at %08x", addr);
}
void rt_unreachable(CPU *cpu, uint32_t fn) {
    (void)cpu;
    rt_fatal("fell off the end of function %08x", fn);
}
void rt_guest_int(CPU *cpu, uint32_t addr, int n) {
    (void)cpu;
    rt_log("guest executed int %d at %08x (ignored)", n, addr);
}
void rt_ra_mismatch(CPU *cpu, uint32_t expected) {
    rt_fatal("return address mismatch: expected %08x, guest returned to %08x", expected, cpu->ret_addr);
}

/* ---- function lookup ------------------------------------------------------------ */
static GuestFn *g_text_map;

#define MAX_THUNKS 4096
static struct { GuestFn fn; const char *name; } g_thunks[MAX_THUNKS];
static int g_thunk_count;
static pthread_mutex_t g_thunk_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t thunk_register(GuestFn fn, const char *name) {
    pthread_mutex_lock(&g_thunk_lock);
    for (int i = 0; i < g_thunk_count; i++)
        if (g_thunks[i].fn == fn) {
            pthread_mutex_unlock(&g_thunk_lock);
            return THUNK_BASE + i * 16;
        }
    if (g_thunk_count >= MAX_THUNKS) rt_fatal("too many thunks");
    int i = g_thunk_count++;
    g_thunks[i].fn = fn;
    g_thunks[i].name = name;
    pthread_mutex_unlock(&g_thunk_lock);
    return THUNK_BASE + i * 16;
}

const char *thunk_name(uint32_t addr) {
    uint32_t i = (addr - THUNK_BASE) / 16;
    if (addr >= THUNK_BASE && i < (uint32_t)g_thunk_count) return g_thunks[i].name;
    return NULL;
}

void funcs_init(void) {
    g_text_map = calloc(TEXT_HI - TEXT_LO, sizeof(GuestFn));
    for (int i = 0; i < g_func_count; i++) {
        uint32_t a = g_func_table[i].addr;
        if (a >= TEXT_LO && a < TEXT_HI) g_text_map[a - TEXT_LO] = g_func_table[i].fn;
    }
}

GuestFn lookup_guest_fn(uint32_t addr) {
    if (addr >= TEXT_LO && addr < TEXT_HI) return g_text_map[addr - TEXT_LO];
    if (addr >= THUNK_BASE) {
        uint32_t i = (addr - THUNK_BASE) / 16;
        if (i < (uint32_t)g_thunk_count && (addr & 15) == 0) return g_thunks[i].fn;
    }
    return NULL;
}

void call_indirect(CPU *cpu, uint32_t target) {
    /* PORT_TRACE_IND=1: log each indirect target the first time it is reached */
    static int trace = -1;
    static uint8_t *seen;
    if (trace < 0) {
        trace = getenv("PORT_TRACE_IND") != NULL;
        if (trace) seen = calloc(TEXT_HI - TEXT_LO, 1);
    }
    if (trace && target >= TEXT_LO && target < TEXT_HI && !seen[target - TEXT_LO]) {
        seen[target - TEXT_LO] = 1;
        rt_log("ind %x (ret %x)", target, RD32(cpu->esp));
    }
    GuestFn fn = lookup_guest_fn(target);
    if (!fn) rt_fatal("indirect call/jump to unknown address %08x", target);
    fn(cpu);
}

/* ---- host -> guest calls ------------------------------------------------------- */
#define HOST_RETURN_MAGIC 0xFFFFFFF0u

uint32_t guest_call(CPU *cpu, uint32_t addr, int nargs, const uint32_t *args) {
    GuestFn fn = lookup_guest_fn(addr);
    if (!fn) rt_fatal("guest_call to unknown address %08x", addr);
    CPU saved = *cpu;
    uint32_t esp = cpu->esp;
    for (int i = nargs - 1; i >= 0; i--) {
        esp -= 4;
        WR32(esp, args[i]);
    }
    esp -= 4;
    WR32(esp, HOST_RETURN_MAGIC);
    cpu->esp = esp;
    fn(cpu);
    if (cpu->ret_addr != HOST_RETURN_MAGIC)
        rt_fatal("guest callback %08x returned to %08x", addr, cpu->ret_addr);
    uint32_t r = cpu->eax;
    uint32_t fpr_top = cpu->ftop;
    double fpr[8];
    memcpy(fpr, cpu->fpr, sizeof fpr);
    uint32_t edx = cpu->edx;
    *cpu = saved;
    cpu->eax = r;
    cpu->edx = edx;
    /* keep x87 results (float-returning callbacks) */
    cpu->ftop = fpr_top;
    memcpy(cpu->fpr, fpr, sizeof fpr);
    return r;
}

/* ---- threads -------------------------------------------------------------------- */
static __thread CPU *t_cpu;
uint32_t mem_low_alloc(uint32_t size);
static int g_thread_counter;

CPU *cpu_current(void) { return t_cpu; }
void cpu_set_current(CPU *c) { t_cpu = c; }

CPU *cpu_new_thread(uint32_t stack_size) {
    CPU *cpu = calloc(1, sizeof(CPU));
    uint32_t stack = mem_reserve(stack_size, 0x10000);
    if (!stack) rt_fatal("cannot allocate guest stack");
    uint32_t teb = mem_low_alloc(0x1000);
    cpu->thread_index = __atomic_fetch_add(&g_thread_counter, 1, __ATOMIC_SEQ_CST);
    WR32(teb + 0x00, 0xffffffffu);          /* SEH chain end */
    WR32(teb + 0x04, stack + stack_size);   /* stack base */
    WR32(teb + 0x08, stack);                /* stack limit */
    WR32(teb + 0x18, teb);                  /* self */
    WR32(teb + 0x20, 0x100);                /* process id */
    WR32(teb + 0x24, 0x200 + cpu->thread_index * 4); /* thread id */
    cpu->fs_base = teb;
    cpu->esp = stack + stack_size - 16;
    fpu_init(cpu);
    return cpu;
}

/* ---- misc instructions ------------------------------------------------------------ */
void host_cpuid(CPU *cpu) {
    switch (cpu->eax) {
    case 0:
        cpu->eax = 1;
        cpu->ebx = 0x756e6547; /* "Genu" */
        cpu->edx = 0x49656e69; /* "ineI" */
        cpu->ecx = 0x6c65746e; /* "ntel" */
        break;
    case 1:
        cpu->eax = 0x0652; /* Pentium II */
        cpu->ebx = 0;
        cpu->ecx = 0;
        cpu->edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23);
        break;
    default:
        cpu->eax = cpu->ebx = cpu->ecx = cpu->edx = 0;
    }
}

uint64_t host_rdtsc(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 400000000ull + (uint64_t)ts.tv_nsec * 2 / 5;
}

/* ---- x87 helpers -------------------------------------------------------------------- */
void fpu_init(CPU *cpu) {
    cpu->fcw = 0x27f;
    cpu->fsw = 0;
    cpu->ftop = 0;
    memset(cpu->fpr, 0, sizeof cpu->fpr);
}

void fpu_fxam(CPU *cpu) {
    double v = cpu->fpr[cpu->ftop & 7];
    uint32_t c;
    switch (fpclassify(v)) {
    case FP_NAN: c = 0x0100; break;
    case FP_INFINITE: c = 0x0500; break;
    case FP_ZERO: c = 0x4000; break;
    case FP_SUBNORMAL: c = 0x4400; break;
    default: c = 0x0400; break;
    }
    if (signbit(v)) c |= 0x0200;
    cpu->fsw = (cpu->fsw & ~0x4700u) | c;
}

double fpu_round(CPU *cpu, double v) {
    switch ((cpu->fcw >> 10) & 3) {
    case 0: return nearbyint(v); /* host default mode is round-to-nearest-even */
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

int32_t fpu_toint32(CPU *cpu, double v) {
    double r = fpu_round(cpu, v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return INT32_MIN;
    return (int32_t)r;
}

int16_t fpu_toint16(CPU *cpu, double v) {
    double r = fpu_round(cpu, v);
    if (!(r >= -32768.0 && r <= 32767.0)) return INT16_MIN;
    return (int16_t)r;
}

int64_t fpu_toint64(CPU *cpu, double v) {
    double r = fpu_round(cpu, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return INT64_MIN;
    return (int64_t)r;
}

double fpu_ld80(uint32_t a) {
    uint64_t mant = RD64(a);
    uint16_t se = RD16(a + 8);
    int sign = se >> 15;
    int exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7fff) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void fpu_st80(uint32_t a, double v) {
    uint64_t mant = 0;
    uint16_t se = 0;
    if (signbit(v)) se = 0x8000;
    if (isnan(v)) { se |= 0x7fff; mant = 0xc000000000000000ull; }
    else if (isinf(v)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (v != 0.0) {
        int e;
        double m = frexp(fabs(v), &e); /* m in [0.5,1) */
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    WR64(a, mant);
    WR16(a + 8, se);
}

static uint16_t fpu_tagword(CPU *cpu) { (void)cpu; return 0; }

void fpu_stenv(CPU *cpu, uint32_t a) {
    WR32(a + 0, cpu->fcw | 0xffff0000u);
    WR32(a + 4, ((cpu->fsw & ~0x3800u) | ((cpu->ftop & 7) << 11)) | 0xffff0000u);
    WR32(a + 8, fpu_tagword(cpu) | 0xffff0000u);
    for (int i = 12; i < 28; i += 4) WR32(a + i, 0);
}

void fpu_ldenv(CPU *cpu, uint32_t a) {
    cpu->fcw = RD16(a);
    uint16_t sw = RD16(a + 4);
    cpu->fsw = sw & ~0x3800u;
    cpu->ftop = (sw >> 11) & 7;
}

void fpu_fnsave(CPU *cpu, uint32_t a) {
    fpu_stenv(cpu, a);
    for (int i = 0; i < 8; i++) fpu_st80(a + 28 + i * 10, cpu->fpr[(cpu->ftop + i) & 7]);
    fpu_init(cpu);
}

void fpu_frstor(CPU *cpu, uint32_t a) {
    fpu_ldenv(cpu, a);
    for (int i = 0; i < 8; i++) cpu->fpr[(cpu->ftop + i) & 7] = fpu_ld80(a + 28 + i * 10);
}

/* ---- CRT overrides ------------------------------------------------------------------- */
/* void *memmove(void *dst, const void *src, size_t n) -- cdecl */
void host_memmove(CPU *cpu) {
    uint32_t dst = ARG(0), src = ARG(1), n = ARG(2);
    memmove(GPTR(dst), GPTR(src), n);
    RET_CDECL(dst);
}

void rt_probe(uint32_t addr, uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx, uint32_t esp, uint32_t ebp,
              uint32_t esi, uint32_t edi) {
    /* logs when eax/ecx/edx change (the interesting values), up to 60 times per address */
    static struct { uint32_t a; int n; uint32_t k; uint32_t calls; } hits[256];
    static int every = -1;
    if (every < 0) every = getenv("PORT_PROBE_EVERY") ? atoi(getenv("PORT_PROBE_EVERY")) : 0;
    int i = 0;
    while (i < 256 && hits[i].a && hits[i].a != addr) i++;
    if (i == 256) return;
    hits[i].a = addr;
    if (every > 1 && hits[i].calls++ % (uint32_t)every) return;
    uint32_t k = getenv("PORT_PROBE_ALL") ? (uint32_t)hits[i].n : eax * 2654435761u ^ ecx * 40503u ^ edx;
    if (hits[i].n && hits[i].k == k) return;
    hits[i].k = k;
    if (hits[i].n++ >= 60) return;
    rt_log("probe %x: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x", addr, eax, ecx, edx,
           ebx, esp, ebp, esi, edi);
}

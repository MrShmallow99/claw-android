#define _GNU_SOURCE
/* Entry point: load the game exe into guest memory and run the recompiled code. */
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "platform.h"
#include "win32/win32.h"
#include "image_info.h"
#include "pack/vfs.h"
#include "gamecfg.h"
#include <signal.h>
#include <unistd.h>
#ifndef __ANDROID__
#include <execinfo.h>
#endif

#if !defined(__ANDROID__)
/* PORT_PROF=1: samples the host PC 1000x per second of CPU time; at exit the hottest
 * symbols go to the log (desktop only; perf may be unavailable to normal users) */
#include <dlfcn.h>
#include <sys/time.h>
#include <ucontext.h>
#define PROF_MAX (1 << 20)
static uintptr_t *g_prof;
static volatile int g_prof_n;
static void on_prof(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    if (g_prof_n < PROF_MAX) g_prof[g_prof_n++] = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
}
static int prof_cmp(const void *a, const void *b) {
    const uintptr_t *x = a, *y = b;
    return x[1] < y[1] ? 1 : x[1] > y[1] ? -1 : 0;
}
static void prof_report(void) {
    struct itimerval off = {0};
    setitimer(ITIMER_PROF, &off, NULL);
    int n = g_prof_n;
    /* histogram by symbol start address */
    uintptr_t (*h)[2] = calloc(65536, sizeof *h);
    const char **names = calloc(65536, sizeof *names);
    int hn = 0;
    for (int i = 0; i < n; i++) {
        Dl_info di;
        uintptr_t key = dladdr((void *)g_prof[i], &di) && di.dli_saddr ? (uintptr_t)di.dli_saddr : 0;
        int k = 0;
        while (k < hn && h[k][0] != key) k++;
        if (k == hn && hn < 65535) { h[hn][0] = key; names[hn] = key ? di.dli_sname : "?"; hn++; }
        h[k][1]++;
    }
    /* sort index by count (names stay with their slot via a parallel copy) */
    for (int i = 0; i < hn; i++) h[i][0] = (uintptr_t)i; /* names[] is indexed by the original slot */
    qsort(h, (size_t)hn, sizeof *h, prof_cmp);
    rt_log("profile: %d samples", n);
    /* unresolved PCs (static functions): module-relative, for addr2line */
    FILE *raw = fopen("/tmp/port_prof_raw.txt", "w");
    for (int i = 0; raw && i < n; i++) {
        Dl_info di;
        if (dladdr((void *)g_prof[i], &di) && !di.dli_saddr && di.dli_fname)
            fprintf(raw, "%s %lx\n", di.dli_fname, (unsigned long)(g_prof[i] - (uintptr_t)di.dli_fbase));
    }
    if (raw) fclose(raw);
    for (int i = 0; i < hn && i < 40; i++)
        rt_log("  %5.1f%%  %s", 100.0 * h[i][1] / (n ? n : 1), names[h[i][0]] ? names[h[i][0]] : "?");
}
static void prof_start(void) {
    g_prof = malloc(sizeof(uintptr_t) * PROF_MAX);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = {{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &it, NULL);
    atexit(prof_report);
}
#endif

void funcs_init(void);
void cpu_set_current(CPU *c);
extern FILE *g_logfile;
int g_trace_api;

/* IMAGE_BASE, IMAGE_SIZE, ENTRY_POINT, EXPECTED_SIZE and EXPECTED_TEXT_HASH come
 * from gen/image_info.h, written by recomp/codegen.py for the exact exe. */

static uint8_t *read_game_file(const char *rel, size_t *len) {
    VFile *f = vfs_open(rel);
    if (!f) return NULL;
    int64_t n = vfs_size(f);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (!b || vfs_read(f, b, (size_t)n) != (size_t)n) { free(b); vfs_close(f); return NULL; }
    vfs_close(f);
    *len = (size_t)n;
    return b;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* FNV-1a over the code section: the recompiled code only matches this exact build. */
static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void load_image(void) {
    const char *path = cfg_str("game", "exe", "game.exe");
    size_t len = 0;
    uint8_t *f = read_game_file(path, &len);
    if (!f) rt_fatal("%s not found in %s", path, platform_game_dir());
    if (len != EXPECTED_SIZE) rt_fatal("%s is not the expected version (size %zu, want %u)", path, len, EXPECTED_SIZE);
    uint32_t pe = rd32(f + 0x3c);
    if (rd32(f + pe) != 0x4550) rt_fatal("%s: bad PE header", path);
    uint16_t nsec = rd16(f + pe + 6);
    uint16_t optsz = rd16(f + pe + 20);
    const uint8_t *sec = f + pe + 24 + optsz;
    memcpy(GPTR(IMAGE_BASE), f, 0x1000);
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsize = rd32(sec + 8), va = rd32(sec + 12), rsize = rd32(sec + 16), roff = rd32(sec + 20);
        uint32_t n = rsize < vsize || vsize == 0 ? rsize : vsize;
        memcpy(GPTR(IMAGE_BASE + va), f + roff, n);
        if (!memcmp(sec, ".text", 5)) {
            uint32_t h = fnv1a(f + roff, n);
            rt_log("%s .text hash %08x", path, h);
            if (h != EXPECTED_TEXT_HASH) rt_fatal("%s: code does not match the version this port was built from", path);
        }
    }
    free(f);
    /* Code patches the game's startup scripts apply (the recompiled code already has them). */
    for (int i = 0; i < g_image_patch_count; i++)
        memcpy(GPTR(g_image_patches[i].addr), g_image_patches[i].bytes, (size_t)g_image_patches[i].len);
    if (g_image_patch_count) rt_log("applied %d startup code patches", g_image_patch_count);
    /* Fill the import address table with host thunks. */
    for (int i = 0; i < g_import_count; i++)
        WR32(g_import_table[i].iat, thunk_register(g_import_table[i].fn, g_import_table[i].name));
    rt_log("loaded %s", path);
}

/* Debug options can also come from <save dir>/env.txt (one NAME=VALUE per line;
 * "$SAVE" in a value expands to the save directory).  Handy on Android. */
static void load_env_file(void) {
    char path[1200], line[1024];
    snprintf(path, sizeof path, "%s/env.txt", platform_save_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char val[1200];
        if (!strncmp(eq + 1, "$SAVE", 5)) snprintf(val, sizeof val, "%s%s", platform_save_dir(), eq + 6);
        else snprintf(val, sizeof val, "%s", eq + 1);
        setenv(line, val, 1);
    }
    fclose(f);
}

/* A crash in recompiled code is usually a bad guest address: report it as one. */
/* (no rt_log here: the fault may have happened inside it, holding its lock) */
static void fault_write(const char *s) {
    if (write(2, s, strlen(s)) < 0) {}
    if (g_logfile && write(fileno(g_logfile), s, strlen(s)) < 0) {}
}
static void on_fault(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uint8_t *a = si->si_addr;
    char line[160];
    if (g_mem && a >= g_mem && a < g_mem + 0x100000000ull)
        snprintf(line, sizeof line, "FAULT: signal %d at guest address %08x\n", sig, (uint32_t)(a - g_mem));
    else
        snprintf(line, sizeof line, "FAULT: signal %d at host address %p\n", sig, (void *)a);
    fault_write(line);
#ifndef __ANDROID__
    void *bt[32];
    int n = backtrace(bt, 32);
    if (g_logfile) backtrace_symbols_fd(bt, n, fileno(g_logfile));
    backtrace_symbols_fd(bt, n, 2);
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(int argc, char **argv) {
    if (platform_init(argc, argv)) return 1;
    load_env_file();
    char logpath[1200];
    snprintf(logpath, sizeof logpath, "%s/port.log", platform_save_dir());
    g_logfile = fopen(logpath, "w");
    g_trace_api = getenv("PORT_TRACE") != NULL;
    rt_log("%s - recompiled port", cfg_str("game", "title", "game"));
    rt_log("game dir: %s  save dir: %s", platform_game_dir(), platform_save_dir());
    win32_paths_init();
#if !defined(__ANDROID__)
    if (getenv("PORT_PROF")) prof_start();
#endif
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    mem_init();
    funcs_init();
    load_image();
    CPU *cpu = cpu_new_thread(0x100000);
    cpu_set_current(cpu);
    uint32_t r = guest_call(cpu, ENTRY_POINT, 0, NULL);
    rt_log("guest entry returned %u", r);
    platform_shutdown();
    return (int)r;
}

/* Debug sampling profiler (desktop Linux only): PORT_PROFILE=<file> records the
 * host program counter 1000x/s and writes raw samples at exit; resolve them with
 * tools/profile_report.py. */
#if defined(__linux__) && !defined(__ANDROID__) && defined(__x86_64__)
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <dlfcn.h>

#define MAX_SAMPLES (1 << 20)
static void *g_samples[MAX_SAMPLES];
static volatile int g_nsamples;
static const char *g_out;

static void on_prof(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    ucontext_t *uc = uc_;
    int n = g_nsamples;
    if (n < MAX_SAMPLES) {
        g_samples[n] = (void *)uc->uc_mcontext.gregs[REG_RIP];
        g_nsamples = n + 1;
    }
}

static void dump(void) {
    FILE *f = fopen(g_out, "w");
    if (!f) return;
    Dl_info di;
    void *base = dladdr((void *)dump, &di) ? di.dli_fbase : 0;
    fprintf(f, "base %p\n", base);
    for (int i = 0; i < g_nsamples; i++) fprintf(f, "%lx\n", (unsigned long)((char *)g_samples[i] - (char *)base));
    fclose(f);
}

__attribute__((constructor)) static void profiler_init(void) {
    g_out = getenv("PORT_PROFILE");
    if (!g_out) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = {{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &it, NULL);
    atexit(dump);
    signal(SIGTERM, exit);
}
#endif

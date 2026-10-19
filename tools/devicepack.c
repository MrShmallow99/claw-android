/* devicepack <game dir> [threads] -- runs the on-device packer (runtime/pack/packer.c)
 * on the desktop: writes <game dir>/game.pak with the compiled-in game.ini's [pack]
 * rules and verifies it, without deleting anything.  Use a copy of the game folder. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "gamecfg.h"

int packer_run(const char *game_dir, int threads, int delete_sources,
               void (*cb)(void *, int, int64_t, int64_t), void *ctx, char *err, size_t errn);
static char g_game[1024] = "/nonexistent", g_save[1024] = "/nonexistent";
const char *platform_game_dir(void) { return g_game; }
const char *platform_save_dir(void) { return g_save; }
void platform_set_dirs(const char *game, const char *save) {
    snprintf(g_game, sizeof g_game, "%s", game);
    snprintf(g_save, sizeof g_save, "%s", save);
}
void rt_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}
static void progress(void *ctx, int phase, int64_t done, int64_t total) {
    (void)ctx;
    static const char *names[] = {"scanning", "compressing", "verifying", "cleaning up"};
    fprintf(stderr, "\r%-12s %6.1f / %6.1f MB   ", names[phase], done / 1e6, total / 1e6);
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: devicepack <game dir> [threads]\n"); return 2; }
    cfg_init();
    char err[512];
    int r = packer_run(argv[1], argc > 2 ? atoi(argv[2]) : 0, 0, progress, NULL, err, sizeof err);
    fprintf(stderr, "\n%s\n", r ? err : "ok: game.pak written and verified");
    return r ? 1 : 0;
}

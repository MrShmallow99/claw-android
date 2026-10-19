/* packverify game.pak name=original_path ...    or    packverify game.pak @list (name<TAB>path lines)
 * Reads each entry back through the runtime's VFS and compares it with the
 * original file byte for byte (sequential reads of varying sizes plus random seeks).
 * Build: gcc -O2 -I runtime -o packverify tools/packverify.c runtime/pack/vfs.c runtime/pack/edlrec.c \
 *        runtime/pack/drflac_impl.c third_party/lzma/LzmaDec.c -lpthread */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../runtime/pack/vfs.h"

const char *platform_save_dir(void) { return "/nonexistent-save"; }
const char *platform_game_dir(void) { return "/nonexistent-game"; }
void rt_log(const char *fmt, ...) { (void)fmt; }

static int verbose = 1;

int main(int argc, char **argv) {
    if (vfs_pack_open(argv[1], 0)) { fprintf(stderr, "cannot open pack\n"); return 1; }
    int bad = 0, total = 0;
    srand(1);
    char **items = argv + 2;
    int nitems = argc - 2;
    if (argc == 3 && argv[2][0] == '@') { /* list file: name<TAB>path per line */
        FILE *lf = fopen(argv[2] + 1, "r");
        char line[4096];
        nitems = 0;
        items = NULL;
        while (lf && fgets(line, sizeof line, lf)) {
            line[strcspn(line, "\r\n")] = 0;
            char *t = strchr(line, '\t');
            if (!t) continue;
            *t = '=';
            items = realloc(items, sizeof(char *) * (size_t)(nitems + 1));
            items[nitems++] = strdup(line);
        }
        verbose = 0;
    }
    for (int i = 0; i < nitems; i++) {
        char *eq = strchr(items[i], '=');
        *eq = 0;
        const char *name = items[i], *orig = eq + 1;
        total++;
        FILE *f = fopen(orig, "rb");
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *a = malloc(n + 1), *b = malloc(n + 1);
        if (fread(a, 1, n, f) != (size_t)n) return 1;
        fclose(f);
        VFile *v = vfs_open(name);
        int ok = v && vfs_size(v) == n;
        if (ok) {
            long p = 0;
            while (p < n) {
                size_t want = (size_t)(rand() % 70000) + 1;
                size_t got = vfs_read(v, b + p, want);
                if (!got) break;
                p += (long)got;
            }
            ok = p == n && !memcmp(a, b, n);
            /* random seeks (forward and backward) */
            for (int k = 0; ok && k < 20 && n > 0; k++) {
                long o = rand() % n, len = rand() % 5000 + 1;
                if (o + len > n) len = n - o;
                vfs_seek(v, o, SEEK_SET);
                ok = vfs_read(v, b, (size_t)len) == (size_t)len && !memcmp(a + o, b, (size_t)len);
            }
        }
        if (verbose || !ok) printf("%-24s %10ld %s\n", name, n, ok ? "identical" : "MISMATCH");
        bad += !ok;
        vfs_close(v);
        free(a);
        free(b);
    }
    printf("%d files checked: ", total);
    printf(bad ? "%d MISMATCHES\n" : "all files identical\n", bad);
    return bad != 0;
}

/* KERNEL32 emulation, part 2: the APIs MFC-based games add on top of the CRT set
 * (find files, Global/Local memory, atoms, profile strings, time, events, lstr*). */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "win32.h"
#include "gamecfg.h"
#include "../pack/vfs.h"

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_NO_MORE_FILES 18
#define INVALID_HANDLE_VALUE 0xffffffffu

/* ---- strings --------------------------------------------------------------------- */
void imp_KERNEL32_lstrlenA(CPU *cpu) { RET_STDCALL(1, ARG(0) ? strlen(gstr(ARG(0))) : 0); }
void imp_KERNEL32_lstrcpyA(CPU *cpu) {
    strcpy(gstr(ARG(0)), ARG(1) ? gstr(ARG(1)) : "");
    RET_STDCALL(2, ARG(0));
}
void imp_KERNEL32_lstrcpynA(CPU *cpu) {
    int32_t n = (int32_t)ARG(2);
    char *d = gstr(ARG(0));
    const char *s = ARG(1) ? gstr(ARG(1)) : "";
    if (n > 0) {
        int i = 0;
        for (; i < n - 1 && s[i]; i++) d[i] = s[i];
        d[i] = 0;
    }
    RET_STDCALL(3, ARG(0));
}
void imp_KERNEL32_lstrcatA(CPU *cpu) {
    strcat(gstr(ARG(0)), ARG(1) ? gstr(ARG(1)) : "");
    RET_STDCALL(2, ARG(0));
}
/* lstrcmp uses word sort order; for the ASCII names games compare this is close enough */
void imp_KERNEL32_lstrcmpA(CPU *cpu) {
    int r = strcmp(ARG(0) ? gstr(ARG(0)) : "", ARG(1) ? gstr(ARG(1)) : "");
    RET_STDCALL(2, (uint32_t)(r < 0 ? -1 : r > 0));
}
void imp_KERNEL32_lstrcmpiA(CPU *cpu) {
    int r = strcasecmp(ARG(0) ? gstr(ARG(0)) : "", ARG(1) ? gstr(ARG(1)) : "");
    RET_STDCALL(2, (uint32_t)(r < 0 ? -1 : r > 0));
}
/* CompareStringA(lcid, flags, s1, n1, s2, n2): 1 less, 2 equal, 3 greater */
void imp_KERNEL32_CompareStringA(CPU *cpu) {
    uint32_t flags = ARG(1);
    const char *a = gstr(ARG(2)), *b = gstr(ARG(4));
    int32_t na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    if (na < 0) na = (int32_t)strlen(a);
    if (nb < 0) nb = (int32_t)strlen(b);
    int n = na < nb ? na : nb, r = 0;
    for (int i = 0; i < n && !r; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (flags & 1) { x = tolower(x); y = tolower(y); } /* NORM_IGNORECASE */
        r = x - y;
    }
    if (!r) r = na - nb;
    RET_STDCALL(6, r < 0 ? 1 : r > 0 ? 3 : 2);
}
void imp_KERNEL32_CompareStringW(CPU *cpu) {
    uint32_t flags = ARG(1);
    uint32_t a = ARG(2), b = ARG(4);
    int32_t na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    if (na < 0) for (na = 0; RD16(a + 2 * na); na++) {}
    if (nb < 0) for (nb = 0; RD16(b + 2 * nb); nb++) {}
    int n = na < nb ? na : nb, r = 0;
    for (int i = 0; i < n && !r; i++) {
        int x = RD16(a + 2 * i), y = RD16(b + 2 * i);
        if ((flags & 1) && x < 128 && y < 128) { x = tolower(x); y = tolower(y); }
        r = x - y;
    }
    if (!r) r = na - nb;
    RET_STDCALL(6, r < 0 ? 1 : r > 0 ? 3 : 2);
}

void imp_KERNEL32_MulDiv(CPU *cpu) {
    int64_t a = (int32_t)ARG(0), b = (int32_t)ARG(1), c = (int32_t)ARG(2);
    if (!c) RET_STDCALL(3, 0xffffffffu);
    int64_t p = a * b;
    /* rounds half away from zero, like Windows */
    int64_t r = (p >= 0) == (c >= 0) ? (p + (c >= 0 ? c : -c) / 2 * (c >= 0 ? 1 : -1)) / c
                                     : (p - (c >= 0 ? c : -c) / 2 * (c >= 0 ? 1 : -1)) / c;
    RET_STDCALL(3, (uint32_t)(int32_t)r);
}

void imp_KERNEL32_FormatMessageA(CPU *cpu) {
    uint32_t buf = ARG(4), size = ARG(5), flags = ARG(0);
    const char *msg = "Error.\r\n";
    uint32_t n = (uint32_t)strlen(msg);
    if (flags & 0x100) { /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        uint32_t p = heap_alloc(n + 1, 0);
        memcpy(GPTR(p), msg, n + 1);
        WR32(buf, p);
    } else if (buf && size > n) {
        memcpy(GPTR(buf), msg, n + 1);
    }
    RET_STDCALL(7, n);
}

void imp_KERNEL32_OutputDebugStringA(CPU *cpu) {
    if (ARG(0)) rt_log("OutputDebugString: %s", gstr(ARG(0)));
    RET_STDCALL(1, 0);
}

/* ---- Global/Local memory: fixed blocks; the handle is the pointer --------------- */
void imp_KERNEL32_GlobalAlloc(CPU *cpu) { RET_STDCALL(2, heap_alloc(ARG(1) ? ARG(1) : 1, (ARG(0) & 0x40) != 0)); }
void imp_KERNEL32_GlobalFree(CPU *cpu) { heap_free(ARG(0)); RET_STDCALL(1, 0); }
void imp_KERNEL32_GlobalLock(CPU *cpu) { RET_STDCALL(1, ARG(0)); }
void imp_KERNEL32_GlobalUnlock(CPU *cpu) { RET_STDCALL(1, 0); }
void imp_KERNEL32_GlobalHandle(CPU *cpu) { RET_STDCALL(1, ARG(0)); }
void imp_KERNEL32_GlobalFlags(CPU *cpu) { RET_STDCALL(1, 0); }
void imp_KERNEL32_GlobalReAlloc(CPU *cpu) {
    uint32_t p = ARG(0), n = ARG(1), flags = ARG(2);
    uint32_t old = heap_size(p);
    uint32_t r = heap_realloc(p, n ? n : 1);
    if (r && (flags & 0x40) && n > old) memset(GPTR(r + old), 0, n - old);
    RET_STDCALL(3, r);
}
void imp_KERNEL32_LocalAlloc(CPU *cpu) { RET_STDCALL(2, heap_alloc(ARG(1) ? ARG(1) : 1, (ARG(0) & 0x40) != 0)); }
void imp_KERNEL32_LocalFree(CPU *cpu) { heap_free(ARG(0)); RET_STDCALL(1, 0); }
void imp_KERNEL32_LocalReAlloc(CPU *cpu) {
    uint32_t p = ARG(0), n = ARG(1), flags = ARG(2);
    uint32_t old = heap_size(p);
    uint32_t r = heap_realloc(p, n ? n : 1);
    if (r && (flags & 0x40) && n > old) memset(GPTR(r + old), 0, n - old);
    RET_STDCALL(3, r);
}
void imp_KERNEL32_HeapSize(CPU *cpu) { RET_STDCALL(3, heap_size(ARG(2))); }

/* the guest address space is all readable; only null-page pointers are "bad" */
void imp_KERNEL32_IsBadReadPtr(CPU *cpu) { RET_STDCALL(2, ARG(0) < 0x10000 && ARG(1)); }
void imp_KERNEL32_IsBadWritePtr(CPU *cpu) { RET_STDCALL(2, ARG(0) < 0x10000 && ARG(1)); }
void imp_KERNEL32_IsBadCodePtr(CPU *cpu) { RET_STDCALL(1, lookup_guest_fn(ARG(0)) == NULL); }

/* ---- atoms ------------------------------------------------------------------------ */
#define MAX_ATOMS 512
static char *g_atoms[MAX_ATOMS];
static int g_atom_refs[MAX_ATOMS];
static pthread_mutex_t g_atom_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t atom_find(const char *s) {
    for (int i = 0; i < MAX_ATOMS; i++)
        if (g_atoms[i] && !strcasecmp(g_atoms[i], s)) return 0xc000u + (uint32_t)i;
    return 0;
}

uint32_t win32_atom_add(const char *s) {
    pthread_mutex_lock(&g_atom_lock);
    uint32_t a = atom_find(s);
    if (a) {
        g_atom_refs[a - 0xc000]++;
    } else {
        for (int i = 0; i < MAX_ATOMS; i++)
            if (!g_atoms[i]) {
                g_atoms[i] = strdup(s);
                g_atom_refs[i] = 1;
                a = 0xc000u + (uint32_t)i;
                break;
            }
    }
    pthread_mutex_unlock(&g_atom_lock);
    return a;
}

const char *win32_atom_name(uint32_t a) {
    if (a < 0xc000 || a >= 0xc000 + MAX_ATOMS) return NULL;
    return g_atoms[a - 0xc000];
}

void imp_KERNEL32_GlobalAddAtomA(CPU *cpu) {
    uint32_t s = ARG(0);
    RET_STDCALL(1, s < 0x10000 ? s : win32_atom_add(gstr(s)));
}
void imp_KERNEL32_GlobalFindAtomA(CPU *cpu) {
    uint32_t s = ARG(0);
    pthread_mutex_lock(&g_atom_lock);
    uint32_t a = s < 0x10000 ? s : atom_find(gstr(s));
    pthread_mutex_unlock(&g_atom_lock);
    RET_STDCALL(1, a);
}
void imp_KERNEL32_GlobalDeleteAtom(CPU *cpu) {
    uint32_t a = ARG(0);
    pthread_mutex_lock(&g_atom_lock);
    if (a >= 0xc000 && a < 0xc000 + MAX_ATOMS && g_atoms[a - 0xc000] && --g_atom_refs[a - 0xc000] <= 0) {
        free(g_atoms[a - 0xc000]);
        g_atoms[a - 0xc000] = NULL;
    }
    pthread_mutex_unlock(&g_atom_lock);
    RET_STDCALL(1, 0);
}
void imp_KERNEL32_GlobalGetAtomNameA(CPU *cpu) {
    const char *n = win32_atom_name(ARG(0));
    uint32_t buf = ARG(1), size = ARG(2);
    if (!n || !size) RET_STDCALL(3, 0);
    size_t l = strlen(n);
    if (l >= size) l = size - 1;
    memcpy(GPTR(buf), n, l);
    WR8(buf + l, 0);
    RET_STDCALL(3, l);
}

/* ---- events / waits ----------------------------------------------------------------- */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int manual, set;
} Event;

void imp_KERNEL32_CreateEventA(CPU *cpu) {
    uint32_t h = handle_new(H_EVENT);
    Event *e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->manual = ARG(1) != 0;
    e->set = ARG(2) != 0;
    handle_get(h, H_EVENT)->ptr = e;
    RET_STDCALL(4, h);
}
void imp_KERNEL32_SetEvent(CPU *cpu) {
    Handle *hd = handle_get(ARG(0), H_EVENT);
    if (hd) {
        Event *e = hd->ptr;
        pthread_mutex_lock(&e->m);
        e->set = 1;
        pthread_cond_broadcast(&e->c);
        pthread_mutex_unlock(&e->m);
    }
    RET_STDCALL(1, hd != NULL);
}
/* WaitForSingleObject(h, ms): events block; other objects count as signalled */
void imp_KERNEL32_WaitForSingleObject(CPU *cpu) {
    Handle *hd = handle_get(ARG(0), H_EVENT);
    uint32_t ms = ARG(1);
    if (!hd) RET_STDCALL(2, 0);
    Event *e = hd->ptr;
    uint32_t r = 0;
    pthread_mutex_lock(&e->m);
    if (!e->set && ms) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ns = (uint64_t)ts.tv_nsec + (uint64_t)(ms == 0xffffffffu ? 3600000u : ms) * 1000000ull;
        ts.tv_sec += (time_t)(ns / 1000000000ull);
        ts.tv_nsec = (long)(ns % 1000000000ull);
        while (!e->set)
            if (pthread_cond_timedwait(&e->c, &e->m, &ts)) break;
    }
    if (e->set) {
        if (!e->manual) e->set = 0;
    } else {
        r = 0x102; /* WAIT_TIMEOUT */
    }
    pthread_mutex_unlock(&e->m);
    RET_STDCALL(2, r);
}
void imp_KERNEL32_DuplicateHandle(CPU *cpu) {
    if (ARG(3)) WR32(ARG(3), ARG(1));
    RET_STDCALL(7, 1);
}
void imp_KERNEL32_OpenProcess(CPU *cpu) { RET_STDCALL(3, 0); }
void imp_KERNEL32_CreateProcessA(CPU *cpu) {
    rt_log("CreateProcessA(%s, %s) refused", ARG(0) ? gstr(ARG(0)) : "", ARG(1) ? gstr(ARG(1)) : "");
    RET_STDCALL(10, 0);
}
void imp_KERNEL32_WinExec(CPU *cpu) {
    rt_log("WinExec(%s) refused", gstr(ARG(0)));
    RET_STDCALL(2, 2); /* ERROR_FILE_NOT_FOUND */
}
void imp_KERNEL32_GetProcessVersion(CPU *cpu) { RET_STDCALL(1, 0x0004000a); }
void imp_KERNEL32_SetErrorMode(CPU *cpu) {
    static uint32_t mode;
    uint32_t old = mode;
    mode = ARG(0);
    RET_STDCALL(1, old);
}
void imp_KERNEL32_SetUnhandledExceptionFilter(CPU *cpu) { RET_STDCALL(1, 0); }

void imp_KERNEL32_ExitThread(CPU *cpu) {
    rt_log("ExitThread(%u)", ARG(0));
    pthread_exit(NULL);
}

/* ---- time ----------------------------------------------------------------------------- */
void imp_KERNEL32_GetTickCount(CPU *cpu) { RET_STDCALL(0, platform_ticks_ms()); }

static void write_systemtime(uint32_t p, const struct tm *t, int ms) {
    WR16(p + 0, t->tm_year + 1900);
    WR16(p + 2, t->tm_mon + 1);
    WR16(p + 4, t->tm_wday);
    WR16(p + 6, t->tm_mday);
    WR16(p + 8, t->tm_hour);
    WR16(p + 10, t->tm_min);
    WR16(p + 12, t->tm_sec);
    WR16(p + 14, ms);
}
void imp_KERNEL32_GetLocalTime(CPU *cpu) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t s = tv.tv_sec;
    struct tm t;
    localtime_r(&s, &t);
    write_systemtime(ARG(0), &t, (int)(tv.tv_usec / 1000));
    RET_STDCALL(1, 0);
}
void imp_KERNEL32_GetSystemTime(CPU *cpu) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t s = tv.tv_sec;
    struct tm t;
    gmtime_r(&s, &t);
    write_systemtime(ARG(0), &t, (int)(tv.tv_usec / 1000));
    RET_STDCALL(1, 0);
}
void imp_KERNEL32_GetTimeZoneInformation(CPU *cpu) {
    memset(GPTR(ARG(0)), 0, 172);
    RET_STDCALL(1, 0); /* TIME_ZONE_ID_UNKNOWN */
}
/* FILETIME: 100 ns units since 1601 */
#define FT_EPOCH 116444736000000000ull
void imp_KERNEL32_FileTimeToLocalFileTime(CPU *cpu) {
    WR64(ARG(1), RD64(ARG(0)));
    RET_STDCALL(2, 1);
}
void imp_KERNEL32_FileTimeToSystemTime(CPU *cpu) {
    uint64_t ft = RD64(ARG(0));
    time_t s = ft >= FT_EPOCH ? (time_t)((ft - FT_EPOCH) / 10000000ull) : 0;
    struct tm t;
    gmtime_r(&s, &t);
    write_systemtime(ARG(1), &t, (int)((ft / 10000) % 1000));
    RET_STDCALL(2, 1);
}

/* ---- files ------------------------------------------------------------------------------ */
void imp_KERNEL32_GetFileSize(CPU *cpu) {
    Handle *hd = handle_get(ARG(0), H_FILE);
    if (!hd || !hd->vf) RET_STDCALL(2, 0xffffffffu);
    int64_t n = vfs_size(hd->vf);
    if (ARG(1)) WR32(ARG(1), (uint32_t)((uint64_t)n >> 32));
    RET_STDCALL(2, (uint32_t)n);
}
void imp_KERNEL32_GetFileTime(CPU *cpu) {
    /* fixed time: 1998-01-01 */
    uint64_t ft = FT_EPOCH + 883612800ull * 10000000ull;
    for (int i = 1; i <= 3; i++)
        if (ARG(i)) WR64(ARG(i), ft);
    RET_STDCALL(4, 1);
}
void imp_KERNEL32_LockFile(CPU *cpu) { RET_STDCALL(5, 1); }
void imp_KERNEL32_UnlockFile(CPU *cpu) { RET_STDCALL(5, 1); }

void imp_KERNEL32_GetCurrentDirectoryA(CPU *cpu) {
    const char *dir = cfg_str("game", "guest_dir", "C:\\GAME");
    uint32_t size = ARG(0), buf = ARG(1);
    uint32_t n = (uint32_t)strlen(dir);
    if (size > n) memcpy(GPTR(buf), dir, n + 1);
    RET_STDCALL(2, size > n ? n : n + 1);
}
void imp_KERNEL32_GetWindowsDirectoryA(CPU *cpu) {
    const char *dir = "C:\\WINDOWS";
    uint32_t n = (uint32_t)strlen(dir);
    if (ARG(1) > n) memcpy(GPTR(ARG(0)), dir, n + 1);
    RET_STDCALL(2, n);
}
void imp_KERNEL32_GetVolumeInformationA(CPU *cpu) {
    if (ARG(1) && ARG(2)) { strncpy(gstr(ARG(1)), "CLAW", ARG(2)); }
    if (ARG(3)) WR32(ARG(3), 0x19971997u);
    if (ARG(4)) WR32(ARG(4), 255);
    if (ARG(5)) WR32(ARG(5), 0x3);
    if (ARG(6) && ARG(7)) strncpy(gstr(ARG(6)), "FAT32", ARG(7));
    RET_STDCALL(8, 1);
}
/* GetFullPathNameA(name, size, buf, &filepart) */
void imp_KERNEL32_GetFullPathNameA(CPU *cpu) {
    const char *name = gstr(ARG(0));
    uint32_t size = ARG(1), buf = ARG(2), fp = ARG(3);
    char out[1024];
    if (isalpha((unsigned char)name[0]) && name[1] == ':') snprintf(out, sizeof out, "%s", name);
    else if (name[0] == '\\') snprintf(out, sizeof out, "C:%s", name);
    else snprintf(out, sizeof out, "%s\\%s", cfg_str("game", "guest_dir", "C:\\GAME"), name);
    for (char *c = out; *c; c++) if (*c == '/') *c = '\\';
    uint32_t n = (uint32_t)strlen(out);
    if (size <= n) RET_STDCALL(4, n + 1);
    memcpy(GPTR(buf), out, n + 1);
    if (fp) {
        char *last = strrchr(out, '\\');
        WR32(fp, buf + (uint32_t)(last ? last + 1 - out : 0));
    }
    RET_STDCALL(4, n);
}

/* OpenFile(name, OFSTRUCT*, style): only OF_EXIST / plain open are used by old code */
void imp_KERNEL32_OpenFile(CPU *cpu) {
    const char *name = gstr(ARG(0));
    uint32_t of = ARG(1), style = ARG(2);
    char rel[1024];
    guest_rel_path(name, rel, sizeof rel);
    int is_dir = 0;
    int ok = vfs_stat(rel, &is_dir, NULL) == 0 && !is_dir;
    if (!ok && (style & 0x4000)) { /* OF_EXIST: the DVD movies may be present as .avi (avi.c) */
        int avi_file_exists(const char *guest);
        ok = avi_file_exists(name);
    }
    if (of) {
        WR8(of, 136);
        WR8(of + 1, 1);
        WR16(of + 2, ok ? 0 : ERROR_FILE_NOT_FOUND);
        strncpy(gstr(of + 8), name, 127);
    }
    if (!ok) RET_STDCALL(3, 0xffffffffu);
    if (style & 0x4000) RET_STDCALL(3, 1); /* OF_EXIST */
    uint32_t h = handle_new(H_FILE);
    handle_get(h, H_FILE)->vf = vfs_open(rel);
    RET_STDCALL(3, h);
}

/* ---- FindFirstFile / FindNextFile over the merged VFS listing ----------------------------- */
typedef struct {
    char **names;
    int *dirs;
    int n, cap, pos;
    char dir[1024];
} FindState;

static int wild_match(const char *pat, const char *s) {
    if (!*pat) return !*s;
    if (*pat == '*') {
        for (;; s++) {
            if (wild_match(pat + 1, s)) return 1;
            if (!*s) return 0;
        }
    }
    if (!*s) return !strcmp(pat, ".") || !strcmp(pat, "*");
    if (*pat == '?' || tolower((unsigned char)*pat) == tolower((unsigned char)*s)) return wild_match(pat + 1, s + 1);
    /* "*.*" matches names without a dot too */
    return 0;
}

typedef struct { FindState *fs; const char *pat; } FindCtx;
static void find_add(void *ctx, const char *name, int is_dir) {
    FindCtx *c = ctx;
    const char *pat = c->pat;
    int ok = wild_match(pat, name);
    if (!ok && !strcmp(pat, "*.*")) ok = 1;
    if (!ok) return;
    FindState *fs = c->fs;
    if (fs->n == fs->cap) {
        fs->cap = fs->cap ? fs->cap * 2 : 32;
        fs->names = realloc(fs->names, fs->cap * sizeof *fs->names);
        fs->dirs = realloc(fs->dirs, fs->cap * sizeof *fs->dirs);
    }
    fs->names[fs->n] = strdup(name);
    fs->dirs[fs->n++] = is_dir;
}

static void write_find_data(uint32_t fd, FindState *fs, int i) {
    memset(GPTR(fd), 0, 320);
    WR32(fd, fs->dirs[i] ? 0x10 : 0x20);
    uint64_t ft = FT_EPOCH + 883612800ull * 10000000ull;
    WR64(fd + 4, ft); WR64(fd + 12, ft); WR64(fd + 20, ft);
    if (!fs->dirs[i]) {
        char rel[1200];
        int64_t sz = 0;
        snprintf(rel, sizeof rel, "%s%s%s", fs->dir, fs->dir[0] ? "/" : "", fs->names[i]);
        vfs_stat(rel, NULL, &sz);
        WR32(fd + 28, (uint32_t)((uint64_t)sz >> 32));
        WR32(fd + 32, (uint32_t)sz);
    }
    strncpy(gstr(fd + 44), fs->names[i], 259);
    /* 8.3 alternate name left empty: the long name is valid */
}

void imp_KERNEL32_FindFirstFileA(CPU *cpu) {
    const char *spec = gstr(ARG(0));
    uint32_t fd = ARG(1);
    char rel[1024];
    guest_rel_path(spec, rel, sizeof rel);
    FindState *fs = calloc(1, sizeof *fs);
    char *slash = strrchr(rel, '/');
    const char *pat = slash ? slash + 1 : rel;
    if (slash) {
        *slash = 0;
        snprintf(fs->dir, sizeof fs->dir, "%s", rel);
    }
    FindCtx c = {fs, pat};
    if (!strpbrk(pat, "*?")) {
        /* exact name: report it if it exists */
        char full[1200];
        int is_dir = 0;
        snprintf(full, sizeof full, "%s%s%s", fs->dir, fs->dir[0] ? "/" : "", pat);
        if (vfs_stat(full, &is_dir, NULL) == 0) find_add(&c, pat, is_dir);
    } else {
        vfs_list_dir(fs->dir, find_add, &c);
    }
    TRACE("FindFirstFileA(%s) -> %d entries", spec, fs->n);
    if (!fs->n) {
        free(fs);
        win32_set_last_error(ERROR_FILE_NOT_FOUND);
        RET_STDCALL(2, INVALID_HANDLE_VALUE);
    }
    uint32_t h = handle_new(H_FIND);
    handle_get(h, H_FIND)->ptr = fs;
    write_find_data(fd, fs, 0);
    fs->pos = 1;
    RET_STDCALL(2, h);
}

void imp_KERNEL32_FindNextFileA(CPU *cpu) {
    Handle *hd = handle_get(ARG(0), H_FIND);
    if (!hd) RET_STDCALL(2, 0);
    FindState *fs = hd->ptr;
    if (fs->pos >= fs->n) {
        win32_set_last_error(ERROR_NO_MORE_FILES);
        RET_STDCALL(2, 0);
    }
    write_find_data(ARG(1), fs, fs->pos++);
    RET_STDCALL(2, 1);
}

void imp_KERNEL32_FindClose(CPU *cpu) {
    Handle *hd = handle_get(ARG(0), H_FIND);
    if (hd) {
        FindState *fs = hd->ptr;
        for (int i = 0; i < fs->n; i++) free(fs->names[i]);
        free(fs->names);
        free(fs->dirs);
        free(fs);
        hd->ptr = NULL;
        hd->type = H_FREE;
    }
    RET_STDCALL(1, 1);
}

/* ---- profile (.ini) files: read through the VFS, written to the save overlay ------------ */
typedef struct { char *data; size_t len; } TextBuf;

static int ini_path_rel(uint32_t file, char *rel, size_t n) {
    const char *f = file ? gstr(file) : "win.ini";
    if (!strpbrk(f, "\\/")) {
        char tmp[600];
        snprintf(tmp, sizeof tmp, "%s\\%s", strcasecmp(f, "win.ini") ? cfg_str("game", "guest_dir", "C:\\GAME") : "C:\\WINDOWS", f);
        guest_rel_path(tmp, rel, n);
    } else {
        guest_rel_path(f, rel, n);
    }
    return 0;
}

static TextBuf ini_load(const char *rel) {
    TextBuf t = {0};
    VFile *vf = vfs_open(rel);
    if (!vf) return t;
    int64_t n = vfs_size(vf);
    t.data = malloc((size_t)n + 1);
    t.len = vfs_read(vf, t.data, (size_t)n);
    t.data[t.len] = 0;
    vfs_close(vf);
    return t;
}

/* finds key in section; returns value span, or the section span when key is NULL */
static int ini_find(const char *d, const char *sec, const char *key, const char **vs, const char **ve,
                    const char **sec_end) {
    const char *p = d;
    int in = 0;
    *sec_end = NULL;
    while (*p) {
        const char *ls = p, *le = p;
        while (*le && *le != '\n') le++;
        p = *le ? le + 1 : le;
        const char *a = ls;
        while (a < le && (*a == ' ' || *a == '\t')) a++;
        if (a < le && *a == '[') {
            if (in) { *sec_end = ls; return 0; }
            const char *b = memchr(a, ']', (size_t)(le - a));
            in = b && (size_t)(b - a - 1) == strlen(sec) && !strncasecmp(a + 1, sec, strlen(sec));
            if (in) *sec_end = p;
            continue;
        }
        if (!in || !key) { if (in) *sec_end = p; continue; }
        *sec_end = p;
        const char *eq = memchr(a, '=', (size_t)(le - a));
        if (!eq) continue;
        const char *ke = eq;
        while (ke > a && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        if ((size_t)(ke - a) == strlen(key) && !strncasecmp(a, key, strlen(key))) {
            const char *v = eq + 1;
            while (v < le && (*v == ' ' || *v == '\t')) v++;
            const char *e = le;
            while (e > v && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
            *vs = v;
            *ve = e;
            return 1;
        }
    }
    return in ? 0 : -1;
}

void imp_KERNEL32_GetPrivateProfileStringA(CPU *cpu) {
    uint32_t sec = ARG(0), key = ARG(1), def = ARG(2), buf = ARG(3), size = ARG(4), file = ARG(5);
    char rel[1024];
    ini_path_rel(file, rel, sizeof rel);
    TextBuf t = ini_load(rel);
    const char *vs = NULL, *ve = NULL, *se;
    char out[4096];
    size_t n = 0;
    if (t.data && sec && key && ini_find(t.data, gstr(sec), gstr(key), &vs, &ve, &se) == 1) {
        n = (size_t)(ve - vs);
        if (n >= sizeof out) n = sizeof out - 1;
        memcpy(out, vs, n);
    } else if (t.data && sec && !key) {
        /* list of keys in the section, each NUL-terminated */
        n = 0;
        const char *p = t.data;
        int in = 0;
        while (*p && n < sizeof out - 2) {
            const char *ls = p, *le = p;
            while (*le && *le != '\n') le++;
            p = *le ? le + 1 : le;
            if (*ls == '[') { in = !strncasecmp(ls + 1, gstr(sec), strlen(gstr(sec))) && ls[1 + strlen(gstr(sec))] == ']'; continue; }
            const char *eq = in ? memchr(ls, '=', (size_t)(le - ls)) : NULL;
            if (eq) {
                size_t kl = (size_t)(eq - ls);
                if (n + kl + 1 >= sizeof out - 1) break;
                memcpy(out + n, ls, kl);
                n += kl;
                out[n++] = 0;
            }
        }
        out[n] = 0;
        free(t.data);
        if (size) {
            if (n + 1 > size) n = size > 2 ? size - 2 : 0;
            memcpy(GPTR(buf), out, n);
            WR8(buf + n, 0);
            if (size > 1) WR8(buf + n + 1, 0);
        }
        RET_STDCALL(6, n);
    } else {
        const char *d = def ? gstr(def) : "";
        n = strlen(d);
        if (n >= sizeof out) n = sizeof out - 1;
        memcpy(out, d, n);
    }
    free(t.data);
    out[n] = 0;
    if (!size) RET_STDCALL(6, 0);
    if (n >= size) n = size - 1;
    memcpy(GPTR(buf), out, n);
    WR8(buf + n, 0);
    RET_STDCALL(6, n);
}

void imp_KERNEL32_GetPrivateProfileIntA(CPU *cpu) {
    uint32_t sec = ARG(0), key = ARG(1), def = ARG(2), file = ARG(3);
    char rel[1024];
    ini_path_rel(file, rel, sizeof rel);
    TextBuf t = ini_load(rel);
    const char *vs, *ve, *se;
    uint32_t v = def;
    if (t.data && ini_find(t.data, gstr(sec), gstr(key), &vs, &ve, &se) == 1) v = (uint32_t)strtol(vs, NULL, 10);
    free(t.data);
    RET_STDCALL(4, v);
}

void imp_KERNEL32_WritePrivateProfileStringA(CPU *cpu) {
    uint32_t sec = ARG(0), key = ARG(1), val = ARG(2), file = ARG(3);
    char rel[1024], guest[1100], path[1200];
    ini_path_rel(file, rel, sizeof rel);
    snprintf(guest, sizeof guest, "%s", rel);
    path_resolve_write(guest, path, sizeof path);
    TextBuf t = ini_load(rel);
    if (!t.data) { t.data = strdup(""); t.len = 0; }
    const char *s = gstr(sec);
    const char *vs = NULL, *ve = NULL, *se = NULL;
    int r = key ? ini_find(t.data, s, gstr(key), &vs, &ve, &se) : ini_find(t.data, s, NULL, &vs, &ve, &se);
    FILE *f = fopen(path, "wb");
    if (!f) { free(t.data); RET_STDCALL(4, 0); }
    if (r == 1) {
        /* replace or delete the existing line */
        const char *ls = vs;
        while (ls > t.data && ls[-1] != '\n') ls--;
        const char *le = ve;
        while (*le && *le != '\n') le++;
        if (*le) le++;
        fwrite(t.data, 1, (size_t)(ls - t.data), f);
        if (val) fprintf(f, "%s=%s\r\n", gstr(key), gstr(val));
        fputs(le, f);
    } else if (r == 0 && se) {
        fwrite(t.data, 1, (size_t)(se - t.data), f);
        if (key && val) fprintf(f, "%s=%s\r\n", gstr(key), gstr(val));
        fputs(se, f);
    } else {
        fputs(t.data, f);
        if (key && val) fprintf(f, "%s[%s]\r\n%s=%s\r\n", t.len && t.data[t.len - 1] != '\n' ? "\r\n" : "", s, gstr(key), gstr(val));
    }
    fclose(f);
    free(t.data);
    RET_STDCALL(4, 1);
}

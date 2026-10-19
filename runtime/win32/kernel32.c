/* KERNEL32 emulation. */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#include "win32.h"
#include "gamecfg.h"
#include "../pack/vfs.h"

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_FILE_EXISTS 80
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define ERROR_ENVVAR_NOT_FOUND 203
#define INVALID_HANDLE_VALUE 0xffffffffu

uint32_t mem_low_alloc(uint32_t size);
void mem_release(uint32_t a, uint32_t size);
void mem_decommit(uint32_t a, uint32_t size);
void cpu_set_current(CPU *c);

/* ---- last error -------------------------------------------------------------------- */
static __thread uint32_t t_last_error;
void win32_set_last_error(uint32_t e) { t_last_error = e; }
uint32_t win32_get_last_error(void) { return t_last_error; }

void imp_KERNEL32_GetLastError(CPU *cpu) { RET_STDCALL(0, t_last_error); }
void imp_KERNEL32_SetLastError(CPU *cpu) { t_last_error = ARG(0); RET_STDCALL(1, 0); }

/* ---- handles ------------------------------------------------------------------------- */
#define MAX_HANDLES 1024
static Handle g_handles[MAX_HANDLES];
static pthread_mutex_t g_handle_lock = PTHREAD_MUTEX_INITIALIZER;
#define HANDLE_BASE 0x1000u
#define STD_INPUT  0x0f00u
#define STD_OUTPUT 0x0f04u
#define STD_ERROR  0x0f08u

uint32_t handle_new(int type) {
    pthread_mutex_lock(&g_handle_lock);
    for (int i = 0; i < MAX_HANDLES; i++)
        if (g_handles[i].type == H_FREE) {
            memset(&g_handles[i], 0, sizeof(Handle));
            g_handles[i].type = type;
            pthread_mutex_unlock(&g_handle_lock);
            return HANDLE_BASE + i * 4;
        }
    pthread_mutex_unlock(&g_handle_lock);
    rt_fatal("out of handles");
}

Handle *handle_get(uint32_t h, int type) {
    if (h < HANDLE_BASE) return NULL;
    uint32_t i = (h - HANDLE_BASE) / 4;
    if (i >= MAX_HANDLES || (h & 3)) return NULL;
    Handle *hd = &g_handles[i];
    if (hd->type == H_FREE || (type && hd->type != type)) return NULL;
    return hd;
}

void handle_close(uint32_t h) {
    Handle *hd = handle_get(h, 0);
    if (!hd) return;
    if (hd->type == H_FILE && hd->vf) vfs_close(hd->vf);
    hd->type = H_FREE;
}

/* ---- paths ------------------------------------------------------------------------------ */
/* Where the game believes it is installed: game.ini [game] guest_dir + exe. */
static char g_exe_guest_buf[512];
const char *g_exe_guest_path = g_exe_guest_buf;
static char g_guest_dir_rel[256]; /* guest_dir without the drive, e.g. "CLAW\\" */

void win32_paths_init(void) {
    const char *dir = cfg_str("game", "guest_dir", "C:\\GAME");
    const char *exe = cfg_str("game", "exe", "game.exe");
    snprintf(g_exe_guest_buf, sizeof g_exe_guest_buf, "%s\\%s", dir, exe);
    for (char *c = g_exe_guest_buf; *c; c++) *c = (char)toupper((unsigned char)*c);
    const char *p = dir;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') p += 2;
    while (*p == '\\' || *p == '/') p++;
    snprintf(g_guest_dir_rel, sizeof g_guest_dir_rel, "%s", p);
    for (char *c = g_guest_dir_rel; *c; c++) if (*c == '/') *c = '\\';
    size_t n = strlen(g_guest_dir_rel);
    if (n && g_guest_dir_rel[n - 1] != '\\' && n + 1 < sizeof g_guest_dir_rel) strcat(g_guest_dir_rel, "\\");
}

/* Normalize a guest path to a relative, '/'-separated path below the game root. */
void guest_rel_path(const char *g, char *out, size_t n) {
    char tmp[1024];
    size_t j = 0;
    for (size_t i = 0; g[i] && j < sizeof tmp - 1; i++) tmp[j++] = g[i] == '/' ? '\\' : g[i];
    tmp[j] = 0;
    const char *p = tmp;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') p += 2;
    while (*p == '\\') p++;
    size_t gl = strlen(g_guest_dir_rel);
    if (gl > 1 && !strncasecmp(p, g_guest_dir_rel, gl)) p += gl;
    /* drop "." components and collapse separators */
    size_t o = 0;
    while (*p) {
        const char *s = p;
        while (*p && *p != '\\') p++;
        size_t len = (size_t)(p - s);
        while (*p == '\\') p++;
        if (len == 0 || (len == 1 && s[0] == '.')) continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            continue;
        }
        if (o && o < n - 1) out[o++] = '/';
        for (size_t k = 0; k < len && o < n - 1; k++) out[o++] = s[k];
    }
    out[o] = 0;
}

static void mkdirs_for(const char *path) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
}

int path_resolve_write(const char *guest, char *out, size_t outsz) {
    char rel[1024];
    guest_rel_path(guest, rel, sizeof rel);
    if (vfs_ci_lookup(platform_save_dir(), rel, out, outsz) == 0) return 0;
    /* keep the on-disk name lowercase in the overlay so later lookups are stable */
    char low[1024];
    for (size_t i = 0; i <= strlen(rel); i++) low[i] = (char)tolower((unsigned char)rel[i]);
    snprintf(out, outsz, "%s/%s", platform_save_dir(), low);
    mkdirs_for(out);
    return 0;
}

/* copies a game file (from any source) into the save overlay */
static int copy_to_overlay(const char *rel, const char *dst) {
    VFile *a = vfs_open(rel);
    if (!a) return -1;
    FILE *b = fopen(dst, "wb");
    if (!b) { vfs_close(a); return -1; }
    char buf[65536];
    size_t n;
    while ((n = vfs_read(a, buf, sizeof buf)) > 0) fwrite(buf, 1, n, b);
    vfs_close(a);
    fclose(b);
    return 0;
}

/* ---- files ------------------------------------------------------------------------------- */
void imp_KERNEL32_CreateFileA(CPU *cpu) {
    const char *name = gstr(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    int want_write = (access & 0x40000000u) != 0;
    char rel[1024], path[1200];
    guest_rel_path(name, rel, sizeof rel);
    VFile *vf = NULL;
    int is_dir = 0;
    int exists_read = vfs_stat(rel, &is_dir, NULL) == 0 && !is_dir;
    snprintf(path, sizeof path, "%s", rel);
    if (!want_write) {
        if (exists_read && (disp == 3 || disp == 4)) vf = vfs_open(rel);
        if (!vf && disp == 4) { /* OPEN_ALWAYS read-only: create empty in overlay */
            path_resolve_write(name, path, sizeof path);
            vf = vfs_from_stdio(fopen(path, "w+b"));
        }
    } else {
        char wpath[1200];
        path_resolve_write(name, wpath, sizeof wpath);
        struct stat st;
        int in_overlay = stat(wpath, &st) == 0;
        FILE *fp = NULL;
        switch (disp) {
        case 1: /* CREATE_NEW */
            if (exists_read) { win32_set_last_error(ERROR_FILE_EXISTS); RET_STDCALL(7, INVALID_HANDLE_VALUE); }
            fp = fopen(wpath, "w+b");
            break;
        case 2: /* CREATE_ALWAYS */
        case 5: /* TRUNCATE_EXISTING */
            if (disp == 5 && !exists_read) break;
            fp = fopen(wpath, "w+b");
            break;
        case 3: /* OPEN_EXISTING */
        case 4: /* OPEN_ALWAYS */
            if (!in_overlay && exists_read) copy_to_overlay(rel, wpath);
            fp = fopen(wpath, "r+b");
            if (!fp && disp == 4) fp = fopen(wpath, "w+b");
            break;
        }
        vf = vfs_from_stdio(fp);
        snprintf(path, sizeof path, "%s", wpath);
    }
    TRACE("CreateFileA(%s, %x, disp=%u) -> %s", name, access, disp, vf ? path : "FAIL");
    if (!vf) {
        win32_set_last_error(ERROR_FILE_NOT_FOUND);
        RET_STDCALL(7, INVALID_HANDLE_VALUE);
    }
    uint32_t h = handle_new(H_FILE);
    handle_get(h, H_FILE)->vf = vf;
    win32_set_last_error(0);
    RET_STDCALL(7, h);
}

static void console_write(const char *p, uint32_t n) {
    char buf[1024];
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, p, n);
    buf[n] = 0;
    rt_log("[guest stdout] %s", buf);
}

void imp_KERNEL32_ReadFile(CPU *cpu) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pread = ARG(3);
    Handle *hd = handle_get(h, H_FILE);
    if (!hd) {
        if (pread) WR32(pread, 0);
        win32_set_last_error(ERROR_INVALID_HANDLE);
        RET_STDCALL(5, 0);
    }
    size_t r = vfs_read(hd->vf, GPTR(buf), n);
    if (pread) WR32(pread, (uint32_t)r);
    RET_STDCALL(5, 1);
}

void imp_KERNEL32_WriteFile(CPU *cpu) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pwr = ARG(3);
    if (h == STD_OUTPUT || h == STD_ERROR) {
        console_write(GPTR(buf), n);
        if (pwr) WR32(pwr, n);
        RET_STDCALL(5, 1);
    }
    Handle *hd = handle_get(h, H_FILE);
    if (!hd) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        RET_STDCALL(5, 0);
    }
    size_t w = vfs_write(hd->vf, GPTR(buf), n);
    if (pwr) WR32(pwr, (uint32_t)w);
    RET_STDCALL(5, 1);
}

void imp_KERNEL32_SetFilePointer(CPU *cpu) {
    uint32_t h = ARG(0), phigh = ARG(2), method = ARG(3);
    int64_t dist = (int32_t)ARG(1);
    if (phigh) dist = (int64_t)(((uint64_t)RD32(phigh) << 32) | ARG(1));
    Handle *hd = handle_get(h, H_FILE);
    if (!hd) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        RET_STDCALL(4, 0xffffffffu);
    }
    static const int whence[3] = {SEEK_SET, SEEK_CUR, SEEK_END};
    int64_t pos = vfs_seek(hd->vf, dist, whence[method % 3]);
    if (pos < 0) {
        win32_set_last_error(131);
        RET_STDCALL(4, 0xffffffffu);
    }
    if (phigh) WR32(phigh, (uint32_t)(pos >> 32));
    win32_set_last_error(0);
    RET_STDCALL(4, (uint32_t)pos);
}

void imp_KERNEL32_SetEndOfFile(CPU *cpu) {
    Handle *hd = handle_get(ARG(0), H_FILE);
    if (!hd) RET_STDCALL(1, 0);
    FILE *fp = vfs_stdio(hd->vf);
    if (!fp) RET_STDCALL(1, 0);
    fflush(fp);
    int r = ftruncate(fileno(fp), ftello(fp));
    RET_STDCALL(1, r == 0);
}

void imp_KERNEL32_FlushFileBuffers(CPU *cpu) {
    Handle *hd = handle_get(ARG(0), H_FILE);
    if (hd && vfs_stdio(hd->vf)) fflush(vfs_stdio(hd->vf));
    RET_STDCALL(1, 1);
}

void imp_KERNEL32_GetFileType(CPU *cpu) {
    uint32_t h = ARG(0);
    if (h == STD_INPUT || h == STD_OUTPUT || h == STD_ERROR) RET_STDCALL(1, 2); /* FILE_TYPE_CHAR */
    RET_STDCALL(1, handle_get(h, H_FILE) ? 1 : 0);
}

void imp_KERNEL32_GetStdHandle(CPU *cpu) {
    int32_t n = (int32_t)ARG(0);
    uint32_t h = n == -10 ? STD_INPUT : n == -11 ? STD_OUTPUT : n == -12 ? STD_ERROR : INVALID_HANDLE_VALUE;
    RET_STDCALL(1, h);
}

void imp_KERNEL32_SetStdHandle(CPU *cpu) { RET_STDCALL(2, 1); }
void imp_KERNEL32_SetHandleCount(CPU *cpu) { RET_STDCALL(1, ARG(0)); }

void imp_KERNEL32_GetFileAttributesA(CPU *cpu) {
    const char *name = gstr(ARG(0));
    char rel[1024];
    int is_dir;
    guest_rel_path(name, rel, sizeof rel);
    if (vfs_stat(rel, &is_dir, NULL)) {
        TRACE("GetFileAttributesA(%s) -> not found", name);
        win32_set_last_error(ERROR_FILE_NOT_FOUND);
        RET_STDCALL(1, 0xffffffffu);
    }
    TRACE("GetFileAttributesA(%s) -> %s", name, is_dir ? "dir" : "file");
    RET_STDCALL(1, is_dir ? 0x10u : 0x80u);
}

void imp_KERNEL32_SetFileAttributesA(CPU *cpu) { RET_STDCALL(2, 1); }
void imp_KERNEL32_SetCurrentDirectoryA(CPU *cpu) {
    TRACE("SetCurrentDirectoryA(%s)", gstr(ARG(0)));
    RET_STDCALL(1, 1);
}
void imp_KERNEL32_GetDriveTypeA(CPU *cpu) { RET_STDCALL(1, 3); /* DRIVE_FIXED */ }
void imp_KERNEL32_GetLogicalDrives(CPU *cpu) { RET_STDCALL(0, 1u << 2); /* C: */ }

void imp_KERNEL32_CloseHandle(CPU *cpu) {
    uint32_t h = ARG(0);
    Handle *hd = handle_get(h, 0);
    if (hd && hd->type == H_THREAD) { /* keep thread record; just drop the handle */
        RET_STDCALL(1, 1);
    }
    handle_close(h);
    RET_STDCALL(1, 1);
}

void imp_KERNEL32_CreateFileMappingA(CPU *cpu) {
    win32_set_last_error(0);
    RET_STDCALL(6, handle_new(H_MAPPING));
}

/* ---- module / process ----------------------------------------------------------------------- */
void imp_KERNEL32_GetModuleFileNameA(CPU *cpu) {
    uint32_t buf = ARG(1), size = ARG(2);
    size_t len = strlen(g_exe_guest_path);
    if (size == 0) RET_STDCALL(3, 0);
    size_t n = len < size - 1 ? len : size - 1;
    memcpy(GPTR(buf), g_exe_guest_path, n);
    WR8(buf + n, 0);
    RET_STDCALL(3, (uint32_t)n);
}

void imp_KERNEL32_GetModuleHandleA(CPU *cpu) {
    const char *n = gstr(ARG(0));
    uint32_t h = 0;
    if (!n) h = HMODULE_EXE;
    else if (!strncasecmp(n, "user32", 6)) h = HMODULE_USER32;
    else if (!strncasecmp(n, "kernel32", 8)) h = HMODULE_KERNEL;
    TRACE("GetModuleHandleA(%s) -> %x", n ? n : "NULL", h);
    RET_STDCALL(1, h);
}

void imp_KERNEL32_LoadLibraryA(CPU *cpu) {
    RET_STDCALL(1, win32_load_library(gstr(ARG(0))));
}

void imp_KERNEL32_FreeLibrary(CPU *cpu) { RET_STDCALL(1, 1); }

void imp_KERNEL32_GetProcAddress(CPU *cpu) {
    uint32_t m = ARG(0), np = ARG(1);
    if (np < 0x10000) RET_STDCALL(2, 0);
    RET_STDCALL(2, win32_get_proc_address(m, gstr(np)));
}

void imp_KERNEL32_GetVersion(CPU *cpu) { RET_STDCALL(0, 0xC0000A04u); /* Windows 98 */ }

void imp_KERNEL32_GetVersionExA(CPU *cpu) {
    uint32_t p = ARG(0);
    WR32(p + 4, 4);              /* major */
    WR32(p + 8, 10);             /* minor */
    WR32(p + 12, 0x040A07CEu);   /* build 1998 */
    WR32(p + 16, 1);             /* VER_PLATFORM_WIN32_WINDOWS */
    memset(GPTR(p + 20), 0, 128);
    memcpy(GPTR(p + 20), " A", 2);
    RET_STDCALL(1, 1);
}

static uint32_t g_cmdline;
void imp_KERNEL32_GetCommandLineA(CPU *cpu) {
    if (!g_cmdline) {
        /* as Windows launches it: quoted path and a space; PORT_ARGS adds arguments */
        char buf[1024];
        const char *extra = getenv("PORT_ARGS");
        snprintf(buf, sizeof buf, "\"%s\" %s", g_exe_guest_path, extra ? extra : "");
        g_cmdline = mem_low_alloc((uint32_t)strlen(buf) + 1);
        strcpy(GPTR(g_cmdline), buf);
    }
    RET_STDCALL(0, g_cmdline);
}

void imp_KERNEL32_GetStartupInfoA(CPU *cpu) {
    uint32_t p = ARG(0);
    memset(GPTR(p), 0, 68);
    WR32(p, 68);
    RET_STDCALL(1, 0);
}

static uint32_t g_env_block;
void imp_KERNEL32_GetEnvironmentStrings(CPU *cpu) {
    if (!g_env_block) g_env_block = mem_low_alloc(16);
    RET_STDCALL(0, g_env_block);
}
void imp_KERNEL32_GetEnvironmentStringsW(CPU *cpu) {
    win32_set_last_error(ERROR_CALL_NOT_IMPLEMENTED);
    RET_STDCALL(0, 0);
}
void imp_KERNEL32_FreeEnvironmentStringsA(CPU *cpu) { RET_STDCALL(1, 1); }
void imp_KERNEL32_FreeEnvironmentStringsW(CPU *cpu) { RET_STDCALL(1, 1); }
void imp_KERNEL32_GetEnvironmentVariableA(CPU *cpu) {
    TRACE("GetEnvironmentVariableA(%s)", gstr(ARG(0)));
    win32_set_last_error(ERROR_ENVVAR_NOT_FOUND);
    RET_STDCALL(3, 0);
}
void imp_KERNEL32_SetEnvironmentVariableA(CPU *cpu) { RET_STDCALL(2, 1); }

void imp_KERNEL32_ExitProcess(CPU *cpu) {
    rt_log("ExitProcess(%u)", ARG(0));
    platform_shutdown();
    exit((int)ARG(0));
}
void imp_KERNEL32_TerminateProcess(CPU *cpu) {
    rt_log("TerminateProcess(%u)", ARG(1));
    platform_shutdown();
    exit((int)ARG(1));
}

void imp_KERNEL32_GetCurrentProcess(CPU *cpu) { RET_STDCALL(0, 0xffffffffu); }
void imp_KERNEL32_GetPriorityClass(CPU *cpu) { RET_STDCALL(1, 0x20); }
void imp_KERNEL32_SetPriorityClass(CPU *cpu) { RET_STDCALL(2, 1); }
void imp_KERNEL32_GetCurrentThread(CPU *cpu) { RET_STDCALL(0, 0xfffffffeu); }
void imp_KERNEL32_GetThreadPriority(CPU *cpu) { RET_STDCALL(1, 0); }
void imp_KERNEL32_SetThreadPriority(CPU *cpu) { RET_STDCALL(2, 1); }

void imp_KERNEL32_UnhandledExceptionFilter(CPU *cpu) { RET_STDCALL(1, 1); }
void imp_KERNEL32_RtlUnwind(CPU *cpu) {
    rt_fatal("RtlUnwind called (guest exception unwinding is not supported)");
}
void imp_KERNEL32_RaiseException(CPU *cpu) {
    rt_fatal("RaiseException(%08x) called by guest", ARG(0));
}

/* ---- memory ------------------------------------------------------------------------------------ */
void imp_KERNEL32_GetSystemInfo(CPU *cpu) {
    uint32_t p = ARG(0);
    memset(GPTR(p), 0, 36);
    WR32(p + 4, 4096);
    WR32(p + 8, 0x10000);
    WR32(p + 12, 0x7ffeffff);
    WR32(p + 16, 1);
    WR32(p + 20, 1);
    WR32(p + 24, 586);
    WR32(p + 28, 0x10000);
    WR16(p + 32, 6);
    RET_STDCALL(1, 0);
}

void imp_KERNEL32_VirtualQuery(CPU *cpu) {
    uint32_t a = ARG(0), p = ARG(1);
    uint32_t page = a & ~0xfffu;
    WR32(p + 0, page);
    WR32(p + 4, page & ~0xffffu);
    WR32(p + 8, 4);
    WR32(p + 12, ((page | 0xffffu) + 1) - page);
    WR32(p + 16, 0x1000);  /* MEM_COMMIT */
    WR32(p + 20, 4);       /* PAGE_READWRITE */
    WR32(p + 24, 0x20000); /* MEM_PRIVATE */
    RET_STDCALL(3, 28);
}

void imp_KERNEL32_VirtualProtect(CPU *cpu) {
    if (ARG(3)) WR32(ARG(3), 4);
    RET_STDCALL(4, 1);
}

#define MAX_REGIONS 4096
static struct { uint32_t addr, size; } g_regions[MAX_REGIONS];
static pthread_mutex_t g_region_lock = PTHREAD_MUTEX_INITIALIZER;

void imp_KERNEL32_VirtualAlloc(CPU *cpu) {
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (addr) {
        TRACE("VirtualAlloc(%08x, %x, %x) commit", addr, size, type);
        RET_STDCALL(4, addr & ~0xfffu);
    }
    uint32_t total = (size + 0xffff) & ~0xffffu;
    uint32_t a = mem_reserve(total, 0x10000);
    if (!a) {
        win32_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET_STDCALL(4, 0);
    }
    pthread_mutex_lock(&g_region_lock);
    for (int i = 0; i < MAX_REGIONS; i++)
        if (!g_regions[i].addr) {
            g_regions[i].addr = a;
            g_regions[i].size = total;
            break;
        }
    pthread_mutex_unlock(&g_region_lock);
    TRACE("VirtualAlloc(0, %x, %x) -> %08x", size, type, a);
    RET_STDCALL(4, a);
}

void imp_KERNEL32_VirtualFree(CPU *cpu) {
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (type & 0x8000) { /* MEM_RELEASE */
        pthread_mutex_lock(&g_region_lock);
        for (int i = 0; i < MAX_REGIONS; i++)
            if (g_regions[i].addr == addr) {
                uint32_t s = g_regions[i].size;
                g_regions[i].addr = 0;
                pthread_mutex_unlock(&g_region_lock);
                mem_release(addr, s);
                RET_STDCALL(3, 1);
            }
        pthread_mutex_unlock(&g_region_lock);
        RET_STDCALL(3, 0);
    }
    if (type & 0x4000) mem_decommit(addr, size);
    RET_STDCALL(3, 1);
}

void imp_KERNEL32_HeapCreate(CPU *cpu) { RET_STDCALL(3, 0x00500000u); }
void imp_KERNEL32_HeapDestroy(CPU *cpu) { RET_STDCALL(1, 1); }
void imp_KERNEL32_HeapAlloc(CPU *cpu) {
    uint32_t flags = ARG(1), size = ARG(2);
    uint32_t p = heap_alloc(size ? size : 1, (flags & 8) != 0);
    RET_STDCALL(3, p);
}
void imp_KERNEL32_HeapFree(CPU *cpu) {
    heap_free(ARG(2));
    RET_STDCALL(3, 1);
}
void imp_KERNEL32_HeapReAlloc(CPU *cpu) {
    uint32_t flags = ARG(1), p = ARG(2), size = ARG(3);
    if (flags & 0x10) { /* HEAP_REALLOC_IN_PLACE_ONLY */
        uint32_t c = RD32(p - 16 + 4);
        if (c < 13 && size + 16 <= (16u << c)) {
            uint32_t old = RD32(p - 16);
            if ((flags & 8) && size > old) memset(GPTR(p + old), 0, size - old);
            WR32(p - 16, size);
            RET_STDCALL(4, p);
        }
        RET_STDCALL(4, 0);
    }
    uint32_t old = heap_size(p);
    uint32_t n = heap_realloc(p, size);
    if (n && (flags & 8) && size > old) memset(GPTR(n + old), 0, size - old);
    RET_STDCALL(4, n);
}

/* ---- synchronization ----------------------------------------------------------------------------- */
#define MAX_CS 1024
static pthread_mutex_t g_cs[MAX_CS];
static int g_cs_count;
static pthread_mutex_t g_cs_alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *cs_get(uint32_t cs) {
    uint32_t id = RD32(cs + 16);
    if (id == 0 || id > MAX_CS) {
        /* not initialized via InitializeCriticalSection: do it lazily */
        pthread_mutex_lock(&g_cs_alloc_lock);
        id = RD32(cs + 16);
        if (id == 0 || id > MAX_CS) {
            if (g_cs_count >= MAX_CS) rt_fatal("too many critical sections");
            pthread_mutexattr_t at;
            pthread_mutexattr_init(&at);
            pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&g_cs[g_cs_count], &at);
            id = ++g_cs_count;
            WR32(cs + 16, id);
        }
        pthread_mutex_unlock(&g_cs_alloc_lock);
    }
    return &g_cs[id - 1];
}

void imp_KERNEL32_InitializeCriticalSection(CPU *cpu) {
    uint32_t cs = ARG(0);
    memset(GPTR(cs), 0, 24);
    cs_get(cs);
    RET_STDCALL(1, 0);
}
void imp_KERNEL32_DeleteCriticalSection(CPU *cpu) { RET_STDCALL(1, 0); }
void imp_KERNEL32_EnterCriticalSection(CPU *cpu) {
    pthread_mutex_lock(cs_get(ARG(0)));
    RET_STDCALL(1, 0);
}
void imp_KERNEL32_LeaveCriticalSection(CPU *cpu) {
    pthread_mutex_unlock(cs_get(ARG(0)));
    RET_STDCALL(1, 0);
}
void imp_KERNEL32_InterlockedIncrement(CPU *cpu) {
    uint32_t r = __atomic_add_fetch((uint32_t *)GPTR(ARG(0)), 1, __ATOMIC_SEQ_CST);
    RET_STDCALL(1, r);
}
void imp_KERNEL32_InterlockedDecrement(CPU *cpu) {
    uint32_t r = __atomic_sub_fetch((uint32_t *)GPTR(ARG(0)), 1, __ATOMIC_SEQ_CST);
    RET_STDCALL(1, r);
}

/* ---- threads --------------------------------------------------------------------------------------- */
typedef struct {
    pthread_t th;
    CPU *cpu;
    uint32_t start, param;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int suspend_count;
    int terminated;
    uint32_t id;
} GuestThread;

static __thread GuestThread *t_thread;

static void thread_check_terminate(void) {
    GuestThread *t = t_thread;
    if (t && __atomic_load_n(&t->terminated, __ATOMIC_SEQ_CST)) {
        rt_log("guest thread %u terminated", t->id);
        pthread_exit(NULL);
    }
}

static void thread_wait_resumed(GuestThread *t) {
    pthread_mutex_lock(&t->lock);
    while (t->suspend_count > 0 && !t->terminated) pthread_cond_wait(&t->cond, &t->lock);
    pthread_mutex_unlock(&t->lock);
    thread_check_terminate();
}

static void *thread_main(void *arg) {
    GuestThread *t = arg;
    t_thread = t;
    cpu_set_current(t->cpu);
    thread_wait_resumed(t);
    uint32_t a = t->param;
    guest_call(t->cpu, t->start, 1, &a);
    rt_log("guest thread %u returned", t->id);
    return NULL;
}

void imp_KERNEL32_CreateThread(CPU *cpu) {
    uint32_t stack = ARG(1), start = ARG(2), param = ARG(3), flags = ARG(4), ptid = ARG(5);
    GuestThread *t = calloc(1, sizeof *t);
    t->cpu = cpu_new_thread(stack ? (stack + 0xffff) & ~0xffffu : 0x100000);
    t->start = start;
    t->param = param;
    t->suspend_count = (flags & 4) ? 1 : 0;
    t->id = 0x200 + t->cpu->thread_index * 4;
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->cond, NULL);
    uint32_t h = handle_new(H_THREAD);
    handle_get(h, H_THREAD)->ptr = t;
    if (ptid) WR32(ptid, t->id);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8 << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->th, &at, thread_main, t)) rt_fatal("pthread_create failed");
    rt_log("CreateThread(start=%08x, param=%08x) -> handle %x", start, param, h);
    RET_STDCALL(6, h);
}

static GuestThread *thread_from_handle(uint32_t h) {
    if (h == 0xfffffffeu) return t_thread;
    Handle *hd = handle_get(h, H_THREAD);
    return hd ? hd->ptr : NULL;
}

void imp_KERNEL32_SuspendThread(CPU *cpu) {
    GuestThread *t = thread_from_handle(ARG(0));
    if (!t) RET_STDCALL(1, 0xffffffffu);
    pthread_mutex_lock(&t->lock);
    int prev = t->suspend_count++;
    pthread_mutex_unlock(&t->lock);
    if (t == t_thread) {
        /* complete the stdcall before blocking so a later resume continues cleanly */
        cpu->eax = (uint32_t)prev;
        cpu->ret_addr = RD32(cpu->esp);
        cpu->esp += 8;
        thread_wait_resumed(t);
        return;
    }
    rt_log("SuspendThread on another thread is not supported (ignored)");
    RET_STDCALL(1, prev);
}

void imp_KERNEL32_ResumeThread(CPU *cpu) {
    GuestThread *t = thread_from_handle(ARG(0));
    if (!t) RET_STDCALL(1, 0xffffffffu);
    pthread_mutex_lock(&t->lock);
    int prev = t->suspend_count;
    if (t->suspend_count > 0) t->suspend_count--;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);
    RET_STDCALL(1, prev);
}

void imp_KERNEL32_TerminateThread(CPU *cpu) {
    GuestThread *t = thread_from_handle(ARG(0));
    if (!t) RET_STDCALL(2, 0);
    pthread_mutex_lock(&t->lock);
    t->terminated = 1;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);
    if (t == t_thread) pthread_exit(NULL);
    RET_STDCALL(2, 1);
}

void imp_KERNEL32_GetCurrentThreadId(CPU *cpu) {
    RET_STDCALL(0, 0x200 + cpu->thread_index * 4);
}

uint64_t g_sleep_us; /* time the main thread spent sleeping (performance stats) */
void imp_KERNEL32_Sleep(CPU *cpu) {
    uint32_t ms = ARG(0);
    uint64_t t0 = platform_ticks_us();
    thread_check_terminate();
    if (cpu->thread_index == 0) platform_pump_events();
    platform_sleep_ms(ms);
    if (cpu->thread_index == 0) g_sleep_us += platform_ticks_us() - t0;
    thread_check_terminate();
    RET_STDCALL(1, 0);
}

/* TLS */
#define TLS_SLOTS 64
static int g_tls_used[TLS_SLOTS];
static __thread uint32_t t_tls[TLS_SLOTS];
void imp_KERNEL32_TlsAlloc(CPU *cpu) {
    for (int i = 0; i < TLS_SLOTS; i++)
        if (!__atomic_exchange_n(&g_tls_used[i], 1, __ATOMIC_SEQ_CST)) RET_STDCALL(0, i);
    RET_STDCALL(0, 0xffffffffu);
}
void imp_KERNEL32_TlsSetValue(CPU *cpu) {
    uint32_t i = ARG(0);
    if (i >= TLS_SLOTS) RET_STDCALL(2, 0);
    t_tls[i] = ARG(1);
    RET_STDCALL(2, 1);
}
void imp_KERNEL32_TlsGetValue(CPU *cpu) {
    uint32_t i = ARG(0);
    win32_set_last_error(0);
    RET_STDCALL(1, i < TLS_SLOTS ? t_tls[i] : 0);
}

/* ---- time ------------------------------------------------------------------------------------------- */
void imp_KERNEL32_QueryPerformanceFrequency(CPU *cpu) {
    WR64(ARG(0), 1000000ull);
    RET_STDCALL(1, 1);
}
void imp_KERNEL32_QueryPerformanceCounter(CPU *cpu) {
    WR64(ARG(0), platform_ticks_us());
    RET_STDCALL(1, 1);
}

/* ---- code pages / locale (Windows 98 behaviour: no wide-char APIs) ------------------------------------ */
void imp_KERNEL32_GetACP(CPU *cpu) { RET_STDCALL(0, 1252); }
void imp_KERNEL32_GetOEMCP(CPU *cpu) { RET_STDCALL(0, 437); }
void imp_KERNEL32_GetCPInfo(CPU *cpu) {
    uint32_t p = ARG(1);
    memset(GPTR(p), 0, 20);
    WR32(p, 1);
    WR8(p + 4, '?');
    RET_STDCALL(2, 1);
}

void imp_KERNEL32_MultiByteToWideChar(CPU *cpu) {
    uint32_t src = ARG(2), dst = ARG(4);
    int32_t srclen = (int32_t)ARG(3), dstlen = (int32_t)ARG(5);
    if (srclen < 0) srclen = (int32_t)strlen(gstr(src)) + 1;
    if (dstlen == 0) RET_STDCALL(6, srclen);
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) WR16(dst + i * 2, RD8(src + i));
    RET_STDCALL(6, n);
}

void imp_KERNEL32_WideCharToMultiByte(CPU *cpu) {
    uint32_t src = ARG(2), dst = ARG(4);
    int32_t srclen = (int32_t)ARG(3), dstlen = (int32_t)ARG(5);
    if (srclen < 0) {
        srclen = 0;
        while (RD16(src + srclen * 2)) srclen++;
        srclen++;
    }
    if (dstlen == 0) RET_STDCALL(8, srclen);
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) {
        uint16_t c = RD16(src + i * 2);
        WR8(dst + i, c < 256 ? c : '?');
    }
    RET_STDCALL(8, n);
}

void imp_KERNEL32_LCMapStringA(CPU *cpu) {
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t srclen = (int32_t)ARG(3), dstlen = (int32_t)ARG(5);
    if (srclen < 0) srclen = (int32_t)strlen(gstr(src)) + 1;
    if (dstlen == 0) RET_STDCALL(6, srclen);
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) {
        int c = RD8(src + i);
        if (flags & 0x100) c = tolower(c);
        else if (flags & 0x200) c = toupper(c);
        WR8(dst + i, c);
    }
    RET_STDCALL(6, n);
}

void imp_KERNEL32_LCMapStringW(CPU *cpu) {
    win32_set_last_error(ERROR_CALL_NOT_IMPLEMENTED);
    RET_STDCALL(6, 0);
}

void imp_KERNEL32_GetStringTypeA(CPU *cpu) {
    uint32_t src = ARG(2), out = ARG(4);
    int32_t len = (int32_t)ARG(3);
    if (len < 0) len = (int32_t)strlen(gstr(src)) + 1;
    for (int i = 0; i < len; i++) {
        int c = RD8(src + i);
        uint16_t t = 0;
        if (c < 128) {
            if (isupper(c)) t |= 0x1;
            if (islower(c)) t |= 0x2;
            if (isdigit(c)) t |= 0x4;
            if (isspace(c)) t |= 0x8;
            if (ispunct(c)) t |= 0x10;
            if (iscntrl(c)) t |= 0x20;
            if (c == ' ' || c == '\t') t |= 0x40;
            if (isxdigit(c)) t |= 0x80;
            if (isalpha(c)) t |= 0x100;
        }
        WR16(out + i * 2, t);
    }
    RET_STDCALL(5, 1);
}

void imp_KERNEL32_GetStringTypeW(CPU *cpu) {
    win32_set_last_error(ERROR_CALL_NOT_IMPLEMENTED);
    RET_STDCALL(4, 0);
}

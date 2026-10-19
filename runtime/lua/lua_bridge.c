#define _GNU_SOURCE
/* The game's Lua side (CrazyHook): a native LuaJIT whose FFI works on guest memory
 * (see tools/luajit_guest32.py), with
 *   - an allocator on the guest heap, so cdata and strings have guest addresses;
 *   - file access (io.open, dofile, require) through the VFS: save overlay, game
 *     folder, pack; writes go to the overlay;
 *   - an lfs module over the merged VFS directory listing;
 *   - ffi.C.Name resolved to the emulated Win32 API, and calls into guest code. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "lua_bridge.h"
#include "../runtime.h"
#include "../win32/win32.h"
#include "../pack/vfs.h"
#include "../gamecfg.h"

/* ---- allocator --------------------------------------------------------------------- */
#define H2G(p) ((p) ? (uint32_t)((const uint8_t *)(p) - g_mem) : 0u)

static void *guest_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud; (void)osize;
    if (nsize == 0) {
        if (ptr) heap_free(H2G(ptr));
        return NULL;
    }
    if (nsize > 0x40000000u) return NULL;
    uint32_t g = ptr ? heap_realloc(H2G(ptr), (uint32_t)nsize) : heap_alloc((uint32_t)nsize, 0);
    return g ? GPTR(g) : NULL;
}

lua_State *lua_bridge_newstate(void) {
    lua_State *L = lua_newstate(guest_lua_alloc, NULL);
    if (!L) rt_fatal("LuaJIT: lua_newstate failed");
    luaL_openlibs(L);
    lua_bridge_open_lfs(L);
    return L;
}

/* ---- files ---------------------------------------------------------------------------- */
static int lua_rel_path(const char *path, char *rel, size_t n) {
    guest_rel_path(path, rel, n);
    return 0;
}

static int copy_file_to(const char *rel, const char *dst) {
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

/* stdio over a VFile (pack entries) */
#ifdef __ANDROID__
static int vf_readfn(void *c, char *buf, int n) { return (int)vfs_read(c, buf, (size_t)n); }
static fpos_t vf_seekfn(void *c, fpos_t off, int wh) { return (fpos_t)vfs_seek(c, (int64_t)off, wh); }
static int vf_closefn(void *c) { vfs_close(c); return 0; }
static FILE *vfile_stdio(VFile *vf) { return funopen(vf, vf_readfn, NULL, vf_seekfn, vf_closefn); }
#else
static ssize_t vf_read(void *c, char *buf, size_t n) { return (ssize_t)vfs_read(c, buf, n); }
static int vf_seek(void *c, off64_t *off, int wh) {
    int64_t r = vfs_seek(c, *off, wh);
    if (r < 0) return -1;
    *off = r;
    return 0;
}
static int vf_close(void *c) { vfs_close(c); return 0; }
static FILE *vfile_stdio(VFile *vf) {
    cookie_io_functions_t io = {vf_read, NULL, vf_seek, vf_close};
    return fopencookie(vf, "rb", io);
}
#endif

FILE *port_fopen(const char *path, const char *mode) {
    if (!path) { errno = EINVAL; return NULL; }
    if (path[0] == '/') return fopen(path, mode); /* host paths (none from the game) */
    char rel[1024], real[1200];
    lua_rel_path(path, rel, sizeof rel);
    int write = strpbrk(mode, "wa+") != NULL;
    if (write) {
        path_resolve_write(path, real, sizeof real);
        struct stat st;
        int is_dir = 0;
        if ((strchr(mode, 'a') || strchr(mode, '+')) && !strchr(mode, 'w') && stat(real, &st) != 0 &&
            vfs_stat(rel, &is_dir, NULL) == 0 && !is_dir)
            copy_file_to(rel, real);
        FILE *f = fopen(real, mode);
        TRACE("lua fopen(%s, %s) -> overlay %s %s", path, mode, real, f ? "ok" : "failed");
        return f;
    }
    if (vfs_loose_path(rel, real, sizeof real) == 0) {
        struct stat st;
        if (stat(real, &st) == 0 && S_ISDIR(st.st_mode)) { errno = EISDIR; return NULL; }
        return fopen(real, mode);
    }
    VFile *vf = vfs_open(rel);
    if (!vf) { errno = ENOENT; return NULL; }
    return vfile_stdio(vf);
}

int port_remove(const char *path) {
    char real[1200];
    path_resolve_write(path, real, sizeof real);
    return remove(real);
}

int port_rename(const char *a, const char *b) {
    char ra[1200], rb[1200], rel[1024];
    path_resolve_write(a, ra, sizeof ra);
    path_resolve_write(b, rb, sizeof rb);
    struct stat st;
    if (stat(ra, &st) != 0) { /* only in the game folder: copy into the overlay first */
        lua_rel_path(a, rel, sizeof rel);
        if (copy_file_to(rel, ra)) return -1;
    }
    return rename(ra, rb);
}

/* ---- lfs (LuaFileSystem subset over the VFS) -------------------------------------------- */
typedef struct { char **names; int n, cap; } Names;
static void collect(void *ctx, const char *name, int is_dir) {
    (void)is_dir;
    Names *ns = ctx;
    if (ns->n == ns->cap) { ns->cap = ns->cap ? ns->cap * 2 : 64; ns->names = realloc(ns->names, sizeof(char *) * (size_t)ns->cap); }
    ns->names[ns->n++] = strdup(name);
}

static int lfs_dir_iter(lua_State *L) {
    Names *ns = lua_touserdata(L, lua_upvalueindex(1));
    int i = (int)lua_tointeger(L, lua_upvalueindex(2));
    if (i >= ns->n) return 0;
    lua_pushinteger(L, i + 1);
    lua_replace(L, lua_upvalueindex(2));
    lua_pushstring(L, ns->names[i]);
    return 1;
}

static int lfs_names_gc(lua_State *L) {
    Names *ns = lua_touserdata(L, 1);
    for (int i = 0; i < ns->n; i++) free(ns->names[i]);
    free(ns->names);
    ns->names = NULL;
    ns->n = 0;
    return 0;
}

static int lfs_dir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    char rel[1024];
    lua_rel_path(path, rel, sizeof rel);
    Names *ns = lua_newuserdata(L, sizeof(Names));
    memset(ns, 0, sizeof *ns);
    if (luaL_newmetatable(L, "port.lfs.names")) {
        lua_pushcfunction(L, lfs_names_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    collect(ns, ".", 1);
    collect(ns, "..", 1);
    if (vfs_list_dir(rel, collect, ns)) return luaL_error(L, "cannot open %s: No such file or directory", path);
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, lfs_dir_iter, 2);
    return 1;
}

static int lfs_attributes(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    char rel[1024];
    lua_rel_path(path, rel, sizeof rel);
    int is_dir = 0;
    int64_t size = 0;
    if (vfs_stat(rel, &is_dir, &size)) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot obtain information from file '%s': No such file or directory", path);
        lua_pushinteger(L, 2);
        return 3;
    }
    const char *want = lua_isstring(L, 2) ? lua_tostring(L, 2) : NULL;
    lua_newtable(L);
    lua_pushstring(L, is_dir ? "directory" : "file");
    lua_setfield(L, -2, "mode");
    lua_pushnumber(L, (lua_Number)size);
    lua_setfield(L, -2, "size");
    lua_pushnumber(L, 883612800.0); /* 1998-01-01 */
    lua_setfield(L, -2, "modification");
    lua_pushnumber(L, 883612800.0);
    lua_setfield(L, -2, "access");
    lua_pushnumber(L, 883612800.0);
    lua_setfield(L, -2, "change");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "dev");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "ino");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "nlink");
    lua_pushstring(L, "rw-rw-rw-");
    lua_setfield(L, -2, "permissions");
    if (want) {
        lua_getfield(L, -1, want);
        return 1;
    }
    return 1;
}

static int lfs_currentdir(lua_State *L) {
    lua_pushstring(L, cfg_str("game", "guest_dir", "C:\\GAME"));
    return 1;
}

static int lfs_mkdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    char real[1200], probe[1300];
    snprintf(probe, sizeof probe, "%s\\.probe", path);
    path_resolve_write(probe, real, sizeof real); /* creates the parents */
    char *s = strrchr(real, '/');
    if (s) *s = 0;
    mkdir(real, 0755);
    lua_pushboolean(L, 1);
    return 1;
}

static int lfs_rmdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    char real[1200];
    path_resolve_write(path, real, sizeof real);
    lua_pushboolean(L, rmdir(real) == 0);
    return 1;
}

static int lfs_touch(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    FILE *f = port_fopen(path, "ab");
    if (f) fclose(f);
    lua_pushboolean(L, f != NULL);
    return 1;
}

static int lfs_chdir(lua_State *L) {
    lua_pushboolean(L, 1); /* the game never leaves its folder */
    return 1;
}

static int luaopen_port_lfs(lua_State *L) {
    static const luaL_Reg f[] = {{"dir", lfs_dir}, {"attributes", lfs_attributes}, {"currentdir", lfs_currentdir},
                                 {"mkdir", lfs_mkdir}, {"rmdir", lfs_rmdir}, {"touch", lfs_touch},
                                 {"chdir", lfs_chdir}, {NULL, NULL}};
    luaL_register(L, "lfs", f);
    lua_pushstring(L, "LuaFileSystem 1.8.0 (port)");
    lua_setfield(L, -2, "_VERSION");
    return 1;
}

void lua_bridge_open_lfs(lua_State *L) {
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, luaopen_port_lfs);
    lua_setfield(L, -2, "lfs");
    lua_pop(L, 2);
}

/* ---- FFI hooks ------------------------------------------------------------------------------ */
/* Win32 entry points the scripts call that the exe itself doesn't import */
extern void imp_GDI32_CreateHatchBrush(CPU *), imp_GDI32_Ellipse(CPU *), imp_GDI32_FillRgn(CPU *),
    imp_GDI32_CombineRgn(CPU *), imp_GDI32_CreateRectRgnIndirect(CPU *), imp_GDI32_GetPixel(CPU *),
    imp_GDI32_SetPixel(CPU *), imp_GDI32_SetTextAlign(CPU *), imp_GDI32_SetROP2(CPU *), imp_USER32_SendInput(CPU *),
    imp_USER32_ScreenToClient(CPU *), imp_USER32_GetWindowDC(CPU *), imp_USER32_SetCapture(CPU *),
    imp_USER32_ReleaseCapture(CPU *), imp_GDI32_CreatePolygonRgn(CPU *), imp_GDI32_PtInRegion(CPU *),
    imp_GDI32_RectInRegion(CPU *), imp_USER32_LoadImageA(CPU *), imp_USER32_GetKeyboardState(CPU *),
    imp_KERNEL32_VirtualProtect(CPU *), imp_USER32_DefDlgProcA(CPU *), imp_USER32_CreateDialogParamA(CPU *),
    imp_USER32_DialogBoxIndirectParamA(CPU *), imp_USER32_PostThreadMessageA(CPU *), imp_USER32_wvsprintfA(CPU *),
    imp_GDI32_CreateCompatibleDC(CPU *), imp_GDI32_DeleteDC(CPU *);
static const struct { const char *name; GuestFn fn; } g_extra[] = {
    {"CreateHatchBrush", imp_GDI32_CreateHatchBrush}, {"Ellipse", imp_GDI32_Ellipse}, {"FillRgn", imp_GDI32_FillRgn},
    {"CombineRgn", imp_GDI32_CombineRgn}, {"CreateRectRgnIndirect", imp_GDI32_CreateRectRgnIndirect},
    {"GetPixel", imp_GDI32_GetPixel}, {"SetPixel", imp_GDI32_SetPixel}, {"SetTextAlign", imp_GDI32_SetTextAlign},
    {"SetROP2", imp_GDI32_SetROP2}, {"SendInput", imp_USER32_SendInput}, {"ScreenToClient", imp_USER32_ScreenToClient},
    {"GetWindowDC", imp_USER32_GetWindowDC}, {"SetCapture", imp_USER32_SetCapture}, {"ReleaseCapture", imp_USER32_ReleaseCapture},
    {"CreatePolygonRgn", imp_GDI32_CreatePolygonRgn}, {"PtInRegion", imp_GDI32_PtInRegion},
    {"RectInRegion", imp_GDI32_RectInRegion}, {"LoadImageA", imp_USER32_LoadImageA},
    {"GetKeyboardState", imp_USER32_GetKeyboardState}, {"VirtualProtect", imp_KERNEL32_VirtualProtect},
    {"DefDlgProcA", imp_USER32_DefDlgProcA}, {"CreateDialogParamA", imp_USER32_CreateDialogParamA},
    {"DialogBoxIndirectParamA", imp_USER32_DialogBoxIndirectParamA}, {"PostThreadMessageA", imp_USER32_PostThreadMessageA},
    {"wvsprintfA", imp_USER32_wvsprintfA}, {"CreateCompatibleDC", imp_GDI32_CreateCompatibleDC}, {"DeleteDC", imp_GDI32_DeleteDC},
    {NULL, NULL}};

void *lj_guest_resolve(const char *name) {
    for (int i = 0; i < g_import_count; i++)
        if (!strcmp(g_import_table[i].name, name)) return GPTR(thunk_register(g_import_table[i].fn, g_import_table[i].name));
    for (int i = 0; g_extra[i].name; i++)
        if (!strcmp(g_extra[i].name, name)) return GPTR(thunk_register(g_extra[i].fn, g_extra[i].name));
    rt_log("lua: ffi.C.%s is not available", name);
    return NULL;
}

int lj_guest_ffi_call(uint32_t target, const uint32_t *stack, int nstack, int nreg, uint32_t reg0, uint32_t reg1,
                      uint32_t *eax, uint32_t *edx, double *st0, int fpret) {
    CPU *cpu = cpu_current();
    uint32_t ecx0 = cpu->ecx, edx0 = cpu->edx;
    if (g_trace_api)
        rt_log("ffi call %08x regs(%d) %08x %08x stack(%d) %08x %08x %08x", target, nreg, reg0, reg1, nstack,
               nstack > 0 ? stack[0] : 0, nstack > 1 ? stack[1] : 0, nstack > 2 ? stack[2] : 0);
    if (nreg >= 1) cpu->ecx = reg0;
    if (nreg >= 2) cpu->edx = reg1;
    *eax = guest_call(cpu, target, nstack, stack);
    *edx = cpu->edx;
    if (fpret) {
        *st0 = cpu->fpr[cpu->ftop & 7];
        cpu->ftop = (cpu->ftop + 1) & 7; /* the caller pops ST0 */
    }
    cpu->ecx = ecx0;
    cpu->edx = edx0;
    return 0;
}

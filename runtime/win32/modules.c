/* Dynamically loaded modules (LoadLibrary/GetProcAddress) and COM helpers. */
#include <string.h>
#include <strings.h>
#include "win32.h"

void dsound_DirectSoundCreate(CPU *cpu);
void user32_GetLastActivePopup(CPU *cpu);
void ddraw_DirectDrawCreate(CPU *cpu);
void ddraw_DirectDrawEnumerateA(CPU *cpu);

uint32_t guest_alloc(uint32_t size) { return heap_alloc(size, 1); }

uint32_t com_make_vtable(const GuestFn *methods, const char *const *names, int count) {
    uint32_t vt = guest_alloc((uint32_t)count * 4);
    for (int i = 0; i < count; i++) WR32(vt + i * 4, thunk_register(methods[i], names ? names[i] : "COM method"));
    return vt;
}

uint32_t win32_load_library(const char *name) {
    uint32_t h = 0;
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    if (!strcasecmp(base, "dsound.dll") || !strcasecmp(base, "dsound")) h = HMODULE_DSOUND;
    else if (!strcasecmp(base, "user32.dll") || !strcasecmp(base, "user32")) h = HMODULE_USER32;
    else if (!strcasecmp(base, "kernel32.dll")) h = HMODULE_KERNEL;
    else if (!strcasecmp(base, "ddraw.dll") || !strcasecmp(base, "ddraw")) h = HMODULE_DDRAW;
    else if (!strcasecmp(base, "crazyhook.dll") || !strcasecmp(base, "crazyhook")) {
        uint32_t crazyhook_load(void);
        h = crazyhook_load();
    }
    /* glide2x.dll is deliberately absent and DirectDraw offers no Direct3D, so
     * the game uses its software renderer. */
    rt_log("LoadLibraryA(%s) -> %08x", name, h);
    return h;
}

uint32_t win32_get_proc_address(uint32_t module, const char *name) {
    uint32_t r = 0;
    uint32_t crazyhook_proc(uint32_t module, const char *name);
    if ((r = crazyhook_proc(module, name))) {
    } else if (module == HMODULE_DSOUND && !strcmp(name, "DirectSoundCreate"))
        r = thunk_register(dsound_DirectSoundCreate, "DirectSoundCreate");
    else if (module == HMODULE_DDRAW && !strcmp(name, "DirectDrawCreate"))
        r = thunk_register(ddraw_DirectDrawCreate, "DirectDrawCreate");
    else if (module == HMODULE_DDRAW && !strcmp(name, "DirectDrawEnumerateA"))
        r = thunk_register(ddraw_DirectDrawEnumerateA, "DirectDrawEnumerateA");
    else if (module == HMODULE_USER32 && !strcmp(name, "GetLastActivePopup"))
        r = thunk_register(user32_GetLastActivePopup, "GetLastActivePopup");
    else {
        const char *dll = module == HMODULE_USER32 ? "USER32.dll" : module == HMODULE_KERNEL ? "KERNEL32.dll" : NULL;
        if (dll)
            for (int i = 0; i < g_import_count; i++)
                if (!strcasecmp(g_import_table[i].dll, dll) && !strcmp(g_import_table[i].name, name))
                    r = thunk_register(g_import_table[i].fn, g_import_table[i].name);
    }
    /* the patched exe looks up Chameleon on every hook call: log each name once */
    static char logged[64][48];
    static int nlogged;
    int seen = 0;
    for (int i = 0; i < nlogged; i++)
        if (!strcmp(logged[i], name)) seen = 1;
    if (!seen) {
        if (nlogged < 64) snprintf(logged[nlogged++], sizeof logged[0], "%s", name);
        rt_log("GetProcAddress(%08x, %s) -> %08x", module, name, r);
    }
    return r;
}

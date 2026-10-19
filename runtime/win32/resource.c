/* Resources of the exe image (.rsrc), read in place from guest memory. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "win32.h"
#include "image_info.h"

static uint32_t g_rsrc; /* guest address of the resource directory root */

static uint32_t rsrc_root(void) {
    if (!g_rsrc) {
        uint32_t pe = IMAGE_BASE + RD32(IMAGE_BASE + 0x3c);
        uint32_t rva = RD32(pe + 0x18 + 0x60 + 2 * 8); /* data directory 2 = resources */
        if (rva) g_rsrc = IMAGE_BASE + rva;
    }
    return g_rsrc;
}

/* compares a directory entry's name (UTF-16 counted string) with an ASCII name */
static int name_eq(uint32_t ustr, const char *s) {
    uint32_t n = RD16(ustr);
    if (strlen(s) != n) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (toupper(RD16(ustr + 2 + 2 * i) & 0xff) != toupper((unsigned char)s[i])) return 0;
    return 1;
}

/* looks up one level: by id (s == NULL) or by name; returns the subentry's offset word */
static uint32_t dir_lookup(uint32_t dir, int id, const char *s, int first) {
    uint32_t root = rsrc_root();
    uint32_t nnamed = RD16(dir + 12), nid = RD16(dir + 14);
    uint32_t e = dir + 16;
    for (uint32_t i = 0; i < nnamed + nid; i++, e += 8) {
        uint32_t nm = RD32(e), off = RD32(e + 4);
        if (first) return off;
        if (nm & 0x80000000u) {
            if (s && name_eq(root + (nm & 0x7fffffffu), s)) return off;
        } else if (!s && (int)nm == id) {
            return off;
        }
    }
    return 0xffffffffu;
}

uint32_t res_find_host(int type, const char *name) {
    uint32_t root = rsrc_root();
    if (!root) return 0;
    uint32_t off = dir_lookup(root, type, NULL, 0);
    if (off == 0xffffffffu || !(off & 0x80000000u)) return 0;
    uint32_t d2 = root + (off & 0x7fffffffu);
    int id = 0;
    const char *s = name;
    if (name[0] == '#') { id = (int)strtol(name + 1, NULL, 10); s = NULL; }
    off = dir_lookup(d2, id, s, 0);
    if (off == 0xffffffffu || !(off & 0x80000000u)) return 0;
    uint32_t d3 = root + (off & 0x7fffffffu);
    off = dir_lookup(d3, 0, NULL, 1); /* any language */
    if (off == 0xffffffffu || (off & 0x80000000u)) return 0;
    return root + off;
}

static void res_name(uint32_t v, char *out, size_t n) {
    if (v < 0x10000) snprintf(out, n, "#%u", v);
    else snprintf(out, n, "%s", gstr(v));
}

uint32_t res_find(uint32_t type, uint32_t name) {
    char nm[256], ty[64];
    res_name(name, nm, sizeof nm);
    if (type >= 0x10000) {
        /* named types are rare (none in the games so far) */
        res_name(type, ty, sizeof ty);
        return 0;
    }
    return res_find_host((int)type, nm);
}

uint32_t res_data(uint32_t entry, uint32_t *size) {
    if (!entry) return 0;
    if (size) *size = RD32(entry + 4);
    return IMAGE_BASE + RD32(entry);
}

int res_load_string(uint32_t id, char *out, int outsz) {
    char nm[32];
    snprintf(nm, sizeof nm, "#%u", id / 16 + 1);
    uint32_t e = res_find_host(6, nm);
    if (!e) return -1;
    uint32_t p = res_data(e, NULL);
    for (uint32_t k = 0; k < (id & 15); k++) p += 2 + 2 * RD16(p);
    int n = RD16(p);
    if (!n) return -1;
    int o = 0;
    for (int i = 0; i < n && o < outsz - 1; i++) out[o++] = (char)RD16(p + 2 + 2 * i);
    out[o] = 0;
    return o;
}

/* ---- KERNEL32 resource APIs (only the exe's own resources exist) ------------------------- */
void imp_KERNEL32_FindResourceA(CPU *cpu) {
    uint32_t e = res_find(ARG(2), ARG(1));
    if (!e) {
        char nm[256];
        res_name(ARG(1), nm, sizeof nm);
        rt_log("FindResourceA(%s, type %u) not found", nm, ARG(2));
    }
    RET_STDCALL(3, e);
}
void imp_KERNEL32_LoadResource(CPU *cpu) { RET_STDCALL(2, res_data(ARG(1), NULL)); }
void imp_KERNEL32_LockResource(CPU *cpu) { RET_STDCALL(1, ARG(0)); }
void imp_KERNEL32_SizeofResource(CPU *cpu) { RET_STDCALL(2, ARG(1) ? RD32(ARG(1) + 4) : 0); }

/* ADVAPI32 registry emulation.  Keys and values live in memory and are saved to
 * <save dir>/registry.txt after every change, one value per line:
 *   KEY<TAB>NAME<TAB>TYPE<TAB>HEXDATA      (KEY like "HKCU\Software\...", NAME "" = default) */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include "win32.h"

#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_MORE_DATA 234
#define ERROR_NO_MORE_ITEMS 259

typedef struct {
    char *key, *name;
    uint32_t type, len;
    uint8_t *data;
} RegValue;

static RegValue *g_vals;
static int g_nvals, g_capvals;
static char **g_keys; /* keys that exist (created), even without values */
static int g_nkeys, g_capkeys;
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_loaded;

#define MAX_OPEN 256
static char *g_open[MAX_OPEN]; /* handle 0x9000 + 4*i -> key path */

static const char *root_name(uint32_t h) {
    switch (h) {
    case 0x80000000u: return "HKCR";
    case 0x80000001u: return "HKCU";
    case 0x80000002u: return "HKLM";
    case 0x80000003u: return "HKU";
    case 0x80000005u: return "HKCC";
    }
    if (h >= 0x9000 && h < 0x9000 + 4 * MAX_OPEN && g_open[(h - 0x9000) / 4]) return g_open[(h - 0x9000) / 4];
    return NULL;
}

static void reg_path(char *out, size_t n) { snprintf(out, n, "%s/registry.txt", platform_save_dir()); }

static void key_add(const char *k) {
    for (int i = 0; i < g_nkeys; i++)
        if (!strcasecmp(g_keys[i], k)) return;
    if (g_nkeys == g_capkeys) {
        g_capkeys = g_capkeys ? g_capkeys * 2 : 32;
        g_keys = realloc(g_keys, g_capkeys * sizeof *g_keys);
    }
    g_keys[g_nkeys++] = strdup(k);
}

static int key_exists(const char *k) {
    size_t n = strlen(k);
    for (int i = 0; i < g_nkeys; i++)
        if (!strncasecmp(g_keys[i], k, n) && (g_keys[i][n] == 0 || g_keys[i][n] == '\\')) return 1;
    for (int i = 0; i < g_nvals; i++)
        if (!strncasecmp(g_vals[i].key, k, n) && (g_vals[i].key[n] == 0 || g_vals[i].key[n] == '\\')) return 1;
    return !strchr(k, '\\'); /* the roots always exist */
}

static RegValue *val_find(const char *k, const char *name) {
    for (int i = 0; i < g_nvals; i++)
        if (!strcasecmp(g_vals[i].key, k) && !strcasecmp(g_vals[i].name, name)) return &g_vals[i];
    return NULL;
}

static void reg_save(void) {
    char path[1200];
    reg_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_nkeys; i++) fprintf(f, "%s\t\t-1\t\n", g_keys[i]);
    for (int i = 0; i < g_nvals; i++) {
        RegValue *v = &g_vals[i];
        fprintf(f, "%s\t%s\t%u\t", v->key, v->name, v->type);
        for (uint32_t k = 0; k < v->len; k++) fprintf(f, "%02x", v->data[k]);
        fputc('\n', f);
    }
    fclose(f);
}

static void val_set(const char *k, const char *name, uint32_t type, const uint8_t *data, uint32_t len) {
    RegValue *v = val_find(k, name);
    if (!v) {
        if (g_nvals == g_capvals) {
            g_capvals = g_capvals ? g_capvals * 2 : 64;
            g_vals = realloc(g_vals, g_capvals * sizeof *g_vals);
        }
        v = &g_vals[g_nvals++];
        v->key = strdup(k);
        v->name = strdup(name);
        v->data = NULL;
    }
    free(v->data);
    v->type = type;
    v->len = len;
    v->data = malloc(len ? len : 1);
    memcpy(v->data, data, len);
}

static void reg_load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    char path[1200], line[8192];
    reg_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        *t2 = 0;
        char *t3 = strchr(t2 + 1, '\t');
        if (!t3) continue;
        *t3 = 0;
        long type = strtol(t2 + 1, NULL, 10);
        if (type < 0) { key_add(line); continue; }
        size_t hl = strlen(t3 + 1) / 2;
        uint8_t *d = malloc(hl + 1);
        for (size_t i = 0; i < hl; i++) {
            unsigned b;
            sscanf(t3 + 1 + 2 * i, "%2x", &b);
            d[i] = (uint8_t)b;
        }
        val_set(line, t1 + 1, (uint32_t)type, d, (uint32_t)hl);
        free(d);
    }
    fclose(f);
}

static int full_key(uint32_t h, uint32_t sub, char *out, size_t n) {
    const char *r = root_name(h);
    if (!r) return -1;
    const char *s = sub ? gstr(sub) : "";
    while (*s == '\\') s++;
    if (*s) snprintf(out, n, "%s\\%s", r, s);
    else snprintf(out, n, "%s", r);
    size_t l = strlen(out);
    while (l && out[l - 1] == '\\') out[--l] = 0;
    return 0;
}

static uint32_t open_handle(const char *k) {
    for (int i = 0; i < MAX_OPEN; i++)
        if (!g_open[i]) {
            g_open[i] = strdup(k);
            return 0x9000u + 4u * (uint32_t)i;
        }
    return 0;
}

/* RegOpenKeyExA(hkey, sub, opts, sam, &result) */
void imp_ADVAPI32_RegOpenKeyExA(CPU *cpu) {
    char k[1024];
    pthread_mutex_lock(&g_reg_lock);
    reg_load();
    uint32_t r = ERROR_FILE_NOT_FOUND;
    if (!full_key(ARG(0), ARG(1), k, sizeof k) && key_exists(k)) {
        WR32(ARG(4), open_handle(k));
        r = ERROR_SUCCESS;
    }
    pthread_mutex_unlock(&g_reg_lock);
    TRACE("RegOpenKeyExA(%s) -> %u", k, r);
    RET_STDCALL(5, r);
}

/* RegCreateKeyExA(hkey, sub, res, class, opts, sam, sec, &result, &disposition) */
void imp_ADVAPI32_RegCreateKeyExA(CPU *cpu) {
    char k[1024];
    pthread_mutex_lock(&g_reg_lock);
    reg_load();
    uint32_t r = ERROR_FILE_NOT_FOUND;
    if (!full_key(ARG(0), ARG(1), k, sizeof k)) {
        int existed = key_exists(k);
        if (!existed) { key_add(k); reg_save(); }
        WR32(ARG(7), open_handle(k));
        if (ARG(8)) WR32(ARG(8), existed ? 2 : 1);
        r = ERROR_SUCCESS;
    }
    pthread_mutex_unlock(&g_reg_lock);
    RET_STDCALL(9, r);
}

void imp_ADVAPI32_RegCloseKey(CPU *cpu) {
    uint32_t h = ARG(0);
    pthread_mutex_lock(&g_reg_lock);
    if (h >= 0x9000 && h < 0x9000 + 4 * MAX_OPEN) {
        free(g_open[(h - 0x9000) / 4]);
        g_open[(h - 0x9000) / 4] = NULL;
    }
    pthread_mutex_unlock(&g_reg_lock);
    RET_STDCALL(1, ERROR_SUCCESS);
}

static uint32_t query(const char *k, const char *name, uint32_t ptype, uint32_t pdata, uint32_t plen) {
    RegValue *v = val_find(k, name);
    if (!v) return ERROR_FILE_NOT_FOUND;
    if (ptype) WR32(ptype, v->type);
    if (!plen) return ERROR_SUCCESS;
    uint32_t cap = RD32(plen);
    WR32(plen, v->len);
    if (!pdata) return ERROR_SUCCESS;
    if (cap < v->len) return ERROR_MORE_DATA;
    memcpy(GPTR(pdata), v->data, v->len);
    return ERROR_SUCCESS;
}

/* RegQueryValueExA(hkey, name, res, &type, data, &len) */
void imp_ADVAPI32_RegQueryValueExA(CPU *cpu) {
    char k[1024];
    pthread_mutex_lock(&g_reg_lock);
    reg_load();
    uint32_t r = ERROR_FILE_NOT_FOUND;
    const char *name = ARG(1) ? gstr(ARG(1)) : "";
    if (!full_key(ARG(0), 0, k, sizeof k)) r = query(k, name, ARG(3), ARG(4), ARG(5));
    pthread_mutex_unlock(&g_reg_lock);
    TRACE("RegQueryValueExA(%s, %s) -> %u", k, name, r);
    RET_STDCALL(6, r);
}

/* RegQueryValueA(hkey, sub, data, &len): the default value of a subkey */
void imp_ADVAPI32_RegQueryValueA(CPU *cpu) {
    char k[1024];
    pthread_mutex_lock(&g_reg_lock);
    reg_load();
    uint32_t r = ERROR_FILE_NOT_FOUND;
    if (!full_key(ARG(0), ARG(1), k, sizeof k)) r = query(k, "", 0, ARG(2), ARG(3));
    pthread_mutex_unlock(&g_reg_lock);
    RET_STDCALL(4, r);
}

/* RegSetValueExA(hkey, name, res, type, data, len) */
void imp_ADVAPI32_RegSetValueExA(CPU *cpu) {
    char k[1024];
    pthread_mutex_lock(&g_reg_lock);
    reg_load();
    uint32_t r = ERROR_FILE_NOT_FOUND;
    if (!full_key(ARG(0), 0, k, sizeof k)) {
        val_set(k, ARG(1) ? gstr(ARG(1)) : "", ARG(3), ARG(4) ? GPTR(ARG(4)) : (void *)"", ARG(4) ? ARG(5) : 0);
        reg_save();
        r = ERROR_SUCCESS;
    }
    pthread_mutex_unlock(&g_reg_lock);
    RET_STDCALL(6, r);
}

void imp_ADVAPI32_RegDeleteValueA(CPU *cpu) {
    char k[1024];
    pthread_mutex_lock(&g_reg_lock);
    reg_load();
    uint32_t r = ERROR_FILE_NOT_FOUND;
    if (!full_key(ARG(0), 0, k, sizeof k)) {
        RegValue *v = val_find(k, ARG(1) ? gstr(ARG(1)) : "");
        if (v) {
            free(v->key); free(v->name); free(v->data);
            *v = g_vals[--g_nvals];
            reg_save();
            r = ERROR_SUCCESS;
        }
    }
    pthread_mutex_unlock(&g_reg_lock);
    RET_STDCALL(2, r);
}

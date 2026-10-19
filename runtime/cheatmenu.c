/* In-game cheat menu: a list of the game's cheats to pick with touch, a controller
 * or the keyboard, plus a field to type any code.
 *
 * Opened by the CHEATS key (PORT_VK_CHEATS: pad Back, the touch CHEATS button, F12).
 * While it is open the game is frozen: the menu runs its own loop on the game thread
 * (from platform_pump_events), drawing over the last frame.  A chosen cheat is posted
 * to the game window as WM_COMMAND <id>, so the game handles it exactly as if the
 * code had been typed; it takes effect when the menu closes.
 *
 * The cheats come from the game's own cheat table in guest memory (game.ini [cheats]
 * map), so cheats that CrazyHook or a custom level registers show up too.  game.ini
 * gives labels and sections; a game hook (crazyhook_cheat_info) adds the text and the
 * on/off state the game scripts keep. */
#include <SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "platform.h"
#include "runtime.h"
#include "gamecfg.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb/stb_truetype.h"

extern const unsigned char g_font_sans[], g_font_sans_bold[];
uint32_t ddraw_coop_hwnd(void);
void wm_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void overlay_release_all(void);
void gamepad_release_all(void);
/* game scripts' view of a cheat (weak: no scripts -> nothing) */
__attribute__((weak)) int crazyhook_message_id(const char *code) { (void)code; return 0; }
__attribute__((weak)) int crazyhook_cheat_info(const char *code, char *text, int n, int *toggle) {
    (void)code; (void)text; (void)n; (void)toggle;
    return 0;
}

/* ---- the cheat list ------------------------------------------------------------------------ */
typedef struct {
    char code[40];      /* upper case, as typed */
    char label[96];
    int section;
    uint32_t id;        /* WM_COMMAND id */
    int toggle;         /* -1 unknown, 0 off, 1 on */
    int sent;           /* chosen while the menu is open */
    int option;         /* a port option (not a game cheat): handled here */
} Cheat;

#define OPT_WIDESCREEN 1
int widescreen_available(void);
int widescreen_enabled(void);
void widescreen_set(int on);
void widescreen_size(int *w, int *h);

#define MAX_CHEATS 512
#define MAX_SECTIONS 16
static Cheat g_cheats[MAX_CHEATS];
static int g_ncheats;
static char g_sections[MAX_SECTIONS][32];
static int g_nsections;           /* the last one is "Other" when used */

typedef struct { char code[40]; char label[96]; int section; uint32_t id; } Label;
static Label g_labels[MAX_CHEATS];
static int g_nlabels;

static void cfg_line(const char *key, const char *v, void *ctx) {
    (void)ctx;
    if (!strcasecmp(key, "section") && g_nsections < MAX_SECTIONS - 1) {
        snprintf(g_sections[g_nsections++], sizeof g_sections[0], "%s", v);
    } else if (!strcasecmp(key, "cheat") && g_nlabels < MAX_CHEATS) {
        Label *l = &g_labels[g_nlabels++];
        int n = 0;
        sscanf(v, "%39s %n", l->code, &n);
        for (char *c = l->code; *c; c++) *c = (char)toupper((unsigned char)*c);
        snprintf(l->label, sizeof l->label, "%s", v + n);
        l->section = g_nsections ? g_nsections - 1 : 0;
    }
}

/* the label for a cheat: by name, else by command id (aliases, spelling differences) */
static const Label *find_label(const char *code, uint32_t id) {
    for (int i = 0; i < g_nlabels; i++)
        if (!strcmp(g_labels[i].code, code)) return &g_labels[i];
    for (int i = 0; i < g_nlabels; i++)
        if (g_labels[i].id && g_labels[i].id == id) return &g_labels[i];
    return NULL;
}

static void add_cheat(const char *code, uint32_t id) {
    if (g_ncheats >= MAX_CHEATS) return;
    const Label *l = find_label(code, id);
    /* several names for one command (MPMONOLITH, MPLITH, MPLOGO...): one row, the labelled name */
    for (int i = 0; i < g_ncheats; i++) {
        Cheat *o = &g_cheats[i];
        if (!strcmp(o->code, code)) return;
        if (o->id == id) {
            if (l && !strcmp(l->code, code)) snprintf(o->code, sizeof o->code, "%s", code);
            else if (!l && strlen(code) < strlen(o->code)) snprintf(o->code, sizeof o->code, "%s", code);
            return;
        }
    }
    Cheat *c = &g_cheats[g_ncheats++];
    memset(c, 0, sizeof *c);
    snprintf(c->code, sizeof c->code, "%s", code);
    c->id = id;
    c->section = -1;
    c->toggle = -1;
    if (l) {
        snprintf(c->label, sizeof c->label, "%s", l->label);
        c->section = l->section;
    }
    char text[96] = "";
    int toggle = -1;
    if (crazyhook_cheat_info(code, text, sizeof text, &toggle)) {
        if (!c->label[0]) snprintf(c->label, sizeof c->label, "%s", text);
        c->toggle = toggle;
    }
}

static int section_order(const Cheat *c) { return c->section < 0 ? MAX_SECTIONS : c->section; }

static int cheat_cmp(const void *a, const void *b) {
    const Cheat *x = a, *y = b;
    int sx = section_order(x), sy = section_order(y);
    if (sx != sy) return sx - sy;
    if (x->option != y->option) return y->option - x->option; /* port options head their section */
    /* labelled cheats keep the game.ini order */
    const Label *lx = find_label(x->code, x->id), *ly = find_label(y->code, y->id);
    int ix = lx ? (int)(lx - g_labels) : g_nlabels, iy = ly ? (int)(ly - g_labels) : g_nlabels;
    if (ix != iy) return ix - iy;
    return strcmp(x->code, y->code);
}

/* The game's cheat table: an MFC CMapStringToPtr of encoded name -> {id, severity}. */
static void read_guest_table(void) {
    unsigned a = 0, field = 0, mapoff = 0;
    if (sscanf(cfg_str("cheats", "map", ""), "%x %x %x", &a, &field, &mapoff) != 3) return;
    int shift = cfg_int("cheats", "name_shift", 0);
    uint32_t root = RD32(a);
    if (!root) return;
    uint32_t mgr = RD32(root + field);
    if (!mgr) return;
    uint32_t map = mgr + mapoff;
    uint32_t table = RD32(map + 4), size = RD32(map + 8);
    if (!table || size > 4096) return;
    for (uint32_t b = 0; b < size; b++)
        for (uint32_t as = RD32(table + 4 * b), guard = 0; as && guard < 1000; as = RD32(as), guard++) {
            uint32_t key = RD32(as + 8), val = RD32(as + 12);
            if (!key || !val) continue;
            char code[40];
            int n = 0;
            for (; n < 39 && RD8(key + (uint32_t)n); n++) code[n] = (char)toupper((unsigned char)(RD8(key + (uint32_t)n) + shift));
            code[n] = 0;
            add_cheat(code, RD32(val));
        }
}

static void build_list(void) {
    g_ncheats = g_nlabels = g_nsections = 0;
    cfg_each_line("cheats", cfg_line, NULL);
    for (int i = 0; i < g_nlabels; i++) g_labels[i].id = (uint32_t)crazyhook_message_id(g_labels[i].code);
    read_guest_table();
    /* port options: rows like the cheats, in the Display section */
    if (widescreen_available() && g_ncheats < MAX_CHEATS) {
        Cheat *c = &g_cheats[g_ncheats++];
        memset(c, 0, sizeof *c);
        int w, h;
        widescreen_size(&w, &h);
        snprintf(c->code, sizeof c->code, "WIDESCREEN");
        snprintf(c->label, sizeof c->label, "Fill the screen: see more of the level (%dx%d)", w, h);
        c->option = OPT_WIDESCREEN;
        c->toggle = widescreen_enabled();
        c->section = -1;
        for (int k = 0; k < g_nsections; k++)
            if (!strcasecmp(g_sections[k], "Display")) c->section = k;
    }
    int other = 0;
    for (int i = 0; i < g_ncheats; i++)
        if (g_cheats[i].section < 0) other = 1;
    if (other && g_nsections < MAX_SECTIONS) {
        for (int i = 0; i < g_ncheats; i++)
            if (g_cheats[i].section < 0) g_cheats[i].section = g_nsections;
        snprintf(g_sections[g_nsections++], sizeof g_sections[0], "Other");
    }
    qsort(g_cheats, (size_t)g_ncheats, sizeof g_cheats[0], cheat_cmp);
    /* sections without any cheat (e.g. CrazyHook ones before a level loads) are hidden by
     * the view, which only shows non-empty ones */
}

/* ---- text ---------------------------------------------------------------------------------------- */
typedef struct {
    SDL_Texture *tex;
    stbtt_bakedchar ch[96];
    float px;
} Font;
static Font g_font, g_bold;
static float g_font_px;

static void font_make(SDL_Renderer *r, Font *f, const unsigned char *ttf, float px) {
    if (f->tex) SDL_DestroyTexture(f->tex);
    int W = 1024, H = 1024;
    unsigned char *a = calloc((size_t)W * H, 1);
    stbtt_BakeFontBitmap(ttf, 0, px, a, W, H, 32, 96, f->ch);
    uint32_t *rgba = malloc((size_t)W * H * 4);
    for (int i = 0; i < W * H; i++) rgba[i] = (uint32_t)a[i] << 24 | 0xffffff;
    f->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, W, H);
    SDL_UpdateTexture(f->tex, NULL, rgba, W * 4);
    SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND);
    free(a);
    free(rgba);
    f->px = px;
}

static float text_w(Font *f, const char *s) {
    float x = 0, y = 0;
    stbtt_aligned_quad q;
    for (; *s; s++)
        if (*s >= 32 && *s < 128) stbtt_GetBakedQuad(f->ch, 1024, 1024, *s - 32, &x, &y, &q, 1);
    return x;
}

/* draws s with its baseline at y; clipped at maxw (with "...") */
static void text(SDL_Renderer *r, Font *f, const char *s, float x, float y, SDL_Color c, float maxw) {
    SDL_SetTextureColorMod(f->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(f->tex, c.a);
    float x0 = x, dots = maxw > 0 ? text_w(f, "...") : 0;
    for (; *s; s++) {
        if (*s < 32 || *s >= 128) continue;
        stbtt_aligned_quad q;
        float nx = x, ny = y;
        stbtt_GetBakedQuad(f->ch, 1024, 1024, *s - 32, &nx, &ny, &q, 1);
        if (maxw > 0 && nx - x0 > maxw - dots && s[1]) {
            text(r, f, "...", x, y, c, 0);
            return;
        }
        SDL_Rect src = {(int)(q.s0 * 1024), (int)(q.t0 * 1024), (int)((q.s1 - q.s0) * 1024), (int)((q.t1 - q.t0) * 1024)};
        SDL_FRect dst = {q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0};
        SDL_RenderCopyF(r, f->tex, &src, &dst);
        x = nx;
    }
}

static void fill(SDL_Renderer *r, float x, float y, float w, float h, SDL_Color c) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRectF(r, &(SDL_FRect){x, y, w, h});
}

/* ---- state ----------------------------------------------------------------------------------- */
static int g_open, g_want_open;
static int g_tab;              /* index into the visible tabs; the last tab is "Type a code" */
static int g_sel;              /* selected row in the current tab */
static int g_focus_list = 1;   /* controller/keyboard focus: tabs (0) or list (1) */
static float g_scroll;         /* list scroll in rows */
static char g_typed[40];
static char g_status[128];
static int g_typing;           /* soft keyboard up */
static uint32_t g_status_at;

static int g_tabs[MAX_SECTIONS + 1], g_ntabs; /* visible sections, then -1 = type a code */

static void make_tabs(void) {
    g_ntabs = 0;
    for (int s = 0; s < g_nsections; s++)
        for (int i = 0; i < g_ncheats; i++)
            if (g_cheats[i].section == s) { g_tabs[g_ntabs++] = s; break; }
    g_tabs[g_ntabs++] = -1;
}

static int rows_in_tab(int tab, int *idx, int max) {
    int n = 0, s = g_tabs[tab];
    if (s < 0) return 0;
    for (int i = 0; i < g_ncheats && n < max; i++)
        if (g_cheats[i].section == s) idx[n++] = i;
    return n;
}

static void status(const char *fmt, const char *arg) {
    snprintf(g_status, sizeof g_status, fmt, arg);
    g_status_at = SDL_GetTicks();
}

static void send_cheat(Cheat *c) {
    if (c->option == OPT_WIDESCREEN) {
        c->toggle = !c->toggle;
        widescreen_set(c->toggle);
        status("%s", c->toggle ? "Widescreen on - used during gameplay" : "Widescreen off - the game's own 4:3");
        return;
    }
    uint32_t hwnd = ddraw_coop_hwnd();
    if (!hwnd) { status("%s", "The game window is not ready"); return; }
    wm_post(hwnd, 0x111, c->id, 0);
    c->sent++;
    if (c->toggle >= 0) c->toggle = !c->toggle;
    rt_log("cheat menu: %s (WM_COMMAND %x)", c->code, c->id);
    status("%s - takes effect when you close the menu", c->code);
}

/* On Android text input is the soft keyboard, which covers most of the menu: it comes up
 * only when the code field is tapped (as the hint says), not when the tab is selected. */
#ifdef __ANDROID__
#define TAP_TO_TYPE 1
#else
#define TAP_TO_TYPE 0
#endif

static void start_typing(int on);

/* a typed code: with or without the "MP" prefix, any case */
static void submit_typed(void) {
    char want[48];
    int n = 0;
    for (const char *s = g_typed; *s && n < 40; s++)
        if (isalnum((unsigned char)*s)) want[n++] = (char)toupper((unsigned char)*s);
    want[n] = 0;
    if (!n) return;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < g_ncheats; i++) {
            const char *c = g_cheats[i].code;
            if (!strcmp(c, want) || (pass && !strncmp(c, "MP", 2) && !strcmp(c + 2, want))) {
                send_cheat(&g_cheats[i]);
                g_typed[0] = 0;
                if (TAP_TO_TYPE) start_typing(0); /* the keyboard would hide the confirmation */
                return;
            }
        }
    status("Unknown code: %s", want);
}

static void start_typing(int on) {
    if (on && !g_typing) SDL_StartTextInput();
    if (!on && g_typing) SDL_StopTextInput();
    g_typing = on;
}

void cheatmenu_toggle(void) {
    if (g_open) g_open = 0;
    else g_want_open = 1;
}
int cheatmenu_is_open(void) { return g_open; }

/* ---- layout ---------------------------------------------------------------------------------- */
typedef struct { float x, y, w, h; } RectF;
static RectF g_panel, g_tabs_r, g_list_r, g_close_r, g_field_r, g_send_r;
static float g_row_h, g_tab_h;

static void layout(int w, int h) {
    float pw = w * 0.94f, ph = h * 0.92f;
    if (pw > h * 1.9f) pw = h * 1.9f;
    g_panel = (RectF){(w - pw) / 2, (h - ph) / 2, pw, ph};
    float px = h / 26.0f;
    if (px < 14) px = 14;
    g_row_h = px * 2.6f;
    g_tab_h = px * 2.3f;
    float head = px * 2.8f;
    float tw = pw * 0.26f;
    g_tabs_r = (RectF){g_panel.x + px * 0.5f, g_panel.y + head, tw, ph - head - px * 2.2f};
    g_list_r = (RectF){g_tabs_r.x + tw + px * 0.5f, g_tabs_r.y, pw - tw - px * 1.5f, g_tabs_r.h};
    g_close_r = (RectF){g_panel.x + pw - head, g_panel.y, head, head};
    g_field_r = (RectF){g_list_r.x, g_list_r.y + px, g_list_r.w * 0.7f, g_row_h};
    g_send_r = (RectF){g_field_r.x + g_field_r.w + px * 0.5f, g_field_r.y, g_list_r.w * 0.3f - px * 0.5f, g_row_h};
}

static int in(RectF r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static int visible_rows(void) { int n = (int)(g_list_r.h / g_row_h); return n < 1 ? 1 : n; }

static void clamp_sel(void) {
    int idx[MAX_CHEATS];
    int n = rows_in_tab(g_tab, idx, MAX_CHEATS);
    if (g_sel >= n) g_sel = n - 1;
    if (g_sel < 0) g_sel = 0;
    int vis = visible_rows();
    if (g_sel < g_scroll) g_scroll = (float)g_sel;
    if (g_sel >= g_scroll + vis) g_scroll = (float)(g_sel - vis + 1);
    float maxs = (float)(n - vis);
    if (g_scroll > maxs) g_scroll = maxs;
    if (g_scroll < 0) g_scroll = 0;
}

static void set_tab(int t) {
    if (t < 0) t = g_ntabs - 1;
    if (t >= g_ntabs) t = 0;
    if (t != g_tab) { g_sel = 0; g_scroll = 0; }
    g_tab = t;
    start_typing(g_tabs[g_tab] < 0 && !TAP_TO_TYPE);
}

/* ---- drawing ---------------------------------------------------------------------------------- */
static const SDL_Color WHITE = {255, 255, 255, 255}, DIM = {190, 180, 160, 255}, GOLD = {255, 205, 90, 255},
                       GREEN = {120, 230, 120, 255}, RED = {240, 120, 100, 255};

static void draw(SDL_Renderer *r, int w, int h) {
    float px = h / 26.0f;
    if (px < 14) px = 14;
    if (px != g_font_px) {
        font_make(r, &g_font, g_font_sans, px);
        font_make(r, &g_bold, g_font_sans_bold, px);
        g_font_px = px;
    }
    layout(w, h);
    fill(r, 0, 0, (float)w, (float)h, (SDL_Color){0, 0, 0, 150});
    fill(r, g_panel.x, g_panel.y, g_panel.w, g_panel.h, (SDL_Color){24, 18, 12, 235});
    fill(r, g_panel.x, g_panel.y, g_panel.w, px * 2.8f, (SDL_Color){70, 45, 20, 255});
    text(r, &g_bold, "CHEATS", g_panel.x + px, g_panel.y + px * 1.85f, GOLD, 0);
    const char *hint = g_typing ? "Type a cheat code, then Enter" :
                       "Tap or A: use    B: close    LB/RB: section";
    text(r, &g_font, hint, g_panel.x + px * 6.5f, g_panel.y + px * 1.85f, DIM, g_panel.w - px * 10);
    fill(r, g_close_r.x + px * 0.4f, g_close_r.y + px * 0.4f, g_close_r.w - px * 0.8f, g_close_r.h - px * 0.8f,
         (SDL_Color){120, 40, 30, 255});
    text(r, &g_bold, "X", g_close_r.x + g_close_r.w / 2 - text_w(&g_bold, "X") / 2, g_close_r.y + px * 1.85f, WHITE, 0);

    /* section tabs */
    for (int t = 0; t < g_ntabs; t++) {
        float y = g_tabs_r.y + t * g_tab_h;
        if (y + g_tab_h > g_tabs_r.y + g_tabs_r.h) break;
        int cur = t == g_tab;
        if (cur) fill(r, g_tabs_r.x, y, g_tabs_r.w, g_tab_h - 4, (SDL_Color){110, 75, 30, 255});
        if (cur && !g_focus_list && !g_typing) fill(r, g_tabs_r.x, y, 5, g_tab_h - 4, GOLD);
        const char *name = g_tabs[t] < 0 ? "Type a code" : g_sections[g_tabs[t]];
        text(r, cur ? &g_bold : &g_font, name, g_tabs_r.x + px * 0.7f, y + g_tab_h * 0.62f, cur ? WHITE : DIM,
             g_tabs_r.w - px);
    }

    if (g_tabs[g_tab] < 0) {
        /* typed code */
        fill(r, g_field_r.x, g_field_r.y, g_field_r.w, g_field_r.h, (SDL_Color){10, 8, 6, 255});
        char shown[64];
        snprintf(shown, sizeof shown, "%s%s", g_typed, (SDL_GetTicks() / 500) % 2 ? "_" : "");
        text(r, &g_bold, shown, g_field_r.x + px * 0.6f, g_field_r.y + g_row_h * 0.65f, WHITE, g_field_r.w - px);
        fill(r, g_send_r.x, g_send_r.y, g_send_r.w, g_send_r.h, (SDL_Color){60, 110, 50, 255});
        text(r, &g_bold, "Use", g_send_r.x + g_send_r.w / 2 - text_w(&g_bold, "Use") / 2, g_send_r.y + g_row_h * 0.65f,
             WHITE, 0);
        text(r, &g_font, "Any code the game knows, with or without \"mp\" (custom levels can add their own).",
             g_list_r.x, g_field_r.y + g_row_h + px * 1.8f, DIM, g_list_r.w);
        text(r, &g_font, "Tap the field to bring up the keyboard.", g_list_r.x, g_field_r.y + g_row_h + px * 3.4f, DIM,
             g_list_r.w);
    } else {
        int idx[MAX_CHEATS];
        int n = rows_in_tab(g_tab, idx, MAX_CHEATS);
        int vis = visible_rows();
        SDL_RenderSetClipRect(r, &(SDL_Rect){(int)g_list_r.x, (int)g_list_r.y, (int)g_list_r.w, (int)g_list_r.h});
        for (int k = (int)g_scroll; k < n && k < (int)g_scroll + vis + 1; k++) {
            Cheat *c = &g_cheats[idx[k]];
            float y = g_list_r.y + (k - g_scroll) * g_row_h;
            int cur = k == g_sel;
            fill(r, g_list_r.x, y, g_list_r.w, g_row_h - 4,
                 cur ? (SDL_Color){110, 75, 30, 255} : (SDL_Color){45, 34, 22, 255});
            if (cur && g_focus_list) fill(r, g_list_r.x, y, 5, g_row_h - 4, GOLD);
            float bx = g_list_r.x + g_list_r.w - px * 0.6f;
            if (c->toggle >= 0) {
                const char *st = c->toggle ? "ON" : "OFF";
                bx -= text_w(&g_bold, st);
                text(r, &g_bold, st, bx, y + g_row_h * 0.6f, c->toggle ? GREEN : DIM, 0);
                bx -= px;
            }
            if (c->sent) {
                const char *st = "sent";
                bx -= text_w(&g_font, st);
                text(r, &g_font, st, bx, y + g_row_h * 0.6f, GOLD, 0);
                bx -= px;
            }
            float cw = px * 9.5f;
            text(r, &g_bold, c->code, g_list_r.x + px * 0.7f, y + g_row_h * 0.6f, GOLD, cw - px);
            text(r, &g_font, c->label[0] ? c->label : "", g_list_r.x + px * 0.7f + cw, y + g_row_h * 0.6f, WHITE,
                 bx - (g_list_r.x + px * 0.7f + cw) - px * 0.5f);
        }
        SDL_RenderSetClipRect(r, NULL);
        if (n > vis) { /* scroll bar */
            float bh = g_list_r.h * vis / n, by = g_list_r.y + (g_list_r.h - bh) * g_scroll / (n - vis);
            fill(r, g_list_r.x + g_list_r.w + px * 0.15f, by, px * 0.3f, bh, (SDL_Color){200, 160, 90, 200});
        }
    }
    /* footer: status or the game's note */
    float fy = g_panel.y + g_panel.h - px * 0.8f;
    if (g_status[0] && SDL_GetTicks() - g_status_at < 6000)
        text(r, &g_bold, g_status, g_panel.x + px, fy, strncmp(g_status, "Unknown", 7) ? GREEN : RED, g_panel.w - px * 2);
    else
        text(r, &g_font, cfg_str("cheats", "note", ""), g_panel.x + px, fy, DIM, g_panel.w - px * 2);
}

/* ---- input ------------------------------------------------------------------------------------- */
static void activate(void) {
    if (g_tabs[g_tab] < 0) { submit_typed(); return; }
    int idx[MAX_CHEATS];
    int n = rows_in_tab(g_tab, idx, MAX_CHEATS);
    if (g_sel >= 0 && g_sel < n) send_cheat(&g_cheats[idx[g_sel]]);
}

static void move(int d) {
    if (!g_focus_list || g_tabs[g_tab] < 0) set_tab(g_tab + d);
    else { g_sel += d; clamp_sel(); }
}

static void key(SDL_Keycode k) {
    switch (k) {
    case SDLK_ESCAPE: case SDLK_AC_BACK: case SDLK_F12: g_open = 0; break;
    case SDLK_UP: move(-1); break;
    case SDLK_DOWN: move(1); break;
    case SDLK_PAGEUP: g_sel -= visible_rows(); clamp_sel(); break;
    case SDLK_PAGEDOWN: g_sel += visible_rows(); clamp_sel(); break;
    case SDLK_LEFT: g_focus_list = 0; break;
    case SDLK_RIGHT: g_focus_list = 1; break;
    case SDLK_TAB: set_tab(g_tab + 1); break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
        if (g_tabs[g_tab] < 0 || g_focus_list) activate(); else g_focus_list = 1;
        break;
    case SDLK_BACKSPACE:
        if (g_tabs[g_tab] < 0 && g_typed[0]) g_typed[strlen(g_typed) - 1] = 0;
        break;
    }
}

static int g_pad_axis[2]; /* repeat state of the left stick (y) */
static uint32_t g_repeat_at;
static int g_repeat_dir;

static void pad_button(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK: case SDL_CONTROLLER_BUTTON_START: g_open = 0; break;
    case SDL_CONTROLLER_BUTTON_A: if (g_tabs[g_tab] < 0 || g_focus_list) activate(); else g_focus_list = 1; break;
    case SDL_CONTROLLER_BUTTON_Y: set_tab(g_ntabs - 1); break;
    case SDL_CONTROLLER_BUTTON_X: if (g_tabs[g_tab] < 0 && g_typed[0]) g_typed[strlen(g_typed) - 1] = 0; break;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: move(-1); g_repeat_dir = -1; g_repeat_at = SDL_GetTicks() + 400; break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move(1); g_repeat_dir = 1; g_repeat_at = SDL_GetTicks() + 400; break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: g_focus_list = 0; break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: g_focus_list = 1; break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: set_tab(g_tab - 1); break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: set_tab(g_tab + 1); break;
    }
}

/* touch / mouse: tap selects and uses, a drag scrolls the list */
static int g_touch_down, g_touch_moved;
static float g_touch_x, g_touch_y, g_touch_scroll0;

static void pointer(int type, float x, float y) {
    if (type == 0) {
        g_touch_down = 1;
        g_touch_moved = 0;
        g_touch_x = x;
        g_touch_y = y;
        g_touch_scroll0 = g_scroll;
        return;
    }
    if (!g_touch_down) return;
    if (type == 1) {
        if (fabsf(y - g_touch_y) > g_row_h * 0.3f) g_touch_moved = 1;
        if (g_touch_moved && in(g_list_r, g_touch_x, g_touch_y)) {
            g_scroll = g_touch_scroll0 - (y - g_touch_y) / g_row_h;
            int idx[MAX_CHEATS];
            int n = rows_in_tab(g_tab, idx, MAX_CHEATS);
            float maxs = (float)(n - visible_rows());
            if (g_scroll > maxs) g_scroll = maxs;
            if (g_scroll < 0) g_scroll = 0;
        }
        return;
    }
    g_touch_down = 0;
    if (g_touch_moved) return;
    if (in(g_close_r, x, y)) { g_open = 0; return; }
    if (in(g_tabs_r, x, y)) {
        int t = (int)((y - g_tabs_r.y) / g_tab_h);
        if (t >= 0 && t < g_ntabs) { set_tab(t); g_focus_list = 0; }
        return;
    }
    if (g_tabs[g_tab] < 0) {
        if (in(g_field_r, x, y)) { g_typing = 0; start_typing(1); }
        if (in(g_send_r, x, y)) submit_typed();
        return;
    }
    if (in(g_list_r, x, y)) {
        int k = (int)(g_scroll + (y - g_list_r.y) / g_row_h);
        int idx[MAX_CHEATS];
        if (k >= 0 && k < rows_in_tab(g_tab, idx, MAX_CHEATS)) {
            g_sel = k;
            g_focus_list = 1;
            activate();
        }
    }
}

static void handle(const SDL_Event *e, int w, int h) {
    switch (e->type) {
    case SDL_KEYDOWN:
        if (g_tabs[g_tab] >= 0 && !e->key.repeat && e->key.keysym.sym >= SDLK_a && e->key.keysym.sym <= SDLK_z &&
            !(e->key.keysym.mod & (KMOD_CTRL | KMOD_ALT))) {
            /* typing letters on a keyboard jumps to the code field */
            set_tab(g_ntabs - 1);
        }
        key(e->key.keysym.sym);
        break;
    case SDL_TEXTINPUT:
        if (g_tabs[g_tab] < 0)
            for (const char *s = e->text.text; *s; s++)
                if (isalnum((unsigned char)*s) && strlen(g_typed) < sizeof g_typed - 1) {
                    size_t l = strlen(g_typed);
                    g_typed[l] = (char)toupper((unsigned char)*s);
                    g_typed[l + 1] = 0;
                }
        break;
    case SDL_CONTROLLERBUTTONDOWN: pad_button(e->cbutton.button); break;
    case SDL_CONTROLLERBUTTONUP:
        if (e->cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP || e->cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
            g_repeat_dir = 0;
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
            int d = e->caxis.value < -16000 ? -1 : e->caxis.value > 16000 ? 1 : 0;
            if (d != g_pad_axis[1]) {
                g_pad_axis[1] = d;
                if (d) move(d);
                g_repeat_dir = d;
                g_repeat_at = SDL_GetTicks() + 400;
            }
        } else if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
            int d = e->caxis.value < -16000 ? -1 : e->caxis.value > 16000 ? 1 : 0;
            if (d != g_pad_axis[0]) {
                g_pad_axis[0] = d;
                if (d) g_focus_list = d > 0;
            }
        }
        break;
    case SDL_FINGERDOWN: pointer(0, e->tfinger.x * w, e->tfinger.y * h); break;
    case SDL_FINGERMOTION: pointer(1, e->tfinger.x * w, e->tfinger.y * h); break;
    case SDL_FINGERUP: pointer(2, e->tfinger.x * w, e->tfinger.y * h); break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION: {
        if (e->type == SDL_MOUSEMOTION ? e->motion.which == SDL_TOUCH_MOUSEID : e->button.which == SDL_TOUCH_MOUSEID) break;
        int ww, wh;
        SDL_GetWindowSize(SDL_GetWindowFromID(e->button.windowID), &ww, &wh);
        float sx = ww ? (float)w / ww : 1, sy = wh ? (float)h / wh : 1;
        if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) pointer(0, e->button.x * sx, e->button.y * sy);
        else if (e->type == SDL_MOUSEBUTTONUP && e->button.button == SDL_BUTTON_LEFT) pointer(2, e->button.x * sx, e->button.y * sy);
        else if (e->type == SDL_MOUSEMOTION) pointer(1, e->motion.x * sx, e->motion.y * sy);
        break;
    }
    case SDL_MOUSEWHEEL:
        g_scroll -= e->wheel.y;
        clamp_sel();
        break;
    }
}

/* ---- the modal loop --------------------------------------------------------------------------- */
void platform_redraw_last_frame(void (*over)(SDL_Renderer *, int, int));
int platform_route_event(const SDL_Event *e); /* app/quit events still go to the platform */

/* returns 1 when the menu consumed e */
int cheatmenu_handle_event(const SDL_Event *e, int w, int h) {
    if (!g_open) return 0;
    switch (e->type) {
    case SDL_QUIT: case SDL_APP_WILLENTERBACKGROUND: case SDL_APP_DIDENTERBACKGROUND:
    case SDL_APP_DIDENTERFOREGROUND: case SDL_WINDOWEVENT: case SDL_CONTROLLERDEVICEADDED:
    case SDL_CONTROLLERDEVICEREMOVED:
        return 0;
    }
    handle(e, w, h);
    return 1;
}

void cheatmenu_run(void) {
    if (!g_want_open) return;
    g_want_open = 0;
    /* nothing the game sees stays pressed while it is frozen */
    overlay_release_all();
    gamepad_release_all();
    for (int s = 0; s < INPUT_SRC_COUNT; s++) input_clear_source(s);
    int was_typing = SDL_IsTextInputActive();
    build_list();
    for (int i = 0; i < g_ncheats; i++) g_cheats[i].sent = 0;
    make_tabs();
    if (g_tab >= g_ntabs) g_tab = 0;
    g_typing = 0;
    start_typing(g_tabs[g_tab] < 0 && !TAP_TO_TYPE);
    g_status[0] = 0;
    g_open = 1;
    rt_log("cheat menu: %d cheats", g_ncheats);
    for (int i = 0; i < g_ncheats; i++)
        if (!g_cheats[i].label[0]) rt_log("  unlabelled: %s (%x)", g_cheats[i].code, g_cheats[i].id);
    while (g_open && !platform_quit_requested()) {
        void platform_debug_tick(void);
        platform_debug_tick();
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 30)) {
            do platform_route_event(&e);
            while (SDL_PollEvent(&e));
        }
        if (g_repeat_dir && SDL_GetTicks() >= g_repeat_at) {
            move(g_repeat_dir);
            g_repeat_at = SDL_GetTicks() + 90;
        }
        platform_redraw_last_frame(draw);
    }
    g_open = 0;
    g_repeat_dir = 0;
    start_typing(0);
    if (was_typing) SDL_StartTextInput();
    else SDL_StopTextInput();
    for (int s = 0; s < INPUT_SRC_COUNT; s++) input_clear_source(s);
}

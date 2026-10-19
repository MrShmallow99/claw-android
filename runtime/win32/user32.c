/* USER32 emulation: a small window manager.
 *
 * Windows (classes, styles, parents/owners, z-order, props, extra bytes) live in
 * host tables; their window procedures are guest code (or host thunks for the
 * built-in classes in user32_dlg.c).  Messages go through one queue pumped by the
 * game's main thread; WM_PAINT and WM_TIMER are synthesised when the queue is
 * empty, as on Windows.  Windows draw with GDI into a 32 bpp "desktop" canvas;
 * the compositor puts the visible top-level windows over the DirectDraw frame
 * (dialogs over a fullscreen game, like GDI over the primary surface on Win9x). */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <pthread.h>
#include "wnd.h"
#include "gamecfg.h"

uint32_t g_wndproc;          /* kept for the template API: wndproc of the first window */
int g_window_created;
int g_client_w = 640, g_client_h = 480;

static Wnd g_wnd[MAX_WND];
static int g_zcounter_top, g_zcounter_bottom;
static int g_z[MAX_WND];
static uint32_t g_focus, g_active, g_capture;
static int g_app_active = 1;
static int g_desktop_dirty;
static uint32_t g_last_present;

uint32_t ddraw_coop_hwnd(void);
int ddraw_render_frame(uint32_t *dst, int w, int h);

/* ---- desktop canvas ------------------------------------------------------------- */
static Canvas g_desktop;
static uint32_t *g_desktop_px;

Canvas *desktop_canvas(void) {
    if (!g_desktop_px) desktop_resize(640, 480);
    return &g_desktop;
}

void desktop_resize(int w, int h) {
    if (g_desktop_px && g_desktop.w == w && g_desktop.h == h) return;
    free(g_desktop_px);
    g_desktop_px = calloc((size_t)w * h, 4);
    g_desktop = (Canvas){(uint8_t *)g_desktop_px, w, h, w * 4, 32, 0, NULL, 0};
    g_client_w = w;
    g_client_h = h;
    for (int i = 0; i < MAX_WND; i++)
        if (g_wnd[i].used) wm_invalidate(g_wnd[i].hwnd, NULL, 1);
}

/* ---- window table ---------------------------------------------------------------- */
Wnd *wnd_get(uint32_t h) {
    if (h < WND_BASE || h >= WND_BASE + MAX_WND * 4 || (h & 3)) return NULL;
    Wnd *w = &g_wnd[(h - WND_BASE) / 4];
    return w->used ? w : NULL;
}

Wnd *wnd_by_index(int i) { return i >= 0 && i < MAX_WND && g_wnd[i].used ? &g_wnd[i] : NULL; }

static int widx(Wnd *w) { return (int)(w - g_wnd); }

static int is_toplevel(Wnd *w) { return !(w->style & WS_CHILD); }

int wm_is_child_of(uint32_t h, uint32_t anc) {
    Wnd *w = wnd_get(h);
    while (w && (w->style & WS_CHILD)) {
        if (w->parent == anc) return 1;
        w = wnd_get(w->parent);
    }
    return 0;
}

/* visible if it and every ancestor are visible */
static int really_visible(Wnd *w) {
    while (w) {
        if (!(w->style & WS_VISIBLE)) return 0;
        if (!(w->style & WS_CHILD)) return 1;
        w = wnd_get(w->parent);
    }
    return 0;
}

/* siblings ordered by z (lowest first = topmost) */
static uint32_t sibling_scan(uint32_t parent, int child_list, int after_z, int before_z, int want_last) {
    uint32_t best = 0;
    int bz = want_last ? -0x7fffffff : 0x7fffffff;
    for (int i = 0; i < MAX_WND; i++) {
        Wnd *w = &g_wnd[i];
        if (!w->used || w->destroying) continue;
        if (child_list ? (!(w->style & WS_CHILD) || w->parent != parent) : (w->style & WS_CHILD)) continue;
        int z = g_z[i];
        if (z <= after_z || z >= before_z) continue;
        if (want_last ? z > bz : z < bz) { bz = z; best = w->hwnd; }
    }
    return best;
}

uint32_t wm_first_child(uint32_t parent) { return sibling_scan(parent, 1, -0x7fffffff, 0x7fffffff, 0); }

uint32_t wm_next_sibling(uint32_t h) {
    Wnd *w = wnd_get(h);
    if (!w) return 0;
    return sibling_scan(w->parent, (w->style & WS_CHILD) != 0, g_z[widx(w)], 0x7fffffff, 0);
}

uint32_t wm_prev_sibling(uint32_t h) {
    Wnd *w = wnd_get(h);
    if (!w) return 0;
    return sibling_scan(w->parent, (w->style & WS_CHILD) != 0, -0x7fffffff, g_z[widx(w)], 1);
}

/* ---- classes ------------------------------------------------------------------------- */
#define MAX_CLASS 128
static WndClass g_cls[MAX_CLASS];
static int g_ncls;

WndClass *wm_find_class(const char *name, uint32_t atom) {
    for (int i = 0; i < g_ncls; i++)
        if ((atom && g_cls[i].atom == atom) || (name && !strcasecmp(g_cls[i].name, name))) return &g_cls[i];
    return NULL;
}

WndClass *wm_register_class(const char *name, uint32_t style, uint32_t wndproc, uint32_t wnd_extra, uint32_t hbrush,
                           uint32_t hcursor) {
    WndClass *c = wm_find_class(name, 0);
    if (!c) {
        if (g_ncls >= MAX_CLASS) return NULL;
        c = &g_cls[g_ncls++];
        memset(c, 0, sizeof *c);
        snprintf(c->name, sizeof c->name, "%s", name);
        c->atom = win32_atom_add(name);
    }
    c->style = style;
    c->wndproc = wndproc;
    c->wnd_extra = wnd_extra;
    c->hbrush = hbrush;
    c->hcursor = hcursor;
    return c;
}

static void classes_init(void) {
    static int done;
    if (done) return;
    done = 1;
    ctl_register_classes();
}

/* RegisterClassA(WNDCLASSA*) */
void imp_USER32_RegisterClassA(CPU *cpu) {
    classes_init();
    uint32_t wc = ARG(0);
    const char *name;
    char tmp[16];
    uint32_t nm = RD32(wc + 36);
    if (nm < 0x10000) { snprintf(tmp, sizeof tmp, "#%u", nm); name = tmp; }
    else name = gstr(nm);
    WndClass *c = wm_register_class(name, RD32(wc), RD32(wc + 4), RD32(wc + 12), RD32(wc + 28), RD32(wc + 24));
    if (!c) RET_STDCALL(1, 0);
    c->cls_extra = RD32(wc + 8);
    c->hinst = RD32(wc + 16);
    c->hicon = RD32(wc + 20);
    c->menuname = RD32(wc + 32);
    if (!g_wndproc) g_wndproc = c->wndproc;
    TRACE("RegisterClassA(%s) wndproc=%08x", name, c->wndproc);
    RET_STDCALL(1, c->atom);
}

/* GetClassInfoA(hinst, name, WNDCLASSA*) */
void imp_USER32_GetClassInfoA(CPU *cpu) {
    classes_init();
    uint32_t nm = ARG(1), out = ARG(2);
    WndClass *c = nm < 0x10000 ? wm_find_class(NULL, nm) : wm_find_class(gstr(nm), 0);
    if (!c) RET_STDCALL(3, 0);
    WR32(out, c->style);
    WR32(out + 4, c->wndproc);
    WR32(out + 8, c->cls_extra);
    WR32(out + 12, c->wnd_extra);
    WR32(out + 16, c->hinst);
    WR32(out + 20, c->hicon);
    WR32(out + 24, c->hcursor);
    WR32(out + 28, c->hbrush);
    WR32(out + 32, c->menuname);
    WR32(out + 36, nm);
    RET_STDCALL(3, c->atom);
}

void imp_USER32_GetClassLongA(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    int32_t idx = (int32_t)ARG(1);
    uint32_t v = 0;
    if (w && w->cls) {
        switch (idx) {
        case -8: v = w->cls->menuname; break;
        case -10: v = w->cls->hbrush; break;
        case -12: v = w->cls->hcursor; break;
        case -14: v = w->cls->hicon; break;
        case -16: v = w->cls->hinst; break;
        case -18: v = w->cls->wnd_extra; break;
        case -20: v = w->cls->cls_extra; break;
        case -24: v = w->cls->wndproc; break;
        case -26: v = w->cls->style; break;
        case -32: v = w->cls->atom; break;
        default: if (idx >= 0 && idx < 64) v = w->cls->extra[idx / 4];
        }
    }
    RET_STDCALL(2, v);
}

void imp_USER32_GetClassNameA(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2);
    if (!w || !n) RET_STDCALL(3, 0);
    size_t l = strlen(w->cls->name);
    if (l >= n) l = n - 1;
    memcpy(GPTR(buf), w->cls->name, l);
    WR8(buf + l, 0);
    RET_STDCALL(3, l);
}

void imp_USER32_RegisterWindowMessageA(CPU *cpu) { RET_STDCALL(1, win32_atom_add(gstr(ARG(0)))); }

/* ---- hooks ------------------------------------------------------------------------------ */
#define MAX_HOOK 32
static struct { int used, id; uint32_t proc; } g_hooks[MAX_HOOK];
static int g_hook_seq[MAX_HOOK], g_hook_counter;

void imp_USER32_SetWindowsHookExA(CPU *cpu) {
    for (int i = 0; i < MAX_HOOK; i++)
        if (!g_hooks[i].used) {
            g_hooks[i].used = 1;
            g_hooks[i].id = (int)ARG(0);
            g_hooks[i].proc = ARG(1);
            g_hook_seq[i] = ++g_hook_counter;
            TRACE("SetWindowsHookExA(%d, %08x) -> %x", (int)ARG(0), ARG(1), 0x5000 + i * 4);
            RET_STDCALL(4, 0x5000u + (uint32_t)i * 4);
        }
    RET_STDCALL(4, 0);
}

void imp_USER32_UnhookWindowsHookEx(CPU *cpu) {
    uint32_t h = ARG(0);
    if (h >= 0x5000 && h < 0x5000 + MAX_HOOK * 4) g_hooks[(h - 0x5000) / 4].used = 0;
    RET_STDCALL(1, 1);
}

/* the most recently installed hook of type id older than seq (0 = newest) */
static int hook_next(int id, int below_seq) {
    int best = -1, bs = 0;
    for (int i = 0; i < MAX_HOOK; i++)
        if (g_hooks[i].used && g_hooks[i].id == id && (!below_seq || g_hook_seq[i] < below_seq) && g_hook_seq[i] > bs) {
            bs = g_hook_seq[i];
            best = i;
        }
    return best;
}

static uint32_t call_hook(CPU *cpu, int id, uint32_t code, uint32_t wp, uint32_t lp) {
    int i = hook_next(id, 0);
    if (i < 0) return 0;
    uint32_t args[3] = {code, wp, lp};
    return guest_call(cpu, g_hooks[i].proc, 3, args);
}

/* CallNextHookEx(hhk, code, wp, lp) */
void imp_USER32_CallNextHookEx(CPU *cpu) {
    uint32_t h = ARG(0);
    uint32_t r = 0;
    if (h >= 0x5000 && h < 0x5000 + MAX_HOOK * 4) {
        int cur = (int)(h - 0x5000) / 4;
        int nx = hook_next(g_hooks[cur].id, g_hook_seq[cur]);
        if (nx >= 0) {
            uint32_t args[3] = {ARG(1), ARG(2), ARG(3)};
            r = guest_call(cpu, g_hooks[nx].proc, 3, args);
        }
    }
    RET_STDCALL(4, r);
}

/* ---- sending messages ---------------------------------------------------------------------- */
uint32_t wm_send(CPU *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Wnd *w = wnd_get(hwnd);
    if (!w || !w->wndproc) return 0;
    static int trace_msgs = -1;
    if (trace_msgs < 0) trace_msgs = getenv("PORT_TRACE_MSG") != NULL;
    if (trace_msgs && msg != WM_MOUSEMOVE) rt_log("msg %x -> %x (%x, %x)", msg, hwnd, wp, lp);
    uint32_t args[4] = {hwnd, msg, wp, lp};
    return guest_call(cpu, w->wndproc, 4, args);
}

/* template API */
uint32_t window_send(CPU *cpu, uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    return wm_send(cpu, hwnd, msg, wparam, lparam);
}

uint32_t wm_notify_parent(CPU *cpu, Wnd *w, uint32_t code) {
    if (!w->parent) return 0;
    return wm_send(cpu, w->parent, WM_COMMAND, (w->id & 0xffff) | (code << 16), w->hwnd);
}

void imp_USER32_SendMessageA(CPU *cpu) {
    uint32_t h = ARG(0), msg = ARG(1), wp = ARG(2), lp = ARG(3);
    uint32_t r = 0;
    if (h == 0xffff) {
        for (int i = 0; i < MAX_WND; i++)
            if (g_wnd[i].used && is_toplevel(&g_wnd[i])) wm_send(cpu, g_wnd[i].hwnd, msg, wp, lp);
    } else {
        r = wm_send(cpu, h, msg, wp, lp);
    }
    RET_STDCALL(4, r);
}

void imp_USER32_CallWindowProcA(CPU *cpu) {
    uint32_t proc = ARG(0);
    uint32_t args[4] = {ARG(1), ARG(2), ARG(3), ARG(4)};
    uint32_t r = proc ? guest_call(cpu, proc, 4, args) : 0;
    RET_STDCALL(5, r);
}

void imp_USER32_SendDlgItemMessageA(CPU *cpu) {
    uint32_t c = 0;
    for (uint32_t ch = wm_first_child(ARG(0)); ch; ch = wm_next_sibling(ch))
        if (wnd_get(ch)->id == ARG(1)) { c = ch; break; }
    uint32_t r = c ? wm_send(cpu, c, ARG(2), ARG(3), ARG(4)) : 0;
    RET_STDCALL(5, r);
}

/* ---- message queue ------------------------------------------------------------------------- */
#define QSIZE 1024
static WinMsg g_q[QSIZE];
static int g_qn;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;
static int g_quit_posted;
static uint32_t g_quit_code;
static int g_mouse_x, g_mouse_y;
static uint32_t g_msg_time, g_msg_pos;

void wm_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&g_qlock);
    if (msg == WM_MOUSEMOVE && g_qn && g_q[g_qn - 1].message == WM_MOUSEMOVE && g_q[g_qn - 1].hwnd == hwnd) {
        g_q[g_qn - 1].wparam = wp;
        g_q[g_qn - 1].lparam = lp;
        pthread_mutex_unlock(&g_qlock);
        return;
    }
    if (g_qn < QSIZE) g_q[g_qn++] = (WinMsg){hwnd, msg, wp, lp, platform_ticks_ms(), (uint32_t)g_mouse_x, (uint32_t)g_mouse_y};
    pthread_mutex_unlock(&g_qlock);
}

/* template API */
void msg_post(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) { wm_post(hwnd, msg, wparam, lparam); }

static int msg_matches(const WinMsg *m, uint32_t hwnd, uint32_t lo, uint32_t hi) {
    if (hwnd == 0xffffffffu) { if (m->hwnd) return 0; }
    else if (hwnd && m->hwnd != hwnd && !wm_is_child_of(m->hwnd, hwnd)) return 0;
    if (lo || hi) return m->message >= lo && m->message <= hi;
    return 1;
}

/* ---- timers ---------------------------------------------------------------------------------- */
#define MAX_TIMER 64
static struct { int used; uint32_t hwnd, id, ms, proc, next; } g_timers[MAX_TIMER];

void imp_USER32_SetTimer(CPU *cpu) {
    uint32_t hwnd = ARG(0), id = ARG(1), ms = ARG(2), proc = ARG(3);
    if (ms < 10) ms = 10;
    int slot = -1;
    for (int i = 0; i < MAX_TIMER; i++)
        if (g_timers[i].used && g_timers[i].hwnd == hwnd && hwnd && g_timers[i].id == id) slot = i;
    if (slot < 0)
        for (int i = 0; i < MAX_TIMER; i++)
            if (!g_timers[i].used) { slot = i; break; }
    if (slot < 0) RET_STDCALL(4, 0);
    if (!hwnd) id = 0x100 + (uint32_t)slot;
    g_timers[slot].used = 1;
    g_timers[slot].hwnd = hwnd;
    g_timers[slot].id = id;
    g_timers[slot].ms = ms;
    g_timers[slot].proc = proc;
    g_timers[slot].next = platform_ticks_ms() + ms;
    if (getenv("PORT_TRACE_TIMER")) rt_log("SetTimer(%x, %x, %u, %x) slot %d", hwnd, id, ms, proc, slot);
    RET_STDCALL(4, id);
}

void imp_USER32_KillTimer(CPU *cpu) {
    for (int i = 0; i < MAX_TIMER; i++)
        if (g_timers[i].used && g_timers[i].hwnd == ARG(0) && g_timers[i].id == ARG(1)) g_timers[i].used = 0;
    RET_STDCALL(2, 1);
}

/* ---- painting --------------------------------------------------------------------------------- */
static int nc_border(Wnd *w) {
    int b = 0;
    if (w->style & WS_THICKFRAME) b = 4;
    else if ((w->style & WS_DLGFRAME) || (w->exstyle & WS_EX_DLGMODALFRAME) || (w->is_dialog && (w->style & DS_MODALFRAME))) b = 3;
    else if (w->style & WS_BORDER) b = 1;
    if (w->exstyle & WS_EX_CLIENTEDGE) b += 2;
    else if (w->exstyle & WS_EX_STATICEDGE) b += 1;
    return b;
}
static int nc_caption(Wnd *w) { return (w->style & WS_CAPTION) == WS_CAPTION ? 18 : 0; }

/* frame size of a window with these styles (for sizing dialogs from their client area) */
void wm_nc_size(Wnd *w, int *dx, int *dy) {
    *dx = 2 * nc_border(w);
    *dy = 2 * nc_border(w) + nc_caption(w);
}

void wm_client_rect(Wnd *w, Rect *r) {
    int b = nc_border(w), c = nc_caption(w);
    int cw = w->w - 2 * b, ch = w->h - 2 * b - c;
    if (w->style & WS_VSCROLL) cw -= 16;
    if (w->style & WS_HSCROLL) ch -= 16;
    *r = (Rect){0, 0, cw > 0 ? cw : 0, ch > 0 ? ch : 0};
}

static void window_origin(Wnd *w, int *x, int *y) {
    int ox = w->x, oy = w->y;
    if (w->style & WS_CHILD) {
        Wnd *p = wnd_get(w->parent);
        if (p) {
            int px, py;
            wm_client_origin(p, &px, &py);
            ox += px;
            oy += py;
        }
    }
    *x = ox;
    *y = oy;
}

void wm_client_origin(Wnd *w, int *x, int *y) {
    window_origin(w, x, y);
    *x += nc_border(w);
    *y += nc_border(w) + nc_caption(w);
}

/* origin and clip of a window DC on the desktop (gdi32.c) */
int user32_dc_geometry(uint32_t hwnd, int client, int *ox, int *oy, Rect *clip) {
    Wnd *w = wnd_get(hwnd);
    if (!w) return -1;
    if (!really_visible(w)) { *ox = *oy = 0; *clip = (Rect){0, 0, 0, 0}; return 0; }
    int x, y;
    if (client) {
        Rect cr;
        wm_client_origin(w, &x, &y);
        wm_client_rect(w, &cr);
        *clip = (Rect){x, y, x + cr.r, y + cr.b};
    } else {
        window_origin(w, &x, &y);
        *clip = (Rect){x, y, x + w->w, y + w->h};
    }
    *ox = x;
    *oy = y;
    /* clip by every ancestor's client area */
    Wnd *p = (w->style & WS_CHILD) ? wnd_get(w->parent) : NULL;
    while (p) {
        int px, py;
        Rect pr;
        wm_client_origin(p, &px, &py);
        wm_client_rect(p, &pr);
        Rect a = {px, py, px + pr.r, py + pr.b};
        if (a.l > clip->l) clip->l = a.l;
        if (a.t > clip->t) clip->t = a.t;
        if (a.r < clip->r) clip->r = a.r;
        if (a.b < clip->b) clip->b = a.b;
        p = (p->style & WS_CHILD) ? wnd_get(p->parent) : NULL;
    }
    if (clip->r < clip->l) clip->r = clip->l;
    if (clip->b < clip->t) clip->b = clip->t;
    g_desktop_dirty = 1;
    return 0;
}

static void invalidate_one(Wnd *w, const Rect *r, int erase) {
    Rect cr;
    wm_client_rect(w, &cr);
    Rect a = r ? *r : cr;
    if (w->invalid) {
        if (a.l < w->inv.l) w->inv.l = a.l;
        if (a.t < w->inv.t) w->inv.t = a.t;
        if (a.r > w->inv.r) w->inv.r = a.r;
        if (a.b > w->inv.b) w->inv.b = a.b;
    } else {
        w->inv = a;
    }
    w->invalid = 1;
    if (erase) w->erase = 1;
}

/* invalidates a window and (as Windows does without WS_CLIPCHILDREN, and as our
 * single-buffer desktop needs anyway) its descendants */
void wm_invalidate(uint32_t hwnd, const Rect *r, int erase) {
    Wnd *w = wnd_get(hwnd);
    if (!w) return;
    invalidate_one(w, r, erase);
    for (uint32_t c = wm_first_child(hwnd); c; c = wm_next_sibling(c)) wm_invalidate(c, NULL, 1);
    g_desktop_dirty = 1;
}

/* everything overlapping a screen rect needs repainting (after hide/move/destroy) */
static void invalidate_screen_rect(Rect s, uint32_t except) {
    for (int i = 0; i < MAX_WND; i++) {
        Wnd *w = &g_wnd[i];
        if (!w->used || w->hwnd == except || !is_toplevel(w) || !(w->style & WS_VISIBLE)) continue;
        if (w->x < s.r && w->x + w->w > s.l && w->y < s.b && w->y + w->h > s.t) wm_invalidate(w->hwnd, NULL, 1);
    }
}

static void draw_nonclient(Wnd *w) {
    int b = nc_border(w), c = nc_caption(w);
    if (!b && !c && !(w->style & (WS_VSCROLL | WS_HSCROLL))) return;
    uint32_t dc = gdi_dc_new(DC_WINDOW, w->hwnd, 0);
    gdi_dc_set_window_part(dc, 0);
    Rect r = {0, 0, w->w, w->h};
    if (b >= 3) {
        gdi_draw_edge(dc, r, 1, 1);
        gdi_frame_rect_rgb(dc, (Rect){2, 2, w->w - 2, w->h - 2}, sys_color(15));
    } else if (b == 2) {
        gdi_draw_edge(dc, r, 0, 1);
    } else if (b == 1) {
        gdi_frame_rect_rgb(dc, r, 0);
    }
    if (c) {
        Rect cap = {b, b, w->w - b, b + c};
        gdi_fill_rect_rgb(dc, cap, w->hwnd == g_active ? sys_color(2) : sys_color(3));
        uint32_t of = gdi_select(dc, gdi_stock(13));
        gdi_set_text_color(dc, 0xffffff);
        gdi_set_bk_mode(dc, 1);
        Rect tr = {cap.l + 4, cap.t, cap.r - 4, cap.b};
        if (w->text) gdi_draw_text(dc, w->text, -1, &tr, 0x20 | 0x04 | 0x800);
        gdi_select(dc, of);
    }
    gdi_dc_free(dc);
}

/* ---- creation / destruction ------------------------------------------------------------------- */
uint32_t wm_create(CPU *cpu, uint32_t exstyle, const char *clsname, uint32_t clsatom, const char *name, uint32_t style,
                   int x, int y, int w, int h, uint32_t parent, uint32_t menu, uint32_t hinst, uint32_t param) {
    classes_init();
    WndClass *c = wm_find_class(clsname, clsatom);
    if (!c) {
        rt_log("CreateWindowEx: unknown class %s", clsname ? clsname : "?");
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < MAX_WND; i++)
        if (!g_wnd[i].used) { slot = i; break; }
    if (slot < 0) { rt_log("CreateWindowEx: out of windows"); return 0; }
    Wnd *wn = &g_wnd[slot];
    memset(wn, 0, sizeof *wn);
    wn->used = 1;
    wn->hwnd = WND_BASE + (uint32_t)slot * 4;
    wn->cls = c;
    wn->wndproc = c->wndproc;
    wn->style = style & ~WS_VISIBLE;
    wn->exstyle = exstyle;
    wn->hinst = hinst;
    if (style & WS_CHILD) {
        wn->parent = parent;
        wn->id = menu;
        g_z[slot] = ++g_zcounter_bottom;
    } else {
        Wnd *ow = wnd_get(parent);
        while (ow && (ow->style & WS_CHILD)) ow = wnd_get(ow->parent); /* owner is always top level */
        wn->owner = ow ? ow->hwnd : 0;
        wn->parent = wn->owner;
        wn->menu = menu;
        g_z[slot] = --g_zcounter_top;
    }
    if ((uint32_t)x == 0x80000000u) { x = 0; y = 0; }
    if ((uint32_t)w == 0x80000000u) { w = g_client_w; h = g_client_h; }
    wn->x = x; wn->y = y; wn->w = w; wn->h = h;
    wn->create_param = param;
    wn->font = 0;
    /* CREATESTRUCTA in guest memory */
    uint32_t cs = heap_alloc(48 + 8, 1);
    uint32_t gname = name ? guest_strdup(name) : 0;
    uint32_t gcls = clsatom ? clsatom : guest_strdup(clsname);
    WR32(cs + 0, param);
    WR32(cs + 4, hinst);
    WR32(cs + 8, menu);
    WR32(cs + 12, parent);
    WR32(cs + 16, (uint32_t)h);
    WR32(cs + 20, (uint32_t)w);
    WR32(cs + 24, (uint32_t)y);
    WR32(cs + 28, (uint32_t)x);
    WR32(cs + 32, style);
    WR32(cs + 36, gname);
    WR32(cs + 40, gcls);
    WR32(cs + 44, exstyle);
    if (!g_window_created && !(style & WS_CHILD)) g_window_created = 1;
    uint32_t hwnd = wn->hwnd;
    /* CBT hook (MFC attaches its CWnd and subclasses here) */
    if (hook_next(5, 0) >= 0) {
        uint32_t cbt = cs + 48;
        WR32(cbt, cs);
        WR32(cbt + 4, 0);
        if (call_hook(cpu, 5, 3 /* HCBT_CREATEWND */, hwnd, cbt)) {
            wn->used = 0;
            heap_free(cs);
            return 0;
        }
    }
    if (!wnd_get(hwnd)) return 0;
    if (!wm_send(cpu, hwnd, WM_NCCREATE, 0, cs)) {
        rt_log("CreateWindowEx(%s): WM_NCCREATE failed", c->name);
        if (wnd_get(hwnd)) wm_destroy(cpu, hwnd);
        heap_free(cs);
        return 0;
    }
    if ((int32_t)wm_send(cpu, hwnd, WM_CREATE, 0, cs) == -1) {
        rt_log("CreateWindowEx(%s): WM_CREATE failed", c->name);
        if (wnd_get(hwnd)) wm_destroy(cpu, hwnd);
        heap_free(cs);
        return 0;
    }
    heap_free(cs);
    Wnd *wv = wnd_get(hwnd);
    if (!wv) return 0;
    Rect cr;
    wm_client_rect(wv, &cr);
    wm_send(cpu, hwnd, WM_SIZE, 0, (uint32_t)(cr.r & 0xffff) | (uint32_t)cr.b << 16);
    wm_send(cpu, hwnd, WM_MOVE, 0, (uint32_t)(wv->x & 0xffff) | (uint32_t)(wv->y & 0xffff) << 16);
    if (style & WS_CHILD) wm_send(cpu, parent, WM_PARENTNOTIFY, WM_CREATE | (menu << 16), hwnd);
    TRACE("CreateWindowEx(%s, \"%s\", %08x, %d,%d %dx%d, parent %x) -> %x", c->name, name ? name : "", style, x, y, w, h,
          parent, hwnd);
    if (style & WS_VISIBLE) wm_show(cpu, hwnd, 1);
    return hwnd;
}

void imp_USER32_CreateWindowExA(CPU *cpu) {
    uint32_t cls = ARG(1);
    const char *cname = cls >= 0x10000 ? gstr(cls) : NULL;
    uint32_t h = wm_create(cpu, ARG(0), cname, cls < 0x10000 ? cls : 0, ARG(2) ? gstr(ARG(2)) : NULL, ARG(3), (int)ARG(4),
                           (int)ARG(5), (int)ARG(6), (int)ARG(7), ARG(8), ARG(9), ARG(10), ARG(11));
    RET_STDCALL(12, h);
}

void wm_destroy(CPU *cpu, uint32_t hwnd) {
    Wnd *w = wnd_get(hwnd);
    if (!w || w->destroying) return;
    w->destroying = 1;
    if (w->style & WS_VISIBLE) {
        int x, y;
        window_origin(w, &x, &y);
        invalidate_screen_rect((Rect){x, y, x + w->w, y + w->h}, hwnd);
        if (w->style & WS_CHILD) wm_invalidate(w->parent, NULL, 1);
    }
    w->style &= ~WS_VISIBLE;
    /* owned popups first, then children */
    for (int i = 0; i < MAX_WND; i++)
        if (g_wnd[i].used && !g_wnd[i].destroying && !(g_wnd[i].style & WS_CHILD) && g_wnd[i].owner == hwnd)
            wm_destroy(cpu, g_wnd[i].hwnd);
    wm_send(cpu, hwnd, WM_DESTROY, 0, 0);
    for (int i = 0; i < MAX_WND; i++)
        if (g_wnd[i].used && (g_wnd[i].style & WS_CHILD) && g_wnd[i].parent == hwnd) {
            g_wnd[i].destroying = 0;
            wm_destroy(cpu, g_wnd[i].hwnd);
        }
    w->destroying = 1;
    wm_send(cpu, hwnd, WM_NCDESTROY, 0, 0);
    w = wnd_get(hwnd);
    if (!w) return;
    if (g_focus == hwnd || wm_is_child_of(g_focus, hwnd)) g_focus = 0;
    if (g_capture == hwnd) g_capture = 0;
    if (g_active == hwnd) {
        g_active = 0;
        /* activate the owner, or the topmost remaining window */
        Wnd *o = wnd_get(w->owner);
        if (o && (o->style & WS_VISIBLE)) g_active = o->hwnd;
        else {
            uint32_t t = sibling_scan(0, 0, -0x7fffffff, 0x7fffffff, 0);
            while (t && (wnd_get(t)->destroying || !(wnd_get(t)->style & WS_VISIBLE))) t = wm_next_sibling(t);
            g_active = t;
        }
        if (g_active) {
            wm_send(cpu, g_active, WM_ACTIVATE, 1, hwnd);
            if (!g_focus) g_focus = g_active;
        }
    }
    for (int i = 0; i < MAX_TIMER; i++)
        if (g_timers[i].used && g_timers[i].hwnd == hwnd) g_timers[i].used = 0;
    if (w->ctl_free) w->ctl_free(w->ctl);
    free(w->text);
    for (int i = 0; i < w->nprops; i++) free(w->props[i].name);
    w->used = 0;
    g_desktop_dirty = 1;
}

void imp_USER32_DestroyWindow(CPU *cpu) {
    wm_destroy(cpu, ARG(0));
    RET_STDCALL(1, 1);
}

int wm_set_active(CPU *cpu, uint32_t hwnd);

void wm_show(CPU *cpu, uint32_t hwnd, int show) {
    Wnd *w = wnd_get(hwnd);
    if (!w) return;
    int was = (w->style & WS_VISIBLE) != 0;
    if (was == !!show) return;
    wm_send(cpu, hwnd, WM_SHOWWINDOW, show ? 1 : 0, 0);
    w = wnd_get(hwnd);
    if (!w) return;
    if (show) {
        w->style |= WS_VISIBLE;
        wm_invalidate(hwnd, NULL, 1);
    } else {
        w->style &= ~WS_VISIBLE;
        int x, y;
        window_origin(w, &x, &y);
        invalidate_screen_rect((Rect){x, y, x + w->w, y + w->h}, hwnd);
        if (w->style & WS_CHILD) wm_invalidate(w->parent, NULL, 1);
        if (g_focus == hwnd || wm_is_child_of(g_focus, hwnd)) g_focus = 0;
    }
    g_desktop_dirty = 1;
}

/* ShowWindow(hwnd, cmd) */
void imp_USER32_ShowWindow(CPU *cpu) {
    uint32_t hwnd = ARG(0), cmd = ARG(1);
    Wnd *w = wnd_get(hwnd);
    int was = w && (w->style & WS_VISIBLE);
    if (!w) RET_STDCALL(2, 0);
    int show = cmd != 0 && cmd != 6 && cmd != 2 && cmd != 7; /* SW_HIDE, SW_MINIMIZE, SW_SHOWMINIMIZED... */
    if (cmd == 2 || cmd == 6 || cmd == 7) show = 1;          /* keep minimised windows visible: no taskbar here */
    wm_show(cpu, hwnd, show);
    if (show && !(w->style & WS_CHILD) && cmd != 4 && cmd != 8 && cmd != 7) { /* SW_SHOWNOACTIVATE/NA */
        wm_set_active(cpu, hwnd);
    }
    RET_STDCALL(2, was);
}

/* ---- activation, focus, capture ------------------------------------------------------------------ */
static uint32_t g_app_activated;

int wm_set_active(CPU *cpu, uint32_t hwnd) {
    Wnd *w = wnd_get(hwnd);
    if (!w || (w->style & WS_CHILD)) return 0;
    if (!g_app_activated && platform_app_active()) {
        /* the first activation tells the game it has the foreground */
        g_app_activated = 1;
        for (int i = 0; i < MAX_WND; i++)
            if (g_wnd[i].used && is_toplevel(&g_wnd[i])) wm_send(cpu, g_wnd[i].hwnd, WM_ACTIVATEAPP, 1, 0);
    }
    if (g_active == hwnd) return 1;
    uint32_t old = g_active;
    if (old && wnd_get(old)) {
        wm_send(cpu, old, WM_NCACTIVATE, 0, 0);
        wm_send(cpu, old, WM_ACTIVATE, 0, hwnd);
    }
    g_active = hwnd;
    g_z[widx(w)] = --g_zcounter_top;
    wm_send(cpu, hwnd, WM_NCACTIVATE, 1, 0);
    wm_send(cpu, hwnd, WM_ACTIVATE, 1, old);
    if (!g_focus || !(g_focus == hwnd || wm_is_child_of(g_focus, hwnd))) {
        if (!w->is_dialog) wm_set_focus(cpu, hwnd);
    }
    wm_invalidate(hwnd, NULL, 0);
    return 1;
}

uint32_t wm_focus(void) { return g_focus; }

uint32_t wm_set_focus(CPU *cpu, uint32_t hwnd) {
    uint32_t old = g_focus;
    if (hwnd == old) return old;
    if (hwnd && !wnd_get(hwnd)) return 0;
    g_focus = hwnd;
    if (old && wnd_get(old)) wm_send(cpu, old, WM_KILLFOCUS, hwnd, 0);
    if (hwnd && g_focus == hwnd) {
        /* focusing a child activates its top-level window */
        Wnd *t = wnd_get(hwnd);
        while (t && (t->style & WS_CHILD)) t = wnd_get(t->parent);
        if (t && t->hwnd != g_active) wm_set_active(cpu, t->hwnd);
        wm_send(cpu, hwnd, WM_SETFOCUS, old, 0);
    }
    return old;
}

void wm_set_capture(uint32_t hwnd) { g_capture = hwnd; }
uint32_t wm_capture(void) { return g_capture; }

void imp_USER32_SetFocus(CPU *cpu) { RET_STDCALL(1, wm_set_focus(cpu, ARG(0))); }
void imp_USER32_GetFocus(CPU *cpu) { RET_STDCALL(0, g_focus); }
void imp_USER32_GetActiveWindow(CPU *cpu) { RET_STDCALL(0, g_active); }
void imp_USER32_SetActiveWindow(CPU *cpu) {
    uint32_t old = g_active;
    wm_set_active(cpu, ARG(0));
    RET_STDCALL(1, old);
}
void imp_USER32_GetForegroundWindow(CPU *cpu) { RET_STDCALL(0, g_active); }
void imp_USER32_SetForegroundWindow(CPU *cpu) {
    wm_set_active(cpu, ARG(0));
    RET_STDCALL(1, 1);
}
void imp_USER32_GetCapture(CPU *cpu) { RET_STDCALL(0, g_capture); }
void imp_USER32_SetCapture(CPU *cpu) {
    uint32_t old = g_capture;
    g_capture = ARG(0);
    RET_STDCALL(1, old);
}
void imp_USER32_ReleaseCapture(CPU *cpu) {
    g_capture = 0;
    RET_STDCALL(0, 1);
}
void imp_USER32_GetLastActivePopup(CPU *cpu) {
    uint32_t h = ARG(0), r = h;
    for (int i = 0; i < MAX_WND; i++)
        if (g_wnd[i].used && (g_wnd[i].style & WS_VISIBLE) && is_toplevel(&g_wnd[i]) && g_wnd[i].owner == h) r = g_wnd[i].hwnd;
    RET_STDCALL(1, r);
}
/* reachable through GetProcAddress from the CRT message box code */
void user32_GetLastActivePopup(CPU *cpu) { imp_USER32_GetLastActivePopup(cpu); }

void wm_enable(CPU *cpu, uint32_t hwnd, int enable) {
    Wnd *w = wnd_get(hwnd);
    if (!w) return;
    int was = !(w->style & WS_DISABLED);
    if (was == !!enable) return;
    if (enable) w->style &= ~WS_DISABLED;
    else {
        w->style |= WS_DISABLED;
        if (g_focus == hwnd) wm_set_focus(cpu, 0);
        if (g_capture == hwnd) g_capture = 0;
    }
    wm_send(cpu, hwnd, WM_ENABLE, enable ? 1 : 0, 0);
    wm_invalidate(hwnd, NULL, 1);
}

void imp_USER32_EnableWindow(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    uint32_t was_disabled = w ? (w->style & WS_DISABLED) != 0 : 0;
    wm_enable(cpu, ARG(0), ARG(1) != 0);
    RET_STDCALL(2, was_disabled);
}
void imp_USER32_IsWindowEnabled(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    RET_STDCALL(1, w && !(w->style & WS_DISABLED));
}
void imp_USER32_IsWindowVisible(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    RET_STDCALL(1, w && really_visible(w));
}
void imp_USER32_IsWindow(CPU *cpu) { RET_STDCALL(1, wnd_get(ARG(0)) != NULL); }
void imp_USER32_IsIconic(CPU *cpu) { RET_STDCALL(1, 0); }

/* user32_set_active: the platform layer reports app focus changes */
void user32_set_active(int active) {
    rt_log("app %s", active ? "activated" : "deactivated");
    if (active == g_app_active) return;
    g_app_active = active;
    for (int i = 0; i < MAX_WND; i++)
        if (g_wnd[i].used && is_toplevel(&g_wnd[i])) wm_post(g_wnd[i].hwnd, WM_ACTIVATEAPP, active ? 1 : 0, 0);
    if (g_active) wm_post(g_active, WM_ACTIVATE, active ? 1 : 0, 0);
}

void user32_post_close(void) {
    uint32_t main = sibling_scan(0, 0, -0x7fffffff, 0x7fffffff, 1);
    for (int i = 0; i < MAX_WND; i++)
        if (g_wnd[i].used && is_toplevel(&g_wnd[i]) && !g_wnd[i].owner) { main = g_wnd[i].hwnd; break; }
    if (main) wm_post(main, WM_CLOSE, 0, 0);
}

/* ---- window properties ------------------------------------------------------------------------------ */
static int prop_find(Wnd *w, uint32_t key) {
    for (int i = 0; i < w->nprops; i++) {
        if (key < 0x10000 ? w->props[i].atom == key : (w->props[i].name && !strcasecmp(w->props[i].name, gstr(key))))
            return i;
        if (key < 0x10000 && w->props[i].name) {
            const char *an = win32_atom_name(key);
            if (an && !strcasecmp(an, w->props[i].name)) return i;
        }
    }
    return -1;
}

void imp_USER32_SetPropA(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    uint32_t key = ARG(1);
    if (!w) RET_STDCALL(3, 0);
    int i = prop_find(w, key);
    if (i < 0) {
        if (w->nprops >= 16) RET_STDCALL(3, 0);
        i = w->nprops++;
        w->props[i].atom = key < 0x10000 ? key : 0;
        w->props[i].name = key < 0x10000 ? NULL : strdup(gstr(key));
    }
    w->props[i].value = ARG(2);
    RET_STDCALL(3, 1);
}
void imp_USER32_GetPropA(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    int i = w ? prop_find(w, ARG(1)) : -1;
    RET_STDCALL(2, i >= 0 ? w->props[i].value : 0);
}
void imp_USER32_RemovePropA(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    int i = w ? prop_find(w, ARG(1)) : -1;
    if (i < 0) RET_STDCALL(2, 0);
    uint32_t v = w->props[i].value;
    free(w->props[i].name);
    w->props[i] = w->props[--w->nprops];
    RET_STDCALL(2, v);
}

/* ---- window longs / text ------------------------------------------------------------------------------ */
void imp_USER32_SetWindowLongA(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    int32_t idx = (int32_t)ARG(1);
    uint32_t v = ARG(2), old = 0;
    if (!w) RET_STDCALL(3, 0);
    switch (idx) {
    case -4: old = w->wndproc; w->wndproc = v; break;
    case -6: old = w->hinst; w->hinst = v; break;
    case -8: old = w->parent; w->parent = v; if (!(w->style & WS_CHILD)) w->owner = v; break;
    case -12: old = w->id; w->id = v; break;
    case -16: old = w->style; w->style = (w->style & WS_VISIBLE) | (v & ~WS_VISIBLE); break;
    case -20: old = w->exstyle; w->exstyle = v; break;
    case -21: old = w->userdata; w->userdata = v; break;
    default:
        if (idx >= 0 && idx + 4 <= 64) { memcpy(&old, w->extra + idx, 4); memcpy(w->extra + idx, &v, 4); }
    }
    RET_STDCALL(3, old);
}

void imp_USER32_GetWindowLongA(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    int32_t idx = (int32_t)ARG(1);
    uint32_t v = 0;
    if (w) {
        switch (idx) {
        case -4: v = w->wndproc; break;
        case -6: v = w->hinst; break;
        case -8: v = w->parent; break;
        case -12: v = w->id; break;
        case -16: v = w->style; break;
        case -20: v = w->exstyle; break;
        case -21: v = w->userdata; break;
        default: if (idx >= 0 && idx + 4 <= 64) memcpy(&v, w->extra + idx, 4);
        }
    }
    RET_STDCALL(2, v);
}

void wm_set_text(uint32_t hwnd, const char *s) {
    Wnd *w = wnd_get(hwnd);
    if (!w) return;
    free(w->text);
    w->text = strdup(s ? s : "");
}

const char *wm_text(uint32_t hwnd) {
    Wnd *w = wnd_get(hwnd);
    return w && w->text ? w->text : "";
}

void imp_USER32_SetWindowTextA(CPU *cpu) {
    wm_send(cpu, ARG(0), WM_SETTEXT, 0, ARG(1));
    RET_STDCALL(2, 1);
}

void imp_USER32_GetWindowTextA(CPU *cpu) {
    uint32_t r = wm_send(cpu, ARG(0), WM_GETTEXT, ARG(2), ARG(1));
    RET_STDCALL(3, r);
}

/* ---- geometry ------------------------------------------------------------------------------------------- */
static void write_rect(uint32_t p, Rect r) {
    WR32(p, (uint32_t)r.l); WR32(p + 4, (uint32_t)r.t); WR32(p + 8, (uint32_t)r.r); WR32(p + 12, (uint32_t)r.b);
}
static Rect read_rect(uint32_t p) { return (Rect){(int)RD32(p), (int)RD32(p + 4), (int)RD32(p + 8), (int)RD32(p + 12)}; }

void imp_USER32_GetClientRect(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    Rect r = {0, 0, g_client_w, g_client_h};
    if (w) wm_client_rect(w, &r);
    write_rect(ARG(1), r);
    RET_STDCALL(2, 1);
}

void imp_USER32_GetWindowRect(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    Rect r = {0, 0, g_client_w, g_client_h};
    if (w) {
        int x, y;
        window_origin(w, &x, &y);
        r = (Rect){x, y, x + w->w, y + w->h};
    }
    write_rect(ARG(1), r);
    RET_STDCALL(2, 1);
}

void imp_USER32_GetWindowPlacement(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    uint32_t p = ARG(1);
    WR32(p, 44); WR32(p + 4, 0); WR32(p + 8, 1);
    WR32(p + 12, 0); WR32(p + 16, 0); WR32(p + 20, 0); WR32(p + 24, 0);
    Rect r = {0, 0, g_client_w, g_client_h};
    if (w) r = (Rect){w->x, w->y, w->x + w->w, w->y + w->h};
    write_rect(p + 28, r);
    RET_STDCALL(2, 1);
}

void wm_move(CPU *cpu, uint32_t hwnd, int x, int y, int cw, int ch, int repaint) {
    Wnd *w = wnd_get(hwnd);
    if (!w) return;
    int ox, oy;
    window_origin(w, &ox, &oy);
    Rect old = {ox, oy, ox + w->w, oy + w->h};
    int moved = x != w->x || y != w->y, sized = cw != w->w || ch != w->h;
    if (!moved && !sized) return;
    if (w->style & WS_VISIBLE) {
        invalidate_screen_rect(old, hwnd);
        if (w->style & WS_CHILD) wm_invalidate(w->parent, NULL, 1);
    }
    w->x = x; w->y = y; w->w = cw; w->h = ch;
    Rect cr;
    wm_client_rect(w, &cr);
    if (sized) wm_send(cpu, hwnd, WM_SIZE, 0, (uint32_t)(cr.r & 0xffff) | (uint32_t)cr.b << 16);
    if (moved) wm_send(cpu, hwnd, WM_MOVE, 0, (uint32_t)(x & 0xffff) | (uint32_t)(y & 0xffff) << 16);
    if (repaint || 1) wm_invalidate(hwnd, NULL, 1);
}

void imp_USER32_MoveWindow(CPU *cpu) {
    wm_move(cpu, ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5));
    RET_STDCALL(6, 1);
}

/* SetWindowPos(hwnd, after, x, y, cx, cy, flags) */
void imp_USER32_SetWindowPos(CPU *cpu) {
    uint32_t hwnd = ARG(0), after = ARG(1), flags = ARG(6);
    Wnd *w = wnd_get(hwnd);
    if (!w) RET_STDCALL(7, 0);
    int x = (flags & 2) ? w->x : (int)ARG(2), y = (flags & 2) ? w->y : (int)ARG(3);
    int cw = (flags & 1) ? w->w : (int)ARG(4), ch = (flags & 1) ? w->h : (int)ARG(5);
    if (!(flags & 4)) { /* z order */
        int i = widx(w);
        if (after == 1 || after == 0xfffffffeu) g_z[i] = (w->style & WS_CHILD) ? ++g_zcounter_bottom : 0x7ffffff0 - (g_zcounter_bottom++); /* HWND_BOTTOM / NOTOPMOST */
        else if (after == 0 || after == 0xffffffffu) g_z[i] = --g_zcounter_top; /* HWND_TOP / TOPMOST */
        else if (wnd_get(after)) g_z[i] = g_z[widx(wnd_get(after))] + 1;
        wm_invalidate(hwnd, NULL, 1);
    }
    wm_move(cpu, hwnd, x, y, cw, ch, !(flags & 8));
    if (flags & 0x40) wm_show(cpu, hwnd, 1);
    if (flags & 0x80) wm_show(cpu, hwnd, 0);
    if (!(flags & 0x10) && (flags & 0x40) && !(w->style & WS_CHILD)) wm_set_active(cpu, hwnd);
    RET_STDCALL(7, 1);
}

void imp_USER32_AdjustWindowRectEx(CPU *cpu) {
    uint32_t p = ARG(0), style = ARG(1), ex = ARG(3);
    Wnd tmp = {0};
    tmp.style = style;
    tmp.exstyle = ex;
    int b = nc_border(&tmp), c = nc_caption(&tmp);
    if (ARG(2)) c += 19;
    Rect r = read_rect(p);
    r.l -= b; r.r += b; r.t -= b + c; r.b += b;
    write_rect(p, r);
    RET_STDCALL(4, 1);
}

static void screen_offset(uint32_t hwnd, int *x, int *y) {
    Wnd *w = wnd_get(hwnd);
    *x = *y = 0;
    if (w) wm_client_origin(w, x, y);
}

void imp_USER32_ClientToScreen(CPU *cpu) {
    int x, y;
    screen_offset(ARG(0), &x, &y);
    uint32_t p = ARG(1);
    WR32(p, RD32(p) + (uint32_t)x);
    WR32(p + 4, RD32(p + 4) + (uint32_t)y);
    RET_STDCALL(2, 1);
}

void imp_USER32_ScreenToClient(CPU *cpu) {
    int x, y;
    screen_offset(ARG(0), &x, &y);
    uint32_t p = ARG(1);
    WR32(p, RD32(p) - (uint32_t)x);
    WR32(p + 4, RD32(p + 4) - (uint32_t)y);
    RET_STDCALL(2, 1);
}

/* MapWindowPoints(from, to, pts, n) */
void imp_USER32_MapWindowPoints(CPU *cpu) {
    int fx, fy, tx, ty;
    screen_offset(ARG(0), &fx, &fy);
    screen_offset(ARG(1), &tx, &ty);
    uint32_t p = ARG(2), n = ARG(3);
    int dx = fx - tx, dy = fy - ty;
    for (uint32_t i = 0; i < n; i++) {
        WR32(p + 8 * i, RD32(p + 8 * i) + (uint32_t)dx);
        WR32(p + 8 * i + 4, RD32(p + 8 * i + 4) + (uint32_t)dy);
    }
    RET_STDCALL(4, (uint32_t)(dx & 0xffff) | (uint32_t)dy << 16);
}

/* ---- hierarchy -------------------------------------------------------------------------------------------- */
void imp_USER32_GetParent(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    RET_STDCALL(1, !w ? 0 : (w->style & WS_CHILD) ? w->parent : w->owner);
}

void imp_USER32_GetTopWindow(CPU *cpu) {
    uint32_t h = ARG(0);
    RET_STDCALL(1, h ? wm_first_child(h) : sibling_scan(0, 0, -0x7fffffff, 0x7fffffff, 0));
}

/* GetWindow(hwnd, cmd) */
void imp_USER32_GetWindow(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    uint32_t r = 0;
    if (w) {
        int child = (w->style & WS_CHILD) != 0;
        switch (ARG(1)) {
        case 0: r = sibling_scan(w->parent, child, -0x7fffffff, 0x7fffffff, 0); break;
        case 1: r = sibling_scan(w->parent, child, -0x7fffffff, 0x7fffffff, 1); break;
        case 2: r = wm_next_sibling(w->hwnd); break;
        case 3: r = wm_prev_sibling(w->hwnd); break;
        case 4: r = child ? 0 : w->owner; break;
        case 5: r = wm_first_child(w->hwnd); break;
        }
    }
    RET_STDCALL(2, r);
}

void imp_USER32_GetDlgItem(CPU *cpu) {
    uint32_t r = 0;
    for (uint32_t c = wm_first_child(ARG(0)); c; c = wm_next_sibling(c))
        if (wnd_get(c)->id == ARG(1)) { r = c; break; }
    RET_STDCALL(2, r);
}

void imp_USER32_GetDlgCtrlID(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    RET_STDCALL(1, w ? w->id : 0);
}

void imp_USER32_FindWindowA(CPU *cpu) {
    const char *cls = ARG(0) >= 0x10000 ? gstr(ARG(0)) : NULL, *title = ARG(1) ? gstr(ARG(1)) : NULL;
    uint32_t r = 0;
    for (int i = 0; i < MAX_WND && !r; i++) {
        Wnd *w = &g_wnd[i];
        if (!w->used || !is_toplevel(w)) continue;
        if (cls && strcasecmp(w->cls->name, cls)) continue;
        if (title && (!w->text || strcmp(w->text, title))) continue;
        if (cls || title) r = w->hwnd;
    }
    TRACE("FindWindowA(%s, %s) -> %x", cls ? cls : "NULL", title ? title : "NULL", r);
    RET_STDCALL(2, r);
}

/* ---- painting APIs -------------------------------------------------------------------------------------------- */
void imp_USER32_GetDC(CPU *cpu) {
    uint32_t h = ARG(0);
    RET_STDCALL(1, gdi_dc_new(h ? DC_WINDOW : DC_SCREEN, h, 0));
}
void imp_USER32_GetWindowDC(CPU *cpu) {
    uint32_t dc = gdi_dc_new(ARG(0) ? DC_WINDOW : DC_SCREEN, ARG(0), 0);
    gdi_dc_set_window_part(dc, 0);
    RET_STDCALL(1, dc);
}
void imp_USER32_ReleaseDC(CPU *cpu) {
    gdi_dc_free(ARG(1));
    g_desktop_dirty = 1;
    RET_STDCALL(2, 1);
}

void imp_USER32_BeginPaint(CPU *cpu) {
    uint32_t hwnd = ARG(0), ps = ARG(1);
    Wnd *w = wnd_get(hwnd);
    uint32_t dc = gdi_dc_new(DC_WINDOW, hwnd, 0);
    memset(GPTR(ps), 0, 64);
    WR32(ps, dc);
    Rect r = {0, 0, 0, 0};
    int erase = 0;
    if (w) {
        r = w->invalid ? w->inv : (Rect){0, 0, 0, 0};
        erase = w->erase;
        w->invalid = 0;
        w->erase = 0;
    }
    if (erase) erase = wm_send(cpu, hwnd, WM_ERASEBKGND, dc, 0) == 0;
    WR32(ps + 4, (uint32_t)erase);
    write_rect(ps + 8, r);
    RET_STDCALL(2, dc);
}

void imp_USER32_EndPaint(CPU *cpu) {
    gdi_dc_free(RD32(ARG(1)));
    g_desktop_dirty = 1;
    RET_STDCALL(2, 1);
}

void imp_USER32_InvalidateRect(CPU *cpu) {
    uint32_t h = ARG(0);
    Rect r;
    if (ARG(1)) r = read_rect(ARG(1));
    if (!h) {
        for (int i = 0; i < MAX_WND; i++)
            if (g_wnd[i].used && is_toplevel(&g_wnd[i])) wm_invalidate(g_wnd[i].hwnd, NULL, ARG(2));
    } else {
        wm_invalidate(h, ARG(1) ? &r : NULL, ARG(2) != 0);
    }
    RET_STDCALL(3, 1);
}

void imp_USER32_ValidateRect(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    if (w) { w->invalid = 0; w->erase = 0; }
    RET_STDCALL(2, 1);
}

static void paint_now(CPU *cpu, Wnd *w) {
    if (!w->invalid || !really_visible(w)) return;
    if (!(w->style & WS_CHILD) || nc_border(w)) draw_nonclient(w);
    uint32_t h = w->hwnd;
    wm_send(cpu, h, WM_PAINT, 0, 0);
    w = wnd_get(h);
    if (w && w->invalid) { w->invalid = 0; w->erase = 0; } /* a WM_PAINT without BeginPaint */
}

void imp_USER32_UpdateWindow(CPU *cpu) {
    Wnd *w = wnd_get(ARG(0));
    if (w) {
        paint_now(cpu, w);
        for (uint32_t c = wm_first_child(ARG(0)); c; c = wm_next_sibling(c)) {
            Wnd *cw = wnd_get(c);
            if (cw) paint_now(cpu, cw);
        }
    }
    RET_STDCALL(1, 1);
}

void imp_USER32_FillRect(CPU *cpu) {
    Rect r = read_rect(ARG(1));
    gdi_fill_rect_brush(ARG(0), r, ARG(2));
    RET_STDCALL(3, 1);
}

void imp_USER32_FrameRect(CPU *cpu) {
    Rect r = read_rect(ARG(1));
    int is_null;
    uint32_t c = gdi_brush_color(ARG(2), &is_null);
    if (!is_null) gdi_frame_rect_rgb(ARG(0), r, c);
    RET_STDCALL(3, 1);
}

void imp_USER32_DrawFocusRect(CPU *cpu) {
    Rect r = read_rect(ARG(1));
    for (int x = r.l; x < r.r; x += 2) {
        gdi_fill_rect_rgb(ARG(0), (Rect){x, r.t, x + 1, r.t + 1}, 0);
        gdi_fill_rect_rgb(ARG(0), (Rect){x, r.b - 1, x + 1, r.b}, 0);
    }
    for (int y = r.t; y < r.b; y += 2) {
        gdi_fill_rect_rgb(ARG(0), (Rect){r.l, y, r.l + 1, y + 1}, 0);
        gdi_fill_rect_rgb(ARG(0), (Rect){r.r - 1, y, r.r, y + 1}, 0);
    }
    RET_STDCALL(2, 1);
}

/* DrawEdge(hdc, rect, edge, flags) */
void imp_USER32_DrawEdge(CPU *cpu) {
    Rect r = read_rect(ARG(1));
    uint32_t edge = ARG(2), flags = ARG(3);
    int raised = (edge & 0x5) != 0; /* BDR_RAISEDOUTER | BDR_RAISEDINNER */
    int deep = (edge & 0x3) && (edge & 0xc);
    if (edge == 0x6 || edge == 0x9) { /* EDGE_ETCHED / EDGE_BUMP */
        gdi_frame_rect_rgb(ARG(0), (Rect){r.l + 1, r.t + 1, r.r, r.b}, sys_color(20));
        gdi_frame_rect_rgb(ARG(0), (Rect){r.l, r.t, r.r - 1, r.b - 1}, sys_color(16));
    } else {
        gdi_draw_edge(ARG(0), r, raised, deep);
    }
    if (flags & 0x800) gdi_fill_rect_rgb(ARG(0), (Rect){r.l + 2, r.t + 2, r.r - 2, r.b - 2}, sys_color(15)); /* BF_MIDDLE */
    if (flags & 0x2000) { r.l += 2; r.t += 2; r.r -= 2; r.b -= 2; write_rect(ARG(1), r); }                  /* BF_ADJUST */
    RET_STDCALL(4, 1);
}

/* DrawFrameControl(hdc, rect, type, state): buttons and checks */
void imp_USER32_DrawFrameControl(CPU *cpu) {
    Rect r = read_rect(ARG(1));
    uint32_t type = ARG(2), state = ARG(3);
    gdi_fill_rect_rgb(ARG(0), r, sys_color(type == 4 ? 5 : 15));
    gdi_draw_edge(ARG(0), r, !(state & 0x200), 1); /* DFCS_PUSHED */
    if (state & 0x400) {                          /* DFCS_CHECKED */
        int cx = (r.l + r.r) / 2, cy = (r.t + r.b) / 2;
        for (int i = -3; i <= 3; i++) gdi_fill_rect_rgb(ARG(0), (Rect){cx + i, cy + (i < 0 ? -i : i) / 2 - 1, cx + i + 1, cy + (i < 0 ? -i : i) / 2 + 1}, 0);
    }
    RET_STDCALL(4, 1);
}

void imp_USER32_DrawTextA(CPU *cpu) {
    Rect r = read_rect(ARG(3));
    int h = gdi_draw_text(ARG(0), gstr(ARG(1)), (int)ARG(2), &r, ARG(4));
    if (ARG(4) & 0x400) write_rect(ARG(3), r);
    RET_STDCALL(5, (uint32_t)h);
}

/* TabbedTextOutA(hdc, x, y, str, n, ntabs, tabs, origin): tabs expand to 8 average chars */
void imp_USER32_TabbedTextOutA(CPU *cpu) {
    const char *s = gstr(ARG(3));
    int n = (int)ARG(4);
    if (n < 0) n = (int)strlen(s);
    Rect r = {(int)ARG(1), (int)ARG(2), 4096, 4096};
    gdi_draw_text(ARG(0), s, n, &r, 0x20 | 0x40 | 0x100 | 0x800);
    int w = gdi_text_width(ARG(0), s, n);
    RET_STDCALL(8, (uint32_t)w | (uint32_t)gdi_font_height(ARG(0)) << 16);
}

/* GrayStringA(hdc, brush, proc, data, n, x, y, w, h): draws the string in gray */
void imp_USER32_GrayStringA(CPU *cpu) {
    uint32_t proc = ARG(2);
    if (proc) {
        uint32_t args[3] = {ARG(0), ARG(3), ARG(4)};
        guest_call(cpu, proc, 3, args);
        RET_STDCALL(9, 1);
    }
    const char *s = gstr(ARG(3));
    int n = (int)ARG(4);
    if (!n) n = (int)strlen(s);
    Rect r = {(int)ARG(5), (int)ARG(6), (int)ARG(5) + 4096, (int)ARG(6) + 4096};
    gdi_set_text_color(ARG(0), sys_color(17));
    gdi_draw_text(ARG(0), s, n, &r, 0x20 | 0x100 | 0x800);
    RET_STDCALL(9, 1);
}

void imp_USER32_GetSysColor(CPU *cpu) { RET_STDCALL(1, sys_color((int)ARG(0))); }
void imp_USER32_GetSysColorBrush(CPU *cpu) { RET_STDCALL(1, sys_color_brush((int)ARG(0))); }

/* ---- rectangles -------------------------------------------------------------------------------------------------- */
void imp_USER32_SetRect(CPU *cpu) { write_rect(ARG(0), (Rect){(int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4)}); RET_STDCALL(5, 1); }
void imp_USER32_SetRectEmpty(CPU *cpu) { write_rect(ARG(0), (Rect){0, 0, 0, 0}); RET_STDCALL(1, 1); }
void imp_USER32_CopyRect(CPU *cpu) { write_rect(ARG(0), read_rect(ARG(1))); RET_STDCALL(2, 1); }
void imp_USER32_IsRectEmpty(CPU *cpu) { Rect r = read_rect(ARG(0)); RET_STDCALL(1, r.r <= r.l || r.b <= r.t); }
void imp_USER32_OffsetRect(CPU *cpu) {
    Rect r = read_rect(ARG(0));
    int dx = (int)ARG(1), dy = (int)ARG(2);
    write_rect(ARG(0), (Rect){r.l + dx, r.t + dy, r.r + dx, r.b + dy});
    RET_STDCALL(3, 1);
}
void imp_USER32_InflateRect(CPU *cpu) {
    Rect r = read_rect(ARG(0));
    int dx = (int)ARG(1), dy = (int)ARG(2);
    write_rect(ARG(0), (Rect){r.l - dx, r.t - dy, r.r + dx, r.b + dy});
    RET_STDCALL(3, 1);
}
void imp_USER32_PtInRect(CPU *cpu) {
    Rect r = read_rect(ARG(0));
    int x = (int)ARG(1), y = (int)ARG(2);
    RET_STDCALL(3, x >= r.l && x < r.r && y >= r.t && y < r.b);
}
void imp_USER32_IntersectRect(CPU *cpu) {
    Rect a = read_rect(ARG(1)), b = read_rect(ARG(2));
    Rect r = {a.l > b.l ? a.l : b.l, a.t > b.t ? a.t : b.t, a.r < b.r ? a.r : b.r, a.b < b.b ? a.b : b.b};
    int ok = r.r > r.l && r.b > r.t;
    write_rect(ARG(0), ok ? r : (Rect){0, 0, 0, 0});
    RET_STDCALL(3, ok);
}
void imp_USER32_UnionRect(CPU *cpu) {
    Rect a = read_rect(ARG(1)), b = read_rect(ARG(2));
    int ae = a.r <= a.l || a.b <= a.t, be = b.r <= b.l || b.b <= b.t;
    Rect r = ae ? b : be ? a : (Rect){a.l < b.l ? a.l : b.l, a.t < b.t ? a.t : b.t, a.r > b.r ? a.r : b.r, a.b > b.b ? a.b : b.b};
    write_rect(ARG(0), r);
    RET_STDCALL(3, !(ae && be));
}

/* ---- input --------------------------------------------------------------------------------------------------------- */
static uint8_t g_key_toggle[256];
static int g_mouse_buttons;

static Wnd *hit_test_children(Wnd *p, int x, int y) {
    for (uint32_t c = wm_first_child(p->hwnd); c; c = wm_next_sibling(c)) {
        Wnd *w = wnd_get(c);
        if (!w || !(w->style & WS_VISIBLE)) continue;
        int ox, oy;
        window_origin(w, &ox, &oy);
        if (x >= ox && x < ox + w->w && y >= oy && y < oy + w->h) {
            Wnd *d = hit_test_children(w, x, y);
            return d ? d : w;
        }
    }
    return NULL;
}

static Wnd *hit_test(int x, int y) {
    /* top-level windows, topmost first */
    for (uint32_t t = sibling_scan(0, 0, -0x7fffffff, 0x7fffffff, 0); t; t = wm_next_sibling(t)) {
        Wnd *w = wnd_get(t);
        if (!w || !(w->style & WS_VISIBLE)) continue;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) {
            Wnd *c = hit_test_children(w, x, y);
            /* static controls are transparent to clicks unless SS_NOTIFY */
            if (c && !strcasecmp(c->cls->name, "STATIC") && !(c->style & 0x100)) c = wnd_get(c->parent);
            return c ? c : w;
        }
    }
    return NULL;
}

/* modal: a disabled window (or one whose top level is disabled) gets no input.
 * WS_CHILD|WS_POPUP is a top-level popup on Windows (CrazyHook's dialogs): the window it
 * is attached to is the owner the modal loop disabled, which doesn't block it. */
static int input_blocked(Wnd *w) {
    while (w) {
        if (w->style & WS_DISABLED) return 1;
        if (!(w->style & WS_CHILD) || (w->style & WS_POPUP)) return 0;
        w = wnd_get(w->parent);
    }
    return 0;
}

/* called by the platform layer: screen (game) coordinates; type 0 move, 1 down, 2 up; button 0 left, 1 right */
void user32_mouse_event(int x, int y, int button, int type) {
    g_mouse_x = x;
    g_mouse_y = y;
    if (type == 1) g_mouse_buttons |= 1 << button;
    if (type == 2) g_mouse_buttons &= ~(1 << button);
    Wnd *w = g_capture ? wnd_get(g_capture) : hit_test(x, y);
    if (!w) return;
    if (type == 1 && input_blocked(w)) return;
    int cx, cy;
    wm_client_origin(w, &cx, &cy);
    uint32_t lp = (uint32_t)((x - cx) & 0xffff) | (uint32_t)((y - cy) & 0xffff) << 16;
    uint32_t wp = (g_mouse_buttons & 1 ? 1 : 0) | (g_mouse_buttons & 2 ? 2 : 0);
    uint32_t msg = WM_MOUSEMOVE;
    if (type == 1) {
        static uint32_t last_t, last_h;
        static int lx, ly;
        uint32_t now = platform_ticks_ms();
        int dbl = button == 0 && last_h == w->hwnd && now - last_t < 500 && abs(x - lx) < 8 && abs(y - ly) < 8 &&
                  (w->cls->style & 8); /* CS_DBLCLKS */
        msg = button ? WM_RBUTTONDOWN : dbl ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN;
        last_t = dbl ? 0 : now;
        last_h = w->hwnd;
        lx = x;
        ly = y;
    } else if (type == 2) {
        msg = button ? WM_RBUTTONUP : WM_LBUTTONUP;
    }
    wm_post(w->hwnd, msg, wp, lp);
}

/* PC/AT scan codes (set 1) and the extended-key flag, as Windows puts them in lParam */
static uint32_t vk_scancode(int vk, int *ext) {
    *ext = 0;
    static const uint8_t letters[26] = {0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                        0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
    if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
    if (vk >= '1' && vk <= '9') return (uint32_t)(2 + vk - '1');
    if (vk == '0') return 0x0b;
    if (vk >= 0x70 && vk <= 0x79) return (uint32_t)(0x3b + vk - 0x70);
    switch (vk) {
    case 0x1b: return 0x01;
    case 0x08: return 0x0e;
    case 0x09: return 0x0f;
    case 0x0d: return 0x1c;
    case 0x11: return 0x1d;
    case 0x10: return 0x2a;
    case 0x12: return 0x38;
    case 0x20: return 0x39;
    case 0x7a: return 0x57;
    case 0x7b: return 0x58;
    case 0x25: *ext = 1; return 0x4b;
    case 0x26: *ext = 1; return 0x48;
    case 0x27: *ext = 1; return 0x4d;
    case 0x28: *ext = 1; return 0x50;
    case 0x2d: *ext = 1; return 0x52;
    case 0x2e: *ext = 1; return 0x53;
    case 0x24: *ext = 1; return 0x47;
    case 0x23: *ext = 1; return 0x4f;
    case 0x21: *ext = 1; return 0x49;
    case 0x22: *ext = 1; return 0x51;
    case 0xbd: return 0x0c;
    case 0xbb: return 0x0d;
    case 0xba: return 0x27;
    case 0xde: return 0x28;
    case 0xbc: return 0x33;
    case 0xbe: return 0x34;
    case 0xbf: return 0x35;
    }
    return 0;
}

static uint8_t g_key_was_down[256];

/* key transitions from input.c (and auto-repeat from the platform) */
void user32_key_event(int vk, int down) {
    if (down && (vk == 0x14 || vk == 0x90 || vk == 0x91)) g_key_toggle[vk] ^= 1;
    uint32_t target = g_focus ? g_focus : g_active;
    if (!target) {
        for (int i = 0; i < MAX_WND; i++)
            if (g_wnd[i].used && is_toplevel(&g_wnd[i])) { target = g_wnd[i].hwnd; break; }
    }
    if (!target) return;
    int alt = input_vk_down(0x12);
    uint32_t msg = down ? (alt || vk == 0x12 || vk == 0x79 ? WM_SYSKEYDOWN : WM_KEYDOWN) : (alt || vk == 0x12 || vk == 0x79 ? WM_SYSKEYUP : WM_KEYUP);
    int ext;
    uint32_t sc = vk_scancode(vk, &ext);
    uint32_t lp = 1 | sc << 16 | (uint32_t)ext << 24;
    if (alt && vk != 0x12) lp |= 1u << 29;                  /* context code */
    if (!down) lp |= 0xc0000000u;                           /* previous state + transition */
    else if (g_key_was_down[vk & 255]) lp |= 1u << 30;      /* auto-repeat */
    g_key_was_down[vk & 255] = (uint8_t)down;
    wm_post(target, msg, (uint32_t)vk, lp);
}

static int g_char_from_keys = 1;
void user32_text_input_active(int active) { g_char_from_keys = !active; }
void user32_char_event(int ch) {
    uint32_t target = g_focus ? g_focus : g_active;
    if (target) wm_post(target, WM_CHAR, (uint32_t)(ch & 0xff), 1);
}

void wm_translate(const WinMsg *m) {
    if (!g_char_from_keys || (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN)) return;
    int vk = (int)m->wparam, shift = input_vk_down(0x10), ctrl = input_vk_down(0x11);
    int ch = 0;
    if (vk >= 'A' && vk <= 'Z') {
        ch = (shift ^ g_key_toggle[0x14]) ? vk : vk + 32;
        if (ctrl) ch = vk - 'A' + 1;
    } else if (vk >= '0' && vk <= '9') {
        static const char sh[] = ")!@#$%^&*(";
        ch = shift ? sh[vk - '0'] : vk;
    } else if (vk == 0x20 || vk == 0x0d || vk == 0x08 || vk == 0x1b || vk == 0x09) {
        ch = vk;
    } else if (vk >= 0x60 && vk <= 0x69) {
        ch = '0' + vk - 0x60;
    } else {
        switch (vk) {
        case 0xba: ch = shift ? ':' : ';'; break;
        case 0xbb: ch = shift ? '+' : '='; break;
        case 0xbc: ch = shift ? '<' : ','; break;
        case 0xbd: ch = shift ? '_' : '-'; break;
        case 0xbe: ch = shift ? '>' : '.'; break;
        case 0xbf: ch = shift ? '?' : '/'; break;
        case 0xde: ch = shift ? '"' : '\''; break;
        }
    }
    if (ch) wm_post(m->hwnd, m->message == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR, (uint32_t)ch, m->lparam);
}

void imp_USER32_TranslateMessage(CPU *cpu) {
    WinMsg m = {RD32(ARG(0)), RD32(ARG(0) + 4), RD32(ARG(0) + 8), RD32(ARG(0) + 12), 0, 0, 0};
    wm_translate(&m);
    RET_STDCALL(1, 0);
}

static uint32_t g_keystate_polls[256];
void imp_USER32_GetKeyState(CPU *cpu) {
    int vk = (int)(ARG(0) & 0xff);
    g_keystate_polls[vk]++;
    int down = vk == 1 ? (g_mouse_buttons & 1) : vk == 2 ? (g_mouse_buttons & 2) : input_vk_down(vk);
    if (vk == 0xa0 || vk == 0xa1) down = input_vk_down(0x10);
    if (vk == 0xa2 || vk == 0xa3) down = input_vk_down(0x11);
    RET_STDCALL(1, (down ? 0xff80u : 0) | g_key_toggle[vk]);
}

/* GetAsyncKeyState: bit 15 = down now, bit 0 = pressed since the previous query */
static uint8_t g_vk_seen_down[256];
void imp_USER32_GetAsyncKeyState(CPU *cpu) {
    uint32_t vk = ARG(0) & 0xff;
    int down = vk == 1 ? (g_mouse_buttons & 1) : vk == 2 ? (g_mouse_buttons & 2) : input_vk_down((int)vk);
    if (vk == 0xa0 || vk == 0xa1) down = input_vk_down(0x10);
    if (vk == 0xa2 || vk == 0xa3) down = input_vk_down(0x11);
    input_note_poll((int)vk);
    static int log_vk = -1;
    static uint32_t counts[256], last_dump;
    if (log_vk < 0) log_vk = getenv("PORT_LOG_VK") != NULL;
    if (log_vk) {
        counts[vk]++;
        if (platform_ticks_ms() - last_dump > 5000) {
            char line[2048];
            int n = snprintf(line, sizeof line, "VK polls:");
            for (int i = 0; i < 256; i++)
                if (counts[i] > 10 && n < 2000) n += snprintf(line + n, sizeof line - n, " %02x:%u", i, counts[i]);
            rt_log("%s", line);
            n = snprintf(line, sizeof line, "GetKeyState polls:");
            for (int i = 0; i < 256; i++)
                if (g_keystate_polls[i] > 10 && n < 2000) n += snprintf(line + n, sizeof line - n, " %02x:%u", i, g_keystate_polls[i]);
            memset(g_keystate_polls, 0, sizeof g_keystate_polls);
            rt_log("%s", line);
            unsigned kt_addr = 0, kt_n = 0;
            if (sscanf(cfg_str("input", "key_table", ""), "%x %u", &kt_addr, &kt_n) == 2 && kt_n <= 64) {
                n = snprintf(line, sizeof line, "key table @%x:", kt_addr);
                for (unsigned i = 0; i < kt_n; i++) n += snprintf(line + n, sizeof line - n, " %02x", RD32(kt_addr + i * 4));
                rt_log("%s", line);
            }
            memset(counts, 0, sizeof counts);
            last_dump = platform_ticks_ms();
        }
    }
    uint32_t r = 0;
    if (down) {
        r = 0x8000;
        if (!g_vk_seen_down[vk]) r |= 1;
    }
    g_vk_seen_down[vk] = (uint8_t)(down != 0);
    RET_STDCALL(1, r);
}

void imp_USER32_GetCursorPos(CPU *cpu) {
    WR32(ARG(0), (uint32_t)g_mouse_x);
    WR32(ARG(0) + 4, (uint32_t)g_mouse_y);
    RET_STDCALL(1, 1);
}
void imp_USER32_SetCursorPos(CPU *cpu) {
    g_mouse_x = (int)ARG(0);
    g_mouse_y = (int)ARG(1);
    RET_STDCALL(2, 1);
}
void imp_USER32_ClipCursor(CPU *cpu) { RET_STDCALL(1, 1); }
void imp_USER32_GetMessagePos(CPU *cpu) { RET_STDCALL(0, g_msg_pos); }
void imp_USER32_GetMessageTime(CPU *cpu) { RET_STDCALL(0, g_msg_time); }

/* SendInput(n, INPUT*, size): keyboard inputs feed the key state (CrazyHook uses it) */
void imp_USER32_SendInput(CPU *cpu) {
    uint32_t n = ARG(0), p = ARG(1), sz = ARG(2);
    for (uint32_t i = 0; i < n; i++, p += sz) {
        if (RD32(p) == 1) { /* INPUT_KEYBOARD */
            int vk = RD16(p + 4);
            int up = (RD32(p + 8) & 2) != 0;
            input_set_vk(vk, INPUT_SRC_KEYBOARD, !up);
        }
    }
    RET_STDCALL(3, n);
}

/* ---- message retrieval ------------------------------------------------------------------------------------------- */
/* composes the screen: the DirectDraw frame (if the game has one) with the visible
 * GDI windows on top, then hands it to the platform */
static uint32_t *g_compose;
static int g_compose_w, g_compose_h;

void wm_present(void) {
    Canvas *d = desktop_canvas();
    int w = d->w, h = d->h;
    if (!g_compose || g_compose_w != w || g_compose_h != h) {
        free(g_compose);
        g_compose = calloc((size_t)w * h, 4);
        g_compose_w = w;
        g_compose_h = h;
    }
    uint32_t coop = ddraw_coop_hwnd();
    int have_dd = ddraw_render_frame(g_compose, w, h);
    if (!have_dd) memcpy(g_compose, g_desktop_px, (size_t)w * h * 4);
    /* overlay top-level windows bottom-up */
    uint32_t order[MAX_WND];
    int n = 0;
    for (uint32_t t = sibling_scan(0, 0, -0x7fffffff, 0x7fffffff, 0); t && n < MAX_WND; t = wm_next_sibling(t)) order[n++] = t;
    for (int k = n - 1; k >= 0; k--) {
        Wnd *win = wnd_get(order[k]);
        if (!win || !(win->style & WS_VISIBLE)) continue;
        if (have_dd && (win->hwnd == coop || (win->x <= 0 && win->y <= 0 && win->w >= w && win->h >= h))) continue;
        if (!have_dd) continue; /* already in the desktop copy */
        int x0 = win->x < 0 ? 0 : win->x, y0 = win->y < 0 ? 0 : win->y;
        int x1 = win->x + win->w > w ? w : win->x + win->w, y1 = win->y + win->h > h ? h : win->y + win->h;
        for (int y = y0; y < y1; y++) memcpy(g_compose + y * w + x0, g_desktop_px + y * w + x0, (size_t)(x1 - x0) * 4);
    }
    /* child windows of the DirectDraw window (CrazyHook's dialogs are WS_CHILD of it):
     * on Windows GDI draws them straight over the primary surface */
    if (have_dd && coop) {
        Wnd *cw = wnd_get(coop);
        int ox = 0, oy = 0;
        if (cw) wm_client_origin(cw, &ox, &oy);
        for (int i = 0; i < MAX_WND; i++) {
            Wnd *win = &g_wnd[i];
            if (!win->used || win->parent != coop || !(win->style & WS_CHILD) || !(win->style & WS_VISIBLE)) continue;
            int x0 = ox + win->x, y0 = oy + win->y, x1 = x0 + win->w, y1 = y0 + win->h;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > w) x1 = w;
            if (y1 > h) y1 = h;
            for (int y = y0; y < y1; y++) memcpy(g_compose + y * w + x0, g_desktop_px + y * w + x0, (size_t)(x1 - x0) * 4);
        }
    }
    platform_present(g_compose, w, h, w * 4, PIX_XRGB8888);
    g_desktop_dirty = 0;
    g_last_present = platform_ticks_ms();
}

/* does anything besides the DirectDraw window need showing? */
static int gdi_windows_visible(void) {
    uint32_t coop = ddraw_coop_hwnd();
    for (int i = 0; i < MAX_WND; i++) {
        Wnd *w = &g_wnd[i];
        if (w->used && is_toplevel(w) && (w->style & WS_VISIBLE) && w->hwnd != coop) return 1;
        if (w->used && coop && w->parent == coop && (w->style & (WS_CHILD | WS_VISIBLE)) == (WS_CHILD | WS_VISIBLE)) return 1;
    }
    return !coop;
}

int wm_gdi_overlay_active(void) { return gdi_windows_visible(); }

void avi_idle(void);
void wm_message_loop_idle(CPU *cpu) {
    (void)cpu;
    platform_pump_events();
    avi_idle();
    uint32_t now = platform_ticks_ms();
    if (g_desktop_dirty && gdi_windows_visible() && now - g_last_present >= 16) wm_present();
}

static int find_paint(WinMsg *m, uint32_t hwnd) {
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < MAX_WND; i++) {
            Wnd *w = &g_wnd[i];
            if (!w->used || !w->invalid || w->destroying) continue;
            if (!really_visible(w)) continue;
            if (hwnd && hwnd != 0xffffffffu && w->hwnd != hwnd && !wm_is_child_of(w->hwnd, hwnd)) continue;
            /* parents paint before their children */
            Wnd *p = (w->style & WS_CHILD) ? wnd_get(w->parent) : NULL;
            int parent_pending = 0;
            while (p) {
                if (p->invalid && really_visible(p)) { parent_pending = 1; break; }
                p = (p->style & WS_CHILD) ? wnd_get(p->parent) : NULL;
            }
            if (parent_pending && pass == 0) continue;
            if (!(w->style & WS_CHILD) || nc_border(w)) draw_nonclient(w);
            *m = (WinMsg){w->hwnd, WM_PAINT, 0, 0, platform_ticks_ms(), (uint32_t)g_mouse_x, (uint32_t)g_mouse_y};
            return 1;
        }
    return 0;
}

static int find_timer(WinMsg *m, uint32_t hwnd, int remove) {
    uint32_t now = platform_ticks_ms();
    for (int i = 0; i < MAX_TIMER; i++) {
        if (!g_timers[i].used || (int32_t)(now - g_timers[i].next) < 0) continue;
        if (hwnd && hwnd != 0xffffffffu && g_timers[i].hwnd != hwnd && !wm_is_child_of(g_timers[i].hwnd, hwnd)) continue;
        if (remove) g_timers[i].next = now + g_timers[i].ms;
        *m = (WinMsg){g_timers[i].hwnd, WM_TIMER, g_timers[i].id, g_timers[i].proc, now, (uint32_t)g_mouse_x, (uint32_t)g_mouse_y};
        return 1;
    }
    return 0;
}

int wm_get_message(CPU *cpu, WinMsg *m, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove, int wait) {
    for (;;) {
        wm_message_loop_idle(cpu);
        pthread_mutex_lock(&g_qlock);
        for (int i = 0; i < g_qn; i++)
            if (msg_matches(&g_q[i], hwnd, lo, hi)) {
                *m = g_q[i];
                if (remove) {
                    memmove(&g_q[i], &g_q[i + 1], sizeof(WinMsg) * (size_t)(g_qn - i - 1));
                    g_qn--;
                }
                pthread_mutex_unlock(&g_qlock);
                /* messages for windows that are gone are dropped */
                if (m->hwnd && !wnd_get(m->hwnd)) { if (remove) goto again; else return 0; }
                g_msg_time = m->time;
                g_msg_pos = (m->x & 0xffff) | m->y << 16;
                return 1;
            }
        pthread_mutex_unlock(&g_qlock);
        if (g_quit_posted && (!lo && !hi)) {
            *m = (WinMsg){0, WM_QUIT, g_quit_code, 0, platform_ticks_ms(), 0, 0};
            if (remove) g_quit_posted = 0;
            return 1;
        }
        if ((!lo && !hi) || (WM_PAINT >= lo && WM_PAINT <= hi))
            if (find_paint(m, hwnd)) return 1;
        if ((!lo && !hi) || (WM_TIMER >= lo && WM_TIMER <= hi))
            if (find_timer(m, hwnd, remove)) return 1;
        if (!wait) return 0;
        if (platform_quit_requested()) {
            static int closing;
            if (!closing++) user32_post_close();
        }
        platform_wait_events(10);
    again:;
    }
}

uint32_t wm_dispatch(CPU *cpu, const WinMsg *m) {
    if (m->message == WM_TIMER && m->lparam) {
        uint32_t args[4] = {m->hwnd, WM_TIMER, m->wparam, platform_ticks_ms()};
        return guest_call(cpu, m->lparam, 4, args);
    }
    if (!m->hwnd) return 0;
    if (m->message == WM_PAINT) {
        Wnd *w = wnd_get(m->hwnd);
        uint32_t r = wm_send(cpu, m->hwnd, WM_PAINT, 0, 0);
        w = wnd_get(m->hwnd);
        if (w && w->invalid) { w->invalid = 0; w->erase = 0; }
        return r;
    }
    return wm_send(cpu, m->hwnd, m->message, m->wparam, m->lparam);
}

static void write_msg(uint32_t p, const WinMsg *m) {
    WR32(p + 0, m->hwnd);
    WR32(p + 4, m->message);
    WR32(p + 8, m->wparam);
    WR32(p + 12, m->lparam);
    WR32(p + 16, m->time);
    WR32(p + 20, m->x);
    WR32(p + 24, m->y);
}

void imp_USER32_PeekMessageA(CPU *cpu) {
    WinMsg m;
    if (wm_get_message(cpu, &m, ARG(1), ARG(2), ARG(3), ARG(4) & 1, 0)) {
        write_msg(ARG(0), &m);
        RET_STDCALL(5, 1);
    }
    RET_STDCALL(5, 0);
}

void imp_USER32_GetMessageA(CPU *cpu) {
    WinMsg m;
    wm_get_message(cpu, &m, ARG(1), ARG(2), ARG(3), 1, 1);
    write_msg(ARG(0), &m);
    RET_STDCALL(4, m.message != WM_QUIT);
}

void imp_USER32_WaitMessage(CPU *cpu) {
    WinMsg m;
    while (!wm_get_message(cpu, &m, 0, 0, 0, 0, 0)) platform_wait_events(10);
    RET_STDCALL(0, 1);
}

void imp_USER32_DispatchMessageA(CPU *cpu) {
    uint32_t p = ARG(0);
    WinMsg m = {RD32(p), RD32(p + 4), RD32(p + 8), RD32(p + 12), RD32(p + 16), RD32(p + 20), RD32(p + 24)};
    uint32_t r = wm_dispatch(cpu, &m);
    RET_STDCALL(1, r);
}

void imp_USER32_PostMessageA(CPU *cpu) {
    uint32_t h = ARG(0);
    if (h == 0xffff) {
        for (int i = 0; i < MAX_WND; i++)
            if (g_wnd[i].used && is_toplevel(&g_wnd[i])) wm_post(g_wnd[i].hwnd, ARG(1), ARG(2), ARG(3));
    } else {
        wm_post(h, ARG(1), ARG(2), ARG(3));
    }
    RET_STDCALL(4, 1);
}

void imp_USER32_PostThreadMessageA(CPU *cpu) {
    wm_post(0, ARG(1), ARG(2), ARG(3));
    RET_STDCALL(4, 1);
}

void imp_USER32_PostQuitMessage(CPU *cpu) {
    g_quit_posted = 1;
    g_quit_code = ARG(0);
    RET_STDCALL(1, 0);
}

void imp_USER32_SendMessageTimeoutA(CPU *cpu) {
    uint32_t r = wm_send(cpu, ARG(0), ARG(1), ARG(2), ARG(3));
    if (ARG(6)) WR32(ARG(6), r);
    RET_STDCALL(7, 1);
}

void imp_USER32_SendNotifyMessageA(CPU *cpu) {
    wm_post(ARG(0) == 0xffff ? 0 : ARG(0), ARG(1), ARG(2), ARG(3));
    RET_STDCALL(4, 1);
}

/* ---- DefWindowProc --------------------------------------------------------------------------------------------- */
uint32_t wm_def_proc(CPU *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Wnd *w = wnd_get(hwnd);
    if (!w) return 0;
    switch (msg) {
    case WM_NCCREATE:
        if (lp && RD32(lp + 36)) wm_set_text(hwnd, gstr(RD32(lp + 36)));
        return 1;
    case WM_SETTEXT:
        wm_set_text(hwnd, lp ? gstr(lp) : "");
        wm_invalidate(hwnd, NULL, 1);
        return 1;
    case WM_GETTEXT: {
        const char *t = wm_text(hwnd);
        if (!wp) return 0;
        size_t n = strlen(t);
        if (n >= wp) n = wp - 1;
        memcpy(GPTR(lp), t, n);
        WR8(lp + n, 0);
        return (uint32_t)n;
    }
    case WM_GETTEXTLENGTH: return (uint32_t)strlen(wm_text(hwnd));
    case WM_CLOSE:
        wm_destroy(cpu, hwnd);
        return 0;
    case WM_ERASEBKGND: {
        uint32_t br = w->cls ? w->cls->hbrush : 0;
        if (!br) return 0;
        Rect r;
        wm_client_rect(w, &r);
        gdi_fill_rect_brush(wp, r, br);
        return 1;
    }
    case WM_PAINT: {
        w->invalid = 0;
        w->erase = 0;
        return 0;
    }
    case WM_SETCURSOR: return 0;
    case WM_MOUSEACTIVATE: return 1;
    case WM_NCHITTEST: return 1;
    case WM_SETFONT: w->font = wp; return 0;
    case WM_GETFONT: return w->font;
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC:
        gdi_set_text_color(wp, sys_color(8));
        if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) return sys_color_brush(5);
        return sys_color_brush(15);
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == 0xf060) wm_send(cpu, hwnd, WM_CLOSE, 0, 0); /* SC_CLOSE */
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == 0x73 && input_vk_down(0x12)) wm_post(hwnd, WM_SYSCOMMAND, 0xf060, 0); /* Alt+F4 */
        return 0;
    case WM_WINDOWPOSCHANGED: return 0;
    case WM_ACTIVATE:
        if ((wp & 0xffff) && !(w->style & WS_CHILD) && !w->is_dialog) wm_set_focus(cpu, hwnd);
        return 0;
    case WM_NCACTIVATE: return 1;
    }
    return 0;
}

void imp_USER32_DefWindowProcA(CPU *cpu) { RET_STDCALL(4, wm_def_proc(cpu, ARG(0), ARG(1), ARG(2), ARG(3))); }

/* ---- menus (data only: the fullscreen game never shows its menu bar) -------------------------------------- */
#define MAX_MENU 64
typedef struct { uint32_t id, flags, sub; char *text; } MenuItem;
static struct { int used; MenuItem *it; int n; } g_menus[MAX_MENU];
#define MENU_BASE 0x06000000u

static uint32_t menu_new(void) {
    for (int i = 1; i < MAX_MENU; i++)
        if (!g_menus[i].used) {
            g_menus[i].used = 1;
            g_menus[i].it = NULL;
            g_menus[i].n = 0;
            return MENU_BASE + (uint32_t)i * 4;
        }
    return 0;
}

static void *menu_get(uint32_t h) {
    if (h < MENU_BASE || h >= MENU_BASE + MAX_MENU * 4) return NULL;
    int i = (int)(h - MENU_BASE) / 4;
    return g_menus[i].used ? &g_menus[i] : NULL;
}

static uint32_t menu_parse(uint32_t *p) {
    uint32_t h = menu_new();
    if (!h) return 0;
    for (;;) {
        uint32_t flags = RD16(*p);
        *p += 2;
        uint32_t id = 0;
        if (!(flags & 0x10)) { id = RD16(*p); *p += 2; } /* MF_POPUP has no id */
        char text[256];
        int n = 0;
        uint16_t c;
        while ((c = RD16(*p))) { if (n < 255) text[n++] = (char)c; *p += 2; }
        *p += 2;
        text[n] = 0;
        MenuItem mi = {id, flags & ~0x80u, 0, strdup(text)};
        if (flags & 0x10) mi.sub = menu_parse(p);
        int mi_idx = (int)(h - MENU_BASE) / 4;
        g_menus[mi_idx].it = realloc(g_menus[mi_idx].it, sizeof(MenuItem) * (size_t)(g_menus[mi_idx].n + 1));
        g_menus[mi_idx].it[g_menus[mi_idx].n++] = mi;
        if (flags & 0x80) break; /* MF_END */
    }
    return h;
}

void imp_USER32_LoadMenuA(CPU *cpu) {
    uint32_t e = res_find(4, ARG(1));
    if (!e) RET_STDCALL(2, 0);
    uint32_t p = res_data(e, NULL);
    p += 4 + RD16(p + 2); /* MENUHEADER: version, offset */
    RET_STDCALL(2, menu_parse(&p));
}
void imp_USER32_GetMenu(CPU *cpu) { Wnd *w = wnd_get(ARG(0)); RET_STDCALL(1, w ? w->menu : 0); }
void imp_USER32_SetMenu(CPU *cpu) { Wnd *w = wnd_get(ARG(0)); if (w) w->menu = ARG(1); RET_STDCALL(2, 1); }
void imp_USER32_DestroyMenu(CPU *cpu) {
    uint32_t h = ARG(0);
    if (menu_get(h)) {
        int i = (int)(h - MENU_BASE) / 4;
        for (int k = 0; k < g_menus[i].n; k++) free(g_menus[i].it[k].text);
        free(g_menus[i].it);
        g_menus[i].used = 0;
    }
    RET_STDCALL(1, 1);
}
static MenuItem *menu_item(uint32_t h, uint32_t item, uint32_t flags) {
    if (!menu_get(h)) return NULL;
    int i = (int)(h - MENU_BASE) / 4;
    if (flags & 0x400) /* MF_BYPOSITION */
        return item < (uint32_t)g_menus[i].n ? &g_menus[i].it[item] : NULL;
    for (int k = 0; k < g_menus[i].n; k++) {
        if (g_menus[i].it[k].id == item && !g_menus[i].it[k].sub) return &g_menus[i].it[k];
        if (g_menus[i].it[k].sub) {
            MenuItem *m = menu_item(g_menus[i].it[k].sub, item, flags);
            if (m) return m;
        }
    }
    return NULL;
}
void imp_USER32_GetSubMenu(CPU *cpu) { MenuItem *m = menu_item(ARG(0), ARG(1), 0x400); RET_STDCALL(2, m ? m->sub : 0); }
void imp_USER32_GetMenuItemCount(CPU *cpu) {
    uint32_t h = ARG(0);
    RET_STDCALL(1, menu_get(h) ? (uint32_t)g_menus[(h - MENU_BASE) / 4].n : 0xffffffffu);
}
void imp_USER32_GetMenuItemID(CPU *cpu) {
    MenuItem *m = menu_item(ARG(0), ARG(1), 0x400);
    RET_STDCALL(2, !m ? 0xffffffffu : m->sub ? 0xffffffffu : m->id);
}
void imp_USER32_GetMenuState(CPU *cpu) {
    MenuItem *m = menu_item(ARG(0), ARG(1), ARG(2));
    RET_STDCALL(3, m ? m->flags : 0xffffffffu);
}
void imp_USER32_EnableMenuItem(CPU *cpu) {
    MenuItem *m = menu_item(ARG(0), ARG(1), ARG(2));
    if (!m) RET_STDCALL(3, 0xffffffffu);
    uint32_t old = m->flags & 3;
    m->flags = (m->flags & ~3u) | (ARG(2) & 3);
    RET_STDCALL(3, old);
}
void imp_USER32_CheckMenuItem(CPU *cpu) {
    MenuItem *m = menu_item(ARG(0), ARG(1), ARG(2));
    if (!m) RET_STDCALL(3, 0xffffffffu);
    uint32_t old = m->flags & 8;
    m->flags = (m->flags & ~8u) | (ARG(2) & 8);
    RET_STDCALL(3, old);
}
void imp_USER32_ModifyMenuA(CPU *cpu) {
    MenuItem *m = menu_item(ARG(0), ARG(1), ARG(2));
    if (m) {
        m->flags = ARG(2) & ~0x400u;
        m->id = ARG(3);
        if (!(ARG(2) & 0x804) && ARG(4) >= 0x10000) { free(m->text); m->text = strdup(gstr(ARG(4))); }
    }
    RET_STDCALL(5, m != NULL);
}
void imp_USER32_SetMenuItemBitmaps(CPU *cpu) { RET_STDCALL(5, 1); }
void imp_USER32_GetMenuCheckMarkDimensions(CPU *cpu) { RET_STDCALL(0, 13 | 13 << 16); }

/* ---- accelerators ----------------------------------------------------------------------------------------------- */
void imp_USER32_LoadAcceleratorsA(CPU *cpu) {
    uint32_t e = res_find(9, ARG(1));
    RET_STDCALL(2, e ? res_data(e, NULL) : 0);
}
void imp_USER32_DestroyAcceleratorTable(CPU *cpu) { RET_STDCALL(1, 1); }

/* TranslateAcceleratorA(hwnd, haccel, msg) */
void imp_USER32_TranslateAcceleratorA(CPU *cpu) {
    uint32_t hwnd = ARG(0), tab = ARG(1), mp = ARG(2);
    uint32_t msg = RD32(mp + 4), key = RD32(mp + 8);
    if (!tab || (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN && msg != WM_CHAR && msg != WM_SYSCHAR)) RET_STDCALL(3, 0);
    int shift = input_vk_down(0x10), ctrl = input_vk_down(0x11), alt = input_vk_down(0x12);
    for (uint32_t p = tab;; p += 8) {
        uint32_t f = RD16(p), k = RD16(p + 2), id = RD16(p + 4);
        int virt = f & 1;
        int match = 0;
        if (virt && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && k == key)
            match = !!(f & 4) == shift && !!(f & 8) == ctrl && !!(f & 0x10) == alt;
        else if (!virt && (msg == WM_CHAR || msg == WM_SYSCHAR) && k == key)
            match = !!(f & 0x10) == alt;
        if (match) {
            wm_send(cpu, hwnd, WM_COMMAND, id | 0x10000u, 0);
            RET_STDCALL(3, 1);
        }
        if (f & 0x80) break;
    }
    RET_STDCALL(3, 0);
}

/* ---- resources --------------------------------------------------------------------------------------------------- */
void imp_USER32_LoadStringA(CPU *cpu) {
    uint32_t id = ARG(1), buf = ARG(2), n = ARG(3);
    char tmp[4096];
    int l = res_load_string(id, tmp, sizeof tmp);
    if (l < 0 || !n) { if (n) WR8(buf, 0); RET_STDCALL(4, 0); }
    if ((uint32_t)l >= n) l = (int)n - 1;
    memcpy(GPTR(buf), tmp, (size_t)l);
    WR8(buf + (uint32_t)l, 0);
    RET_STDCALL(4, (uint32_t)l);
}

static void res_name_str(uint32_t v, char *out, size_t n) {
    if (v < 0x10000) snprintf(out, n, "#%u", v);
    else snprintf(out, n, "%s", gstr(v));
}

void imp_USER32_LoadIconA(CPU *cpu) {
    char nm[256];
    res_name_str(ARG(1), nm, sizeof nm);
    uint32_t h = ARG(0) ? gdi_load_icon(nm) : 0;
    RET_STDCALL(2, h ? h : 0x5001);
}
void imp_USER32_LoadCursorA(CPU *cpu) { RET_STDCALL(2, 0x5002); }
void imp_USER32_SetCursor(CPU *cpu) { RET_STDCALL(1, 0x5002); }
void imp_USER32_DestroyCursor(CPU *cpu) { RET_STDCALL(1, 1); }
void imp_USER32_DestroyIcon(CPU *cpu) { RET_STDCALL(1, 1); }
void imp_USER32_DrawIcon(CPU *cpu) { RET_STDCALL(4, gdi_draw_icon_handle(ARG(0), (int)ARG(1), (int)ARG(2), ARG(3))); }
uint32_t gdi_load_bitmap(const char *name);
void imp_USER32_LoadBitmapA(CPU *cpu) {
    char nm[256];
    res_name_str(ARG(1), nm, sizeof nm);
    RET_STDCALL(2, gdi_load_bitmap(nm));
}
static int g_cursor_count;
void imp_USER32_ShowCursor(CPU *cpu) {
    g_cursor_count += ARG(0) ? 1 : -1;
    RET_STDCALL(1, (uint32_t)g_cursor_count);
}

/* ---- misc ------------------------------------------------------------------------------------------------------- */
void imp_USER32_MessageBeep(CPU *cpu) { RET_STDCALL(1, 1); }
void imp_USER32_WinHelpA(CPU *cpu) { RET_STDCALL(4, 1); }
void imp_USER32_CharUpperA(CPU *cpu) {
    uint32_t p = ARG(0);
    if (p < 0x10000) RET_STDCALL(1, (uint32_t)toupper((int)p));
    for (char *s = gstr(p); *s; s++) *s = (char)toupper((unsigned char)*s);
    RET_STDCALL(1, p);
}
void imp_USER32_ChangeDisplaySettingsA(CPU *cpu) {
    uint32_t dm = ARG(0);
    if (dm) rt_log("ChangeDisplaySettingsA(%ux%u %ubpp)", RD32(dm + 108), RD32(dm + 112), RD32(dm + 104));
    RET_STDCALL(2, 0);
}

void imp_USER32_GetSystemMetrics(CPU *cpu) {
    uint32_t v = 0;
    switch (ARG(0)) {
    case 0: v = (uint32_t)desktop_canvas()->w; break;  /* SM_CXSCREEN */
    case 1: v = (uint32_t)desktop_canvas()->h; break;  /* SM_CYSCREEN */
    case 2: case 3: v = 16; break;                     /* SM_CXVSCROLL, SM_CYHSCROLL */
    case 4: v = 18; break;                             /* SM_CYCAPTION */
    case 5: case 6: v = 1; break;                      /* SM_CXBORDER */
    case 7: case 8: v = 3; break;                      /* SM_CXDLGFRAME */
    case 11: case 12: v = 32; break;                   /* SM_CXICON */
    case 13: case 14: v = 32; break;                   /* SM_CXCURSOR */
    case 15: v = 19; break;                            /* SM_CYMENU */
    case 16: v = (uint32_t)desktop_canvas()->w; break; /* SM_CXFULLSCREEN */
    case 17: v = (uint32_t)desktop_canvas()->h; break;
    case 19: v = 1; break;                             /* SM_MOUSEPRESENT */
    case 32: case 33: v = 4; break;                    /* SM_CXFRAME */
    case 36: case 37: v = 4; break;                    /* SM_CXDOUBLECLK */
    case 43: v = 3; break;                             /* SM_CMOUSEBUTTONS */
    case 45: case 46: v = 2; break;                    /* SM_CXEDGE */
    case 49: case 50: v = 16; break;                   /* SM_CXSMICON */
    case 61: case 62: v = (uint32_t)desktop_canvas()->w; break; /* SM_CXMAXIMIZED */
    case 75: v = 1; break;                             /* SM_MOUSEWHEELPRESENT */
    case 80: v = 1; break;                             /* SM_CMONITORS */
    }
    RET_STDCALL(1, v);
}

void imp_USER32_SystemParametersInfoA(CPU *cpu) {
    uint32_t action = ARG(0), pv = ARG(2);
    if (action == 0x30 && pv) write_rect(pv, (Rect){0, 0, desktop_canvas()->w, desktop_canvas()->h}); /* SPI_GETWORKAREA */
    else if (action == 0x29 && pv) { /* SPI_GETNONCLIENTMETRICS: zero is acceptable for MFC */
        uint32_t sz = RD32(pv);
        memset(GPTR(pv + 4), 0, sz > 4 ? sz - 4 : 0);
    }
    RET_STDCALL(4, 1);
}

int platform_message_box_ex(const char *caption, const char *text, uint32_t type);
void imp_USER32_MessageBoxA(CPU *cpu) {
    const char *text = ARG(1) ? gstr(ARG(1)) : "", *cap = ARG(2) ? gstr(ARG(2)) : cfg_str("game", "title", "Game");
    rt_log("MessageBoxA(\"%s\", \"%s\", %x)", cap, text, ARG(3));
    RET_STDCALL(4, (uint32_t)platform_message_box_ex(cap, text, ARG(3)));
}

/* wsprintfA(buf, fmt, ...) -- cdecl, args on the guest stack */
static int guest_vformat(char *out, int outsz, const char *fmt, uint32_t argp) {
    int o = 0;
    for (const char *f = fmt; *f && o < outsz - 1; f++) {
        if (*f != '%') { out[o++] = *f; continue; }
        f++;
        if (*f == '%') { out[o++] = '%'; continue; }
        char spec[32];
        int sl = 0;
        spec[sl++] = '%';
        while (*f && strchr("-+ #0", *f) && sl < 20) spec[sl++] = *f++;
        while (*f && isdigit((unsigned char)*f) && sl < 24) spec[sl++] = *f++;
        if (*f == '.') { spec[sl++] = *f++; while (*f && isdigit((unsigned char)*f) && sl < 28) spec[sl++] = *f++; }
        int is_long = 0;
        if (*f == 'l' || *f == 'h') { is_long = *f == 'l'; f++; }
        (void)is_long;
        char conv = *f;
        if (!conv) break;
        char tmp[1024];
        if (conv == 's' || conv == 'S') {
            spec[sl++] = 's'; spec[sl] = 0;
            uint32_t sp = RD32(argp); argp += 4;
            snprintf(tmp, sizeof tmp, spec, sp ? gstr(sp) : "(null)");
        } else if (conv == 'c' || conv == 'C') {
            spec[sl++] = 'c'; spec[sl] = 0;
            snprintf(tmp, sizeof tmp, spec, (int)(RD32(argp) & 0xff)); argp += 4;
        } else if (strchr("diuxXo", conv)) {
            spec[sl++] = conv; spec[sl] = 0;
            uint32_t v = RD32(argp); argp += 4;
            if (conv == 'd' || conv == 'i') snprintf(tmp, sizeof tmp, spec, (int)v);
            else snprintf(tmp, sizeof tmp, spec, v);
        } else {
            tmp[0] = 0;
        }
        for (char *t = tmp; *t && o < outsz - 1; t++) out[o++] = *t;
    }
    out[o] = 0;
    return o;
}

void imp_USER32_wsprintfA(CPU *cpu) {
    char buf[1025];
    int n = guest_vformat(buf, sizeof buf, gstr(ARG(1)), cpu->esp + 12);
    memcpy(GPTR(ARG(0)), buf, (size_t)n + 1);
    RET_CDECL((uint32_t)n);
}

void imp_USER32_wvsprintfA(CPU *cpu) {
    char buf[1025];
    int n = guest_vformat(buf, sizeof buf, gstr(ARG(1)), ARG(2));
    memcpy(GPTR(ARG(0)), buf, (size_t)n + 1);
    RET_STDCALL(3, (uint32_t)n);
}

/* LoadImageA(hinst, name, type, cx, cy, flags) */
uint32_t gdi_load_bitmap_file(const char *guest_path);
void imp_USER32_LoadImageA(CPU *cpu) {
    uint32_t name = ARG(1), type = ARG(2), flags = ARG(5);
    char nm[256];
    res_name_str(name, nm, sizeof nm);
    uint32_t r = 0;
    if (type == 0) r = (flags & 0x10) ? gdi_load_bitmap_file(nm) : gdi_load_bitmap(nm); /* IMAGE_BITMAP */
    else if (type == 1) r = gdi_load_icon(nm);                                         /* IMAGE_ICON */
    else if (type == 2) r = 0x5002;                                                     /* IMAGE_CURSOR */
    if (!r) rt_log("LoadImageA(%s, %u) failed", nm, type);
    RET_STDCALL(6, r);
}

void imp_USER32_GetKeyboardState(CPU *cpu) {
    for (int vk = 0; vk < 256; vk++) WR8(ARG(0) + (uint32_t)vk, (input_vk_down(vk) ? 0x80 : 0) | g_key_toggle[vk]);
    RET_STDCALL(1, 1);
}

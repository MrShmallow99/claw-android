/* Dialogs (DialogBoxParam, CreateDialogIndirectParam, DefDlgProc, IsDialogMessage)
 * and the standard controls: BUTTON, STATIC, EDIT, LISTBOX, COMBOBOX.  Controls
 * are host window procedures (thunks); they talk to the game through the usual
 * notifications (WM_COMMAND, WM_CTLCOLOR*, WM_DRAWITEM), and are usable with a
 * mouse, a touch screen (tap, drag to scroll) or a gamepad (arrow/Enter/Esc keys). */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "wnd.h"
#include "../pack/vfs.h"

#define IDOK 1
#define IDCANCEL 2
#define DWLP_MSGRESULT 0
#define DWLP_DLGPROC 4
#define DWLP_USER 8

void platform_text_input(int on);
uint32_t gdi_load_bitmap(const char *name);

static uint32_t rd_msgresult(Wnd *w) { uint32_t v; memcpy(&v, w->extra + DWLP_MSGRESULT, 4); return v; }
static uint32_t rd_dlgproc(Wnd *w) { uint32_t v; memcpy(&v, w->extra + DWLP_DLGPROC, 4); return v; }

#define CTL_PROC(name)                                                                      \
    static uint32_t name##_impl(CPU *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp); \
    static void name(CPU *cpu) {                                                            \
        uint32_t a0 = ARG(0), a1 = ARG(1), a2 = ARG(2), a3 = ARG(3);                        \
        uint32_t r = name##_impl(cpu, a0, a1, a2, a3);                                      \
        RET_STDCALL(4, r);                                                                  \
    }                                                                                       \
    static uint32_t name##_impl(CPU *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)

/* the font a control draws with */
static uint32_t ctl_font(Wnd *w) {
    if (w->font) return w->font;
    Wnd *p = wnd_get(w->parent);
    if (p && p->font) return p->font;
    return gdi_dialog_font(8);
}

/* parent's WM_CTLCOLORxxx: sets DC colours, returns the background brush */
static uint32_t ctl_color(CPU *cpu, Wnd *w, uint32_t dc, uint32_t msg) {
    gdi_set_text_color(dc, (w->style & WS_DISABLED) ? sys_color(17) : sys_color(8));
    extern void imp_GDI32_SetBkColor(CPU *);
    uint32_t br = w->parent ? wm_send(cpu, w->parent, msg, dc, w->hwnd) : 0;
    if (!br) br = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX ? sys_color_brush(5) : sys_color_brush(15);
    return br;
}

static uint32_t paint_begin(Wnd *w) {
    w->invalid = 0;
    w->erase = 0;
    uint32_t dc = gdi_dc_new(DC_WINDOW, w->hwnd, 0);
    gdi_select(dc, ctl_font(w));
    return dc;
}

static void paint_end(uint32_t dc) { gdi_dc_free(dc); }

/* DRAWITEMSTRUCT for owner-drawn controls */
static void send_drawitem(CPU *cpu, Wnd *w, uint32_t type, uint32_t item, uint32_t state, uint32_t dc, Rect r,
                          uint32_t data) {
    uint32_t dis = heap_alloc(48, 1);
    WR32(dis, type);
    WR32(dis + 4, w->id);
    WR32(dis + 8, item);
    WR32(dis + 12, 1); /* ODA_DRAWENTIRE */
    WR32(dis + 16, state);
    WR32(dis + 20, w->hwnd);
    WR32(dis + 24, dc);
    WR32(dis + 28, (uint32_t)r.l); WR32(dis + 32, (uint32_t)r.t); WR32(dis + 36, (uint32_t)r.r); WR32(dis + 40, (uint32_t)r.b);
    WR32(dis + 44, data);
    wm_send(cpu, w->parent, WM_DRAWITEM, w->id, dis);
    heap_free(dis);
}

static int lp_x(uint32_t lp) { return (int16_t)(lp & 0xffff); }
static int lp_y(uint32_t lp) { return (int16_t)(lp >> 16); }

/* ===================================================================================== BUTTON */
typedef struct { int checked, pushed, captured; } Button;

static int btn_type(Wnd *w) { return (int)(w->style & 0xf); }

static void btn_click(CPU *cpu, Wnd *w) {
    Button *b = w->ctl;
    int t = btn_type(w);
    if (t == 3) b->checked = !b->checked;              /* BS_AUTOCHECKBOX */
    else if (t == 6) b->checked = (b->checked + 1) % 3; /* BS_AUTO3STATE */
    else if (t == 9) {                                  /* BS_AUTORADIOBUTTON: uncheck the group */
        for (uint32_t c = wm_first_child(w->parent); c; c = wm_next_sibling(c)) {
            Wnd *o = wnd_get(c);
            if (o && o != w && o->ctl && !strcasecmp(o->cls->name, "BUTTON") && btn_type(o) == 9) {
                ((Button *)o->ctl)->checked = 0;
                wm_invalidate(c, NULL, 1);
            }
        }
        b->checked = 1;
    }
    wm_invalidate(w->hwnd, NULL, 1);
    wm_notify_parent(cpu, w, 0); /* BN_CLICKED */
}

static void btn_paint(CPU *cpu, Wnd *w) {
    Button *b = w->ctl;
    uint32_t dc = paint_begin(w);
    Rect r;
    wm_client_rect(w, &r);
    int t = btn_type(w);
    const char *text = w->text ? w->text : "";
    int focused = wm_focus() == w->hwnd;
    if (t == 0xb) { /* BS_OWNERDRAW */
        send_drawitem(cpu, w, 4, 0, (b->pushed ? 1 : 0) | (focused ? 0x10 : 0) | ((w->style & WS_DISABLED) ? 4 : 0), dc, r, 0);
        paint_end(dc);
        return;
    }
    if (t == 0 || t == 1) { /* push buttons */
        ctl_color(cpu, w, dc, WM_CTLCOLORBTN);
        gdi_set_text_color(dc, (w->style & WS_DISABLED) ? sys_color(17) : sys_color(18));
        gdi_fill_rect_rgb(dc, r, sys_color(15));
        Rect e = r;
        if (t == 1 || focused) { gdi_frame_rect_rgb(dc, e, 0); e.l++; e.t++; e.r--; e.b--; }
        if (b->pushed) gdi_frame_rect_rgb(dc, e, sys_color(16));
        else gdi_draw_edge(dc, e, 1, 1);
        gdi_set_bk_mode(dc, 1);
        Rect tr = r;
        if (b->pushed) { tr.l += 2; tr.t += 2; }
        gdi_draw_text(dc, text, -1, &tr, 0x1 | 0x4 | 0x20);
        if (focused) {
            Rect f = {r.l + 4, r.t + 4, r.r - 4, r.b - 4};
            for (int x = f.l; x < f.r; x += 2) { gdi_fill_rect_rgb(dc, (Rect){x, f.t, x + 1, f.t + 1}, 0); gdi_fill_rect_rgb(dc, (Rect){x, f.b - 1, x + 1, f.b}, 0); }
            for (int y = f.t; y < f.b; y += 2) { gdi_fill_rect_rgb(dc, (Rect){f.l, y, f.l + 1, y + 1}, 0); gdi_fill_rect_rgb(dc, (Rect){f.r - 1, y, f.r, y + 1}, 0); }
        }
        paint_end(dc);
        return;
    }
    uint32_t br = ctl_color(cpu, w, dc, WM_CTLCOLORSTATIC);
    gdi_fill_rect_brush(dc, r, br);
    gdi_set_bk_mode(dc, 1);
    if (t == 7) { /* BS_GROUPBOX */
        int fh = gdi_font_height(dc);
        Rect fr = {r.l, r.t + fh / 2, r.r, r.b};
        gdi_frame_rect_rgb(dc, (Rect){fr.l + 1, fr.t + 1, fr.r, fr.b}, sys_color(20));
        gdi_frame_rect_rgb(dc, (Rect){fr.l, fr.t, fr.r - 1, fr.b - 1}, sys_color(16));
        int tw = gdi_text_width(dc, text, (int)strlen(text));
        Rect tr = {r.l + 8, r.t, r.l + 12 + tw, r.t + fh};
        gdi_fill_rect_brush(dc, tr, br);
        tr.l += 2;
        gdi_draw_text(dc, text, -1, &tr, 0x20);
        paint_end(dc);
        return;
    }
    /* check boxes and radio buttons */
    int bs = 13, by = (r.t + r.b - bs) / 2;
    Rect box = {r.l, by, r.l + bs, by + bs};
    int radio = t == 4 || t == 9;
    if (w->style & 0x20) { box.l = r.r - bs; box.r = r.r; } /* BS_LEFTTEXT */
    gdi_fill_rect_rgb(dc, box, b->pushed ? sys_color(15) : 0xffffff);
    if (radio) {
        gdi_frame_rect_rgb(dc, box, sys_color(16));
        if (b->checked) gdi_fill_rect_rgb(dc, (Rect){box.l + 4, box.t + 4, box.r - 4, box.b - 4}, 0);
    } else {
        gdi_draw_edge(dc, box, 0, 1);
        if (b->checked) {
            for (int i = 0; i < 7; i++) {
                int x = box.l + 3 + i, y = box.t + (i < 3 ? 5 + i : 9 - (i - 2));
                gdi_fill_rect_rgb(dc, (Rect){x, y, x + 1, y + 3}, b->checked == 2 ? sys_color(16) : 0);
            }
        }
    }
    Rect tr = r;
    if (w->style & 0x20) tr.r = box.l - 4;
    else tr.l = box.r + 4;
    gdi_draw_text(dc, text, -1, &tr, 0x4 | 0x20);
    paint_end(dc);
}

static void btn_free(void *p) { free(p); }

CTL_PROC(button_proc) {
    Wnd *w = wnd_get(hwnd);
    if (!w) return 0;
    if (!w->ctl) { w->ctl = calloc(1, sizeof(Button)); w->ctl_free = btn_free; }
    Button *b = w->ctl;
    int t = btn_type(w);
    switch (msg) {
    case WM_GETDLGCODE:
        if (t == 1) return DLGC_BUTTON | DLGC_DEFPUSHBUTTON;
        if (t == 0) return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON;
        if (t == 4 || t == 9) return DLGC_BUTTON | DLGC_RADIOBUTTON;
        if (t == 7) return DLGC_STATIC;
        return DLGC_BUTTON;
    case WM_PAINT: btn_paint(cpu, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (t == 7) return 0;
        wm_set_focus(cpu, hwnd);
        wm_set_capture(hwnd);
        b->captured = 1;
        b->pushed = 1;
        wm_invalidate(hwnd, NULL, 1);
        return 0;
    case WM_MOUSEMOVE:
        if (b->captured) {
            Rect r;
            wm_client_rect(w, &r);
            int in = lp_x(lp) >= 0 && lp_y(lp) >= 0 && lp_x(lp) < r.r && lp_y(lp) < r.b;
            if (in != b->pushed) { b->pushed = in; wm_invalidate(hwnd, NULL, 1); }
        }
        return 0;
    case WM_LBUTTONUP:
        if (b->captured) {
            b->captured = 0;
            wm_set_capture(0);
            int was = b->pushed;
            b->pushed = 0;
            wm_invalidate(hwnd, NULL, 1);
            if (was) btn_click(cpu, w);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == 0x20) { b->pushed = 1; wm_invalidate(hwnd, NULL, 1); }
        return 0;
    case WM_KEYUP:
        if (wp == 0x20 && b->pushed) { b->pushed = 0; btn_click(cpu, w); }
        return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE:
        wm_invalidate(hwnd, NULL, 1);
        return 0;
    case 0xf0: return (uint32_t)b->checked;                                     /* BM_GETCHECK */
    case 0xf1: b->checked = (int)wp; wm_invalidate(hwnd, NULL, 1); return 0;     /* BM_SETCHECK */
    case 0xf2: return (uint32_t)b->checked | (b->pushed ? 4 : 0) | (wm_focus() == hwnd ? 8 : 0); /* BM_GETSTATE */
    case 0xf3: b->pushed = wp != 0; wm_invalidate(hwnd, NULL, 1); return 0;      /* BM_SETSTATE */
    case 0xf4: w->style = (w->style & ~0xfu) | (wp & 0xf); wm_invalidate(hwnd, NULL, 1); return 0; /* BM_SETSTYLE */
    case 0xf5: btn_click(cpu, w); return 0;                                      /* BM_CLICK */
    case WM_SETTEXT: wm_def_proc(cpu, hwnd, msg, wp, lp); wm_invalidate(hwnd, NULL, 1); return 1;
    }
    return wm_def_proc(cpu, hwnd, msg, wp, lp);
}

/* ===================================================================================== STATIC */
typedef struct { uint32_t icon, bitmap; } Static;

static void static_free(void *p) { free(p); }

static void static_paint(CPU *cpu, Wnd *w) {
    Static *s = w->ctl;
    uint32_t dc = paint_begin(w);
    Rect r;
    wm_client_rect(w, &r);
    int t = (int)(w->style & 0x1f);
    if (t == 0xd) { /* SS_OWNERDRAW */
        send_drawitem(cpu, w, 5, 0, 0, dc, r, 0);
        paint_end(dc);
        return;
    }
    uint32_t br = ctl_color(cpu, w, dc, WM_CTLCOLORSTATIC);
    switch (t) {
    case 3: /* SS_ICON */
        gdi_fill_rect_brush(dc, r, br);
        if (s->icon) gdi_draw_icon_handle(dc, 0, 0, s->icon);
        break;
    case 0xe: { /* SS_BITMAP */
        gdi_fill_rect_brush(dc, r, br);
        if (s->bitmap) {
            uint32_t m = gdi_dc_new(DC_MEMORY, 0, 0);
            gdi_select(m, s->bitmap);
            extern void gdi_blit_simple(uint32_t dst, int x, int y, int w, int h, uint32_t src);
            gdi_blit_simple(dc, 0, 0, r.r, r.b, m);
            gdi_dc_free(m);
        }
        break;
    }
    case 4: gdi_fill_rect_rgb(dc, r, sys_color(21)); break;  /* SS_BLACKRECT */
    case 5: gdi_fill_rect_rgb(dc, r, sys_color(16)); break;  /* SS_GRAYRECT */
    case 6: gdi_fill_rect_rgb(dc, r, sys_color(20)); break;  /* SS_WHITERECT */
    case 7: gdi_frame_rect_rgb(dc, r, sys_color(21)); break; /* SS_BLACKFRAME */
    case 8: gdi_frame_rect_rgb(dc, r, sys_color(16)); break;
    case 9: gdi_frame_rect_rgb(dc, r, sys_color(20)); break;
    case 0x10: gdi_fill_rect_rgb(dc, (Rect){r.l, r.t, r.r, r.t + 1}, sys_color(16)); gdi_fill_rect_rgb(dc, (Rect){r.l, r.t + 1, r.r, r.t + 2}, sys_color(20)); break;
    case 0x11: gdi_fill_rect_rgb(dc, (Rect){r.l, r.t, r.l + 1, r.b}, sys_color(16)); gdi_fill_rect_rgb(dc, (Rect){r.l + 1, r.t, r.l + 2, r.b}, sys_color(20)); break;
    case 0x12:
        gdi_frame_rect_rgb(dc, (Rect){r.l + 1, r.t + 1, r.r, r.b}, sys_color(20));
        gdi_frame_rect_rgb(dc, (Rect){r.l, r.t, r.r - 1, r.b - 1}, sys_color(16));
        break;
    default: {
        int is_null;
        gdi_brush_color(br, &is_null);
        if (!is_null) gdi_fill_rect_brush(dc, r, br);
        uint32_t f = 0;
        if (t == 1) f |= 1;
        else if (t == 2) f |= 2;
        if (t == 0xb || t == 0xc) f |= 0x20; else f |= 0x10;
        if (w->style & 0x80) f |= 0x800;          /* SS_NOPREFIX */
        if (w->style & 0x200) f = (f & ~0x10u) | 0x20 | 0x4; /* SS_CENTERIMAGE */
        gdi_set_bk_mode(dc, 1);
        Rect tr = r;
        gdi_draw_text(dc, w->text ? w->text : "", -1, &tr, f);
    }
    }
    if (w->style & 0x1000) gdi_draw_edge(dc, r, 0, 0); /* SS_SUNKEN */
    paint_end(dc);
}

CTL_PROC(static_proc) {
    Wnd *w = wnd_get(hwnd);
    if (!w) return 0;
    if (!w->ctl) { w->ctl = calloc(1, sizeof(Static)); w->ctl_free = static_free; }
    Static *s = w->ctl;
    switch (msg) {
    case WM_CREATE: {
        int t = (int)(w->style & 0x1f);
        if (t == 3 && w->text && w->text[0]) {
            s->icon = gdi_load_icon(w->text);
            if (s->icon && !(w->style & 0x200)) { w->w = w->w < 32 ? 32 : w->w; w->h = w->h < 32 ? 32 : w->h; }
        } else if (t == 0xe && w->text && w->text[0]) {
            s->bitmap = gdi_load_bitmap(w->text);
        }
        return 0;
    }
    case WM_PAINT: static_paint(cpu, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_GETDLGCODE: return DLGC_STATIC;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (w->style & 0x100) wm_notify_parent(cpu, w, msg == WM_LBUTTONDOWN ? 0 : 1); /* STN_CLICKED / DBLCLK */
        return 0;
    case 0x170: { uint32_t o = s->icon; s->icon = wp; wm_invalidate(hwnd, NULL, 1); return o; } /* STM_SETICON */
    case 0x171: return s->icon;                                                                 /* STM_GETICON */
    case 0x172: {                                                                               /* STM_SETIMAGE */
        uint32_t o = wp == 0 ? s->bitmap : s->icon;
        if (wp == 0) s->bitmap = lp; else s->icon = lp;
        wm_invalidate(hwnd, NULL, 1);
        return o;
    }
    case 0x173: return wp == 0 ? s->bitmap : s->icon; /* STM_GETIMAGE */
    case WM_SETTEXT: wm_def_proc(cpu, hwnd, msg, wp, lp); wm_invalidate(hwnd, NULL, 1); return 1;
    case WM_ENABLE: wm_invalidate(hwnd, NULL, 1); return 0;
    }
    return wm_def_proc(cpu, hwnd, msg, wp, lp);
}

/* ===================================================================================== EDIT */
typedef struct {
    char *buf;
    int len, cap, limit;
    int caret, anchor, scroll;
    int modified, readonly;
    char pass;
} Edit;

static void edit_free(void *p) {
    Edit *e = p;
    free(e->buf);
    free(e);
}

static void edit_set(Edit *e, const char *s) {
    int n = (int)strlen(s);
    if (n + 1 > e->cap) { e->cap = n + 64; e->buf = realloc(e->buf, (size_t)e->cap); }
    memcpy(e->buf, s, (size_t)n + 1);
    e->len = n;
    e->caret = e->anchor = n;
    e->scroll = 0;
}

static void edit_sel(Edit *e, int *a, int *b) {
    *a = e->caret < e->anchor ? e->caret : e->anchor;
    *b = e->caret < e->anchor ? e->anchor : e->caret;
}

static void edit_replace(CPU *cpu, Wnd *w, const char *s, int notify) {
    Edit *e = w->ctl;
    int a, b;
    edit_sel(e, &a, &b);
    int n = (int)strlen(s);
    if (e->len - (b - a) + n > e->limit) n = e->limit - (e->len - (b - a));
    if (n < 0) n = 0;
    int nl = e->len - (b - a) + n;
    if (nl + 1 > e->cap) { e->cap = nl + 64; e->buf = realloc(e->buf, (size_t)e->cap); }
    memmove(e->buf + a + n, e->buf + b, (size_t)(e->len - b + 1));
    memcpy(e->buf + a, s, (size_t)n);
    e->len = nl;
    e->caret = e->anchor = a + n;
    e->modified = 1;
    free(w->text);
    w->text = strdup(e->buf);
    wm_invalidate(w->hwnd, NULL, 1);
    if (notify) {
        wm_notify_parent(cpu, w, 0x400); /* EN_UPDATE */
        wm_notify_parent(cpu, w, 0x300); /* EN_CHANGE */
    }
}

static void edit_paint(CPU *cpu, Wnd *w) {
    Edit *e = w->ctl;
    uint32_t dc = paint_begin(w);
    Rect r;
    wm_client_rect(w, &r);
    uint32_t br = ctl_color(cpu, w, dc, (w->style & WS_DISABLED) || e->readonly ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT);
    gdi_fill_rect_brush(dc, r, br);
    char shown[4096];
    int n = e->len < 4095 ? e->len : 4095;
    for (int i = 0; i < n; i++) shown[i] = e->pass ? e->pass : e->buf[i];
    shown[n] = 0;
    int fh = gdi_font_height(dc);
    int ty = (r.b - fh) / 2;
    if (ty < 1) ty = 1;
    /* keep the caret visible */
    int cx = gdi_text_width(dc, shown, e->caret);
    if (cx - e->scroll > r.r - 4) e->scroll = cx - (r.r - 4);
    if (cx - e->scroll < 0) e->scroll = cx;
    int x0 = 2 - e->scroll;
    int a, b;
    edit_sel(e, &a, &b);
    int focused = wm_focus() == w->hwnd;
    gdi_set_bk_mode(dc, 1);
    Rect tr = {x0, ty, r.r + e->scroll + 4096, ty + fh};
    gdi_draw_text(dc, shown, n, &tr, 0x20 | 0x800 | 0x100);
    if (focused && b > a) {
        int sx = x0 + gdi_text_width(dc, shown, a), ex = x0 + gdi_text_width(dc, shown, b);
        gdi_fill_rect_rgb(dc, (Rect){sx, ty, ex, ty + fh}, sys_color(13));
        gdi_set_text_color(dc, sys_color(14));
        Rect sr = {sx, ty, ex + 2, ty + fh};
        gdi_draw_text(dc, shown + a, b - a, &sr, 0x20 | 0x800 | 0x100);
    }
    if (focused) {
        int x = x0 + cx;
        gdi_fill_rect_rgb(dc, (Rect){x, ty, x + 1, ty + fh}, 0);
    }
    paint_end(dc);
}

static int edit_hit(Wnd *w, int x) {
    Edit *e = w->ctl;
    uint32_t dc = gdi_dc_new(DC_WINDOW, w->hwnd, 0);
    gdi_select(dc, ctl_font(w));
    int best = e->len;
    for (int i = 0; i <= e->len; i++) {
        int cx = 2 - e->scroll + gdi_text_width(dc, e->buf, i);
        if (cx > x) {
            int px = 2 - e->scroll + gdi_text_width(dc, e->buf, i > 0 ? i - 1 : 0);
            best = (x - px < cx - x) && i > 0 ? i - 1 : i;
            break;
        }
    }
    gdi_dc_free(dc);
    return best;
}

CTL_PROC(edit_proc) {
    Wnd *w = wnd_get(hwnd);
    if (!w) return 0;
    if (!w->ctl) {
        Edit *ne = calloc(1, sizeof(Edit));
        ne->limit = 30000;
        ne->readonly = (w->style & 0x800) != 0; /* ES_READONLY */
        ne->pass = (w->style & 0x20) ? '*' : 0; /* ES_PASSWORD */
        edit_set(ne, w->text ? w->text : "");
        w->ctl = ne;
        w->ctl_free = edit_free;
    }
    Edit *e = w->ctl;
    int shift = input_vk_down(0x10);
    switch (msg) {
    case WM_GETDLGCODE: return DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS;
    case WM_PAINT: edit_paint(cpu, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETTEXT:
        edit_set(e, lp ? gstr(lp) : "");
        free(w->text);
        w->text = strdup(e->buf);
        e->modified = 0;
        wm_invalidate(hwnd, NULL, 1);
        return 1;
    case WM_GETTEXT: {
        if (!wp) return 0;
        int n = e->len < (int)wp - 1 ? e->len : (int)wp - 1;
        memcpy(GPTR(lp), e->buf, (size_t)n);
        WR8(lp + (uint32_t)n, 0);
        return (uint32_t)n;
    }
    case WM_GETTEXTLENGTH: return (uint32_t)e->len;
    case WM_SETFOCUS:
        wm_invalidate(hwnd, NULL, 1);
        wm_notify_parent(cpu, w, 0x100); /* EN_SETFOCUS */
        if (!e->readonly) platform_text_input(2); /* focus alone: no soft keyboard until tapped */
        return 0;
    case WM_KILLFOCUS:
        e->anchor = e->caret;
        wm_invalidate(hwnd, NULL, 1);
        wm_notify_parent(cpu, w, 0x200); /* EN_KILLFOCUS */
        platform_text_input(0);
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        wm_set_focus(cpu, hwnd);
        e->caret = edit_hit(w, lp_x(lp));
        if (!shift) e->anchor = e->caret;
        if (msg == WM_LBUTTONDBLCLK) { e->anchor = 0; e->caret = e->len; }
        wm_invalidate(hwnd, NULL, 1);
        if (!e->readonly) platform_text_input(1);
        return 0;
    case WM_CHAR:
        if (e->readonly) return 0;
        if (wp == 8) {
            int a, b;
            edit_sel(e, &a, &b);
            if (a == b && a > 0) e->anchor = a - 1;
            edit_replace(cpu, w, "", 1);
        } else if (wp >= 32 && wp != 127) {
            char s[2] = {(char)wp, 0};
            if (w->style & 0x8) s[0] = (char)toupper((int)wp); /* ES_UPPERCASE */
            if (w->style & 0x10) s[0] = (char)tolower((int)wp); /* ES_LOWERCASE */
            if ((w->style & 0x2000) && !isdigit((int)wp)) return 0; /* ES_NUMBER */
            edit_replace(cpu, w, s, 1);
        }
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case 0x25: if (e->caret > 0) e->caret--; break;
        case 0x27: if (e->caret < e->len) e->caret++; break;
        case 0x24: e->caret = 0; break;
        case 0x23: e->caret = e->len; break;
        case 0x2e:
            if (!e->readonly) {
                int a, b;
                edit_sel(e, &a, &b);
                if (a == b && b < e->len) e->caret = b + 1;
                edit_replace(cpu, w, "", 1);
            }
            return 0;
        default: return 0;
        }
        if (!shift) e->anchor = e->caret;
        wm_invalidate(hwnd, NULL, 1);
        return 0;
    case 0xb0: { /* EM_GETSEL */
        int a, b;
        edit_sel(e, &a, &b);
        if (wp) WR32(wp, (uint32_t)a);
        if (lp) WR32(lp, (uint32_t)b);
        return (uint32_t)a | (uint32_t)b << 16;
    }
    case 0xb1: { /* EM_SETSEL */
        int a = (int)wp, b = (int)lp;
        if (a < 0) { e->anchor = e->caret; wm_invalidate(hwnd, NULL, 1); return 0; }
        if (b < 0 || b > e->len) b = e->len;
        if (a > e->len) a = e->len;
        e->anchor = a;
        e->caret = b;
        wm_invalidate(hwnd, NULL, 1);
        return 0;
    }
    case 0xb8: return (uint32_t)e->modified;                   /* EM_GETMODIFY */
    case 0xb9: e->modified = wp != 0; return 0;                /* EM_SETMODIFY */
    case 0xba: return 1;                                       /* EM_GETLINECOUNT */
    case 0xc1: return (uint32_t)e->len;                        /* EM_LINELENGTH */
    case 0xc2: edit_replace(cpu, w, lp ? gstr(lp) : "", 0); return 0; /* EM_REPLACESEL */
    case 0xc4: {                                               /* EM_GETLINE */
        int n = RD16(lp);
        if (n > e->len) n = e->len;
        memcpy(GPTR(lp), e->buf, (size_t)n);
        return (uint32_t)n;
    }
    case 0xc5: e->limit = wp ? (int)wp : 0x7ffffffe; return 0; /* EM_LIMITTEXT */
    case 0xcc: e->pass = (char)wp; wm_invalidate(hwnd, NULL, 1); return 0; /* EM_SETPASSWORDCHAR */
    case 0xcf: e->readonly = wp != 0; wm_invalidate(hwnd, NULL, 1); return 1; /* EM_SETREADONLY */
    case WM_ENABLE: wm_invalidate(hwnd, NULL, 1); return 0;
    }
    return wm_def_proc(cpu, hwnd, msg, wp, lp);
}

/* soft-keyboard text (UTF-8) goes to the focused edit control as WM_CHAR */
void dlg_text_input(const char *u) {
    void user32_char_event(int ch);
    for (const unsigned char *p = (const unsigned char *)u; *p;) {
        int c = *p++;
        if (c >= 0xc0 && c < 0xe0 && *p) { c = ((c & 0x1f) << 6) | (*p++ & 0x3f); }
        else if (c >= 0xe0) { while (*p >= 0x80 && *p < 0xc0) p++; c = '?'; }
        if (c < 256) user32_char_event(c);
    }
}

/* ===================================================================================== LISTBOX */
typedef struct { char *text; uint32_t data; } LbItem;
typedef struct {
    LbItem *it;
    int n, cap;
    int sel, top, ih, caret;
    int drag_y, drag_top, dragging, down, sb_drag;
    uint32_t owner_combo; /* for the combo's drop-down list */
} ListBox;

static void lb_free(void *p) {
    ListBox *l = p;
    for (int i = 0; i < l->n; i++) free(l->it[i].text);
    free(l->it);
    free(l);
}

static int lb_visible_rows(Wnd *w, ListBox *l) {
    Rect r;
    wm_client_rect(w, &r);
    int n = r.b / (l->ih ? l->ih : 13);
    return n < 1 ? 1 : n;
}

static void lb_clamp_top(Wnd *w, ListBox *l) {
    int rows = lb_visible_rows(w, l);
    if (l->top > l->n - rows) l->top = l->n - rows;
    if (l->top < 0) l->top = 0;
}

static void lb_ensure_visible(Wnd *w, ListBox *l, int i) {
    int rows = lb_visible_rows(w, l);
    if (i < l->top) l->top = i;
    if (i >= l->top + rows) l->top = i - rows + 1;
    lb_clamp_top(w, l);
}

static int lb_insert(ListBox *l, int at, const char *s, int sorted) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->it = realloc(l->it, sizeof(LbItem) * (size_t)l->cap); }
    if (sorted) {
        at = 0;
        while (at < l->n && strcasecmp(l->it[at].text, s) <= 0) at++;
    } else if (at < 0 || at > l->n) {
        at = l->n;
    }
    memmove(&l->it[at + 1], &l->it[at], sizeof(LbItem) * (size_t)(l->n - at));
    l->it[at].text = strdup(s);
    l->it[at].data = 0;
    l->n++;
    if (l->sel >= at) l->sel++;
    return at;
}

static int lb_find(ListBox *l, int start, const char *s, int exact) {
    size_t n = strlen(s);
    for (int k = 1; k <= l->n; k++) {
        int i = (start + k) % (l->n ? l->n : 1);
        if (i < 0 || i >= l->n) continue;
        if (exact ? !strcasecmp(l->it[i].text, s) : !strncasecmp(l->it[i].text, s, n)) return i;
    }
    return -1;
}

static void lb_paint(CPU *cpu, Wnd *w, ListBox *l, int in_combo) {
    uint32_t dc = paint_begin(w);
    Rect r;
    wm_client_rect(w, &r);
    Wnd *ctl = in_combo ? wnd_get(l->owner_combo) : w;
    uint32_t br = ctl_color(cpu, ctl ? ctl : w, dc, WM_CTLCOLORLISTBOX);
    gdi_fill_rect_brush(dc, r, br);
    gdi_set_bk_mode(dc, 1);
    int focused = wm_focus() == w->hwnd || in_combo;
    int owner = !in_combo && (w->style & 0x30) && !(w->style & 0x40); /* owner-draw without LBS_HASSTRINGS */
    for (int i = l->top, y = 0; i < l->n && y < r.b; i++, y += l->ih) {
        Rect ir = {0, y, r.r, y + l->ih};
        if (owner || (!in_combo && (w->style & 0x30))) {
            send_drawitem(cpu, w, 2, (uint32_t)i, (i == l->sel ? 1 : 0) | (focused && i == l->caret ? 0x10 : 0), dc, ir, l->it[i].data);
            continue;
        }
        if (i == l->sel) {
            gdi_fill_rect_rgb(dc, ir, sys_color(13));
            gdi_set_text_color(dc, sys_color(14));
        } else {
            gdi_set_text_color(dc, sys_color(8));
        }
        Rect tr = {ir.l + 2, ir.t, ir.r - 2, ir.b};
        gdi_draw_text(dc, l->it[i].text, -1, &tr, 0x20 | 0x4 | 0x800);
    }
    paint_end(dc);
    /* vertical scroll bar in the non-client area */
    if (w->style & WS_VSCROLL) {
        uint32_t wdc = gdi_dc_new(DC_WINDOW, w->hwnd, 0);
        gdi_dc_set_window_part(wdc, 0);
        int b = (w->style & WS_BORDER) ? 1 : 0;
        if (w->exstyle & WS_EX_CLIENTEDGE) b += 2;
        Rect sb = {w->w - b - 16, b, w->w - b, w->h - b};
        gdi_fill_rect_rgb(wdc, sb, 0xe0e0e0);
        Rect up = {sb.l, sb.t, sb.r, sb.t + 16}, dn = {sb.l, sb.b - 16, sb.r, sb.b};
        gdi_fill_rect_rgb(wdc, up, sys_color(15)); gdi_draw_edge(wdc, up, 1, 1);
        gdi_fill_rect_rgb(wdc, dn, sys_color(15)); gdi_draw_edge(wdc, dn, 1, 1);
        for (int k = 0; k < 4; k++) {
            gdi_fill_rect_rgb(wdc, (Rect){up.l + 8 - k, up.t + 5 + k, up.l + 9 + k, up.t + 6 + k}, 0);
            gdi_fill_rect_rgb(wdc, (Rect){dn.l + 8 - k, dn.b - 6 - k, dn.l + 9 + k, dn.b - 5 - k}, 0);
        }
        int rows = lb_visible_rows(w, l), track = sb.b - sb.t - 32;
        if (l->n > rows && track > 8) {
            int th = track * rows / l->n;
            if (th < 10) th = 10;
            int ty = sb.t + 16 + (track - th) * l->top / (l->n - rows);
            Rect thumb = {sb.l, ty, sb.r, ty + th};
            gdi_fill_rect_rgb(wdc, thumb, sys_color(15));
            gdi_draw_edge(wdc, thumb, 1, 1);
        }
        if (b) gdi_frame_rect_rgb(wdc, (Rect){0, 0, w->w, w->h}, 0);
        gdi_dc_free(wdc);
    }
}

static void lb_select(CPU *cpu, Wnd *w, ListBox *l, int i, int notify) {
    if (i < 0 || i >= l->n) return;
    l->sel = l->caret = i;
    lb_ensure_visible(w, l, i);
    wm_invalidate(w->hwnd, NULL, 1);
    if (notify && (w->style & 1)) wm_notify_parent(cpu, w, 1); /* LBN_SELCHANGE */
}

static void combo_closeup(CPU *cpu, Wnd *combo, int accept);

/* shared by LISTBOX and the combo's drop-down ("ComboLBox") */
static uint32_t listbox_common(CPU *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, int dropdown) {
    Wnd *w = wnd_get(hwnd);
    if (!w) return 0;
    if (!w->ctl) {
        ListBox *nl = calloc(1, sizeof(ListBox));
        nl->sel = -1;
        uint32_t dc = gdi_dc_new(DC_WINDOW, hwnd, 0);
        gdi_select(dc, ctl_font(w));
        nl->ih = gdi_font_height(dc) + (dropdown ? 6 : 1);
        gdi_dc_free(dc);
        w->ctl = nl;
        w->ctl_free = lb_free;
    }
    ListBox *l = w->ctl;
    int sorted = (w->style & 2) && !dropdown;
    Rect cr;
    wm_client_rect(w, &cr);
    switch (msg) {
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_PAINT: lb_paint(cpu, w, l, dropdown); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS: case WM_KILLFOCUS: wm_invalidate(hwnd, NULL, 1); return 0;
    case WM_SETFONT: {
        w->font = wp;
        uint32_t dc = gdi_dc_new(DC_WINDOW, hwnd, 0);
        gdi_select(dc, wp);
        l->ih = gdi_font_height(dc) + (dropdown ? 6 : 1);
        gdi_dc_free(dc);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        if (!dropdown) wm_set_focus(cpu, hwnd);
        int x = lp_x(lp), y = lp_y(lp);
        if (dropdown && (x < 0 || y < 0 || x >= cr.r || y >= cr.b)) { /* click outside closes */
            combo_closeup(cpu, wnd_get(l->owner_combo), 0);
            return 0;
        }
        if (x >= cr.r && (w->style & WS_VSCROLL)) { /* scroll bar */
            int rows = lb_visible_rows(w, l);
            if (y < 16) l->top--;
            else if (y >= cr.b - 16) l->top++;
            else l->sb_drag = 1;
            lb_clamp_top(w, l);
            (void)rows;
            wm_set_capture(hwnd);
            wm_invalidate(hwnd, NULL, 1);
            return 0;
        }
        wm_set_capture(hwnd);
        l->down = 1;
        l->dragging = 0;
        l->drag_y = y;
        l->drag_top = l->top;
        if (msg == WM_LBUTTONDBLCLK && !dropdown) {
            int i = l->top + y / l->ih;
            if (i < l->n) { lb_select(cpu, w, l, i, 1); if (w->style & 1) wm_notify_parent(cpu, w, 2); } /* LBN_DBLCLK */
            l->down = 0;
            wm_set_capture(0);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int y = lp_y(lp);
        if (l->sb_drag) {
            int rows = lb_visible_rows(w, l), track = cr.b - 32;
            if (l->n > rows && track > 0) l->top = (y - 16) * (l->n - rows) / track;
            lb_clamp_top(w, l);
            wm_invalidate(hwnd, NULL, 1);
        } else if (l->down) {
            if (abs(y - l->drag_y) > l->ih / 2) l->dragging = 1;
            if (l->dragging) { /* touch-style drag scrolling */
                l->top = l->drag_top - (y - l->drag_y) / l->ih;
                lb_clamp_top(w, l);
                wm_invalidate(hwnd, NULL, 1);
            }
        } else if (dropdown && y >= 0 && y < cr.b) {
            int i = l->top + y / l->ih;
            if (i < l->n && i != l->sel) { l->sel = i; wm_invalidate(hwnd, NULL, 1); }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int y = lp_y(lp), x = lp_x(lp);
        wm_set_capture(dropdown ? hwnd : 0);
        if (l->sb_drag) { l->sb_drag = 0; return 0; }
        if (l->down && !l->dragging && y >= 0 && y < cr.b && x >= 0 && x < cr.r) {
            int i = l->top + y / l->ih;
            if (i < l->n) {
                lb_select(cpu, w, l, i, !dropdown);
                if (dropdown) { l->down = 0; combo_closeup(cpu, wnd_get(l->owner_combo), 1); return 0; }
            }
        }
        l->down = 0;
        l->dragging = 0;
        return 0;
    }
    case WM_MOUSEWHEEL:
        l->top -= (int16_t)(wp >> 16) / 40;
        lb_clamp_top(w, l);
        wm_invalidate(hwnd, NULL, 1);
        return 0;
    case WM_KEYDOWN: {
        int rows = lb_visible_rows(w, l), i = l->sel < 0 ? 0 : l->sel;
        switch (wp) {
        case 0x26: i--; break;
        case 0x28: i = l->sel < 0 ? 0 : i + 1; break;
        case 0x21: i -= rows; break;
        case 0x22: i += rows; break;
        case 0x24: i = 0; break;
        case 0x23: i = l->n - 1; break;
        case 0x0d: if (dropdown) combo_closeup(cpu, wnd_get(l->owner_combo), 1); return 0;
        case 0x1b: if (dropdown) combo_closeup(cpu, wnd_get(l->owner_combo), 0); return 0;
        default: return 0;
        }
        if (i < 0) i = 0;
        if (i >= l->n) i = l->n - 1;
        lb_select(cpu, w, l, i, !dropdown);
        return 0;
    }
    case WM_CHAR:
        if (wp > 32) {
            char s[2] = {(char)wp, 0};
            int i = lb_find(l, l->sel, s, 0);
            if (i >= 0) lb_select(cpu, w, l, i, !dropdown);
        }
        return 0;
    case 0x180: { int i = lb_insert(l, -1, lp ? ((w->style & 0x30) && !(w->style & 0x40) ? "" : gstr(lp)) : "", sorted); if ((w->style & 0x30) && !(w->style & 0x40)) l->it[i].data = lp; wm_invalidate(hwnd, NULL, 1); return (uint32_t)i; } /* LB_ADDSTRING */
    case 0x181: { int i = lb_insert(l, (int)wp, lp ? gstr(lp) : "", 0); wm_invalidate(hwnd, NULL, 1); return (uint32_t)i; } /* LB_INSERTSTRING */
    case 0x182: /* LB_DELETESTRING */
        if (wp >= (uint32_t)l->n) return 0xffffffffu;
        free(l->it[wp].text);
        memmove(&l->it[wp], &l->it[wp + 1], sizeof(LbItem) * (size_t)(l->n - (int)wp - 1));
        l->n--;
        if (l->sel == (int)wp) l->sel = -1;
        else if (l->sel > (int)wp) l->sel--;
        lb_clamp_top(w, l);
        wm_invalidate(hwnd, NULL, 1);
        return (uint32_t)l->n;
    case 0x184: /* LB_RESETCONTENT */
        for (int i = 0; i < l->n; i++) free(l->it[i].text);
        l->n = 0; l->sel = -1; l->top = 0;
        wm_invalidate(hwnd, NULL, 1);
        return 0;
    case 0x185: if (wp) lb_select(cpu, w, l, (int)lp, 0); return 0;       /* LB_SETSEL */
    case 0x186: /* LB_SETCURSEL */
        if ((int32_t)wp < 0 || wp >= (uint32_t)l->n) { l->sel = -1; wm_invalidate(hwnd, NULL, 1); return 0xffffffffu; }
        lb_select(cpu, w, l, (int)wp, 0);
        return wp;
    case 0x187: return wp < (uint32_t)l->n && (int)wp == l->sel;           /* LB_GETSEL */
    case 0x188: return (uint32_t)l->sel;                                   /* LB_GETCURSEL */
    case 0x189: /* LB_GETTEXT */
        if (wp >= (uint32_t)l->n) return 0xffffffffu;
        if ((w->style & 0x30) && !(w->style & 0x40)) { WR32(lp, l->it[wp].data); return 4; }
        strcpy(gstr(lp), l->it[wp].text);
        return (uint32_t)strlen(l->it[wp].text);
    case 0x18a: return wp < (uint32_t)l->n ? (uint32_t)strlen(l->it[wp].text) : 0xffffffffu; /* LB_GETTEXTLEN */
    case 0x18b: return (uint32_t)l->n;                                     /* LB_GETCOUNT */
    case 0x18c: { int i = lb_find(l, (int)wp, gstr(lp), 0); if (i >= 0) lb_select(cpu, w, l, i, 0); return (uint32_t)i; } /* LB_SELECTSTRING */
    case 0x18e: return (uint32_t)l->top;                                   /* LB_GETTOPINDEX */
    case 0x18f: return (uint32_t)lb_find(l, (int)wp, gstr(lp), 0);         /* LB_FINDSTRING */
    case 0x190: return l->sel >= 0;                                        /* LB_GETSELCOUNT */
    case 0x197: l->top = (int)wp; lb_clamp_top(w, l); wm_invalidate(hwnd, NULL, 1); return 0; /* LB_SETTOPINDEX */
    case 0x198: /* LB_GETITEMRECT */
        WR32(lp, 0); WR32(lp + 4, (uint32_t)(((int)wp - l->top) * l->ih));
        WR32(lp + 8, (uint32_t)cr.r); WR32(lp + 12, (uint32_t)(((int)wp - l->top + 1) * l->ih));
        return 1;
    case 0x199: return wp < (uint32_t)l->n ? l->it[wp].data : 0xffffffffu; /* LB_GETITEMDATA */
    case 0x19a: if (wp < (uint32_t)l->n) l->it[wp].data = lp; return 0;   /* LB_SETITEMDATA */
    case 0x19f: return (uint32_t)l->caret;                                 /* LB_GETCARETINDEX */
    case 0x1a0: if (lp) l->ih = (int)lp; return 0;                         /* LB_SETITEMHEIGHT */
    case 0x1a1: return (uint32_t)l->ih;                                    /* LB_GETITEMHEIGHT */
    case 0x1a2: return (uint32_t)lb_find(l, (int)wp, gstr(lp), 1);         /* LB_FINDSTRINGEXACT */
    case 0x18d: { /* LB_DIR(attr, spec): files of the game folder matching spec */
        char rel[1024];
        guest_rel_path(gstr(lp), rel, sizeof rel);
        char *slash = strrchr(rel, '/');
        const char *pat = slash ? slash + 1 : rel;
        if (slash) *slash = 0;
        struct { ListBox *l; const char *pat; int sorted; uint32_t attr; } c = {l, pat, sorted, wp};
        void lb_dir_cb(void *ctx, const char *name, int is_dir);
        vfs_list_dir(slash ? rel : "", lb_dir_cb, &c);
        wm_invalidate(hwnd, NULL, 1);
        return (uint32_t)(l->n - 1);
    }
    }
    return wm_def_proc(cpu, hwnd, msg, wp, lp);
}

static int wild(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') { for (;; s++) { if (wild(p + 1, s)) return 1; if (!*s) return 0; } }
    if (!*s) return 0;
    if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) return wild(p + 1, s + 1);
    return 0;
}

void lb_dir_cb(void *ctx, const char *name, int is_dir) {
    struct { ListBox *l; const char *pat; int sorted; uint32_t attr; } *c = ctx;
    if (is_dir) {
        if (!(c->attr & 0x10)) return;
        char b[300];
        snprintf(b, sizeof b, "[%s]", name);
        lb_insert(c->l, -1, b, c->sorted);
        return;
    }
    if (c->attr & 0x8000) return; /* DDL_EXCLUSIVE with only dirs */
    if (wild(c->pat, name) || !strcmp(c->pat, "*.*")) lb_insert(c->l, -1, name, c->sorted);
}

CTL_PROC(listbox_proc) { return listbox_common(cpu, hwnd, msg, wp, lp, 0); }
CTL_PROC(combolbox_proc) { return listbox_common(cpu, hwnd, msg, wp, lp, 1); }

/* ===================================================================================== COMBOBOX */
typedef struct {
    uint32_t list;      /* the ComboLBox popup (exists always, shown when dropped) */
    int drop_h, dropped, pushed;
} Combo;

static void combo_free(void *p) { free(p); }

static ListBox *combo_lb(Combo *c) {
    Wnd *lw = wnd_get(c->list);
    return lw ? lw->ctl : NULL;
}

static void combo_dropdown(CPU *cpu, Wnd *w) {
    Combo *c = w->ctl;
    if (c->dropped) return;
    Wnd *lw = wnd_get(c->list);
    ListBox *l = combo_lb(c);
    if (!lw || !l) return;
    wm_notify_parent(cpu, w, 7); /* CBN_DROPDOWN */
    int sx, sy;
    wm_client_origin(w, &sx, &sy);
    int ox = sx - ((w->style & WS_BORDER) ? 1 : 0), oy = sy;
    (void)ox;
    Rect cr;
    wm_client_rect(w, &cr);
    int rows = l->n < 8 ? (l->n ? l->n : 1) : 8;
    int h = rows * l->ih + 2;
    Canvas *dk = desktop_canvas();
    int x = sx, y = sy + cr.b + ((w->style & WS_BORDER) ? 1 : 0);
    if (y + h > dk->h) y = sy - h;
    wm_move(cpu, c->list, x - 1, y, cr.r + 2, h, 1);
    lb_ensure_visible(lw, l, l->sel < 0 ? 0 : l->sel);
    wm_show(cpu, c->list, 1);
    c->dropped = 1;
    wm_set_capture(c->list);
}

static void combo_closeup(CPU *cpu, Wnd *w, int accept) {
    if (!w) return;
    Combo *c = w->ctl;
    if (!c || !c->dropped) return;
    c->dropped = 0;
    wm_show(cpu, c->list, 0);
    wm_set_capture(0);
    wm_invalidate(w->hwnd, NULL, 1);
    uint32_t h = w->hwnd;
    wm_notify_parent(cpu, w, 8); /* CBN_CLOSEUP */
    if (accept) {
        wm_notify_parent(cpu, w, 1); /* CBN_SELCHANGE */
        wm_notify_parent(cpu, w, 9); /* CBN_SELENDOK */
    } else {
        wm_notify_parent(cpu, w, 10); /* CBN_SELENDCANCEL */
    }
    (void)h;
}

static void combo_paint(CPU *cpu, Wnd *w) {
    Combo *c = w->ctl;
    ListBox *l = combo_lb(c);
    uint32_t dc = paint_begin(w);
    Rect r;
    wm_client_rect(w, &r);
    uint32_t br = ctl_color(cpu, w, dc, WM_CTLCOLOREDIT);
    gdi_fill_rect_brush(dc, r, br);
    gdi_draw_edge(dc, r, 0, 1);
    Rect btn = {r.r - 18, r.t + 2, r.r - 2, r.b - 2};
    gdi_fill_rect_rgb(dc, btn, sys_color(15));
    gdi_draw_edge(dc, btn, !c->pushed, 1);
    int ax = (btn.l + btn.r) / 2, ay = (btn.t + btn.b) / 2 - 1;
    for (int k = 0; k < 4; k++) gdi_fill_rect_rgb(dc, (Rect){ax - 3 + k, ay + k, ax + 4 - k, ay + k + 1}, 0);
    int focused = wm_focus() == w->hwnd;
    Rect tr = {r.l + 3, r.t + 3, btn.l - 2, r.b - 3};
    if (focused && !c->dropped) {
        gdi_fill_rect_rgb(dc, tr, sys_color(13));
        gdi_set_text_color(dc, sys_color(14));
    }
    gdi_set_bk_mode(dc, 1);
    if (l && l->sel >= 0 && l->sel < l->n) {
        Rect t2 = {tr.l + 1, tr.t, tr.r, tr.b};
        gdi_draw_text(dc, l->it[l->sel].text, -1, &t2, 0x20 | 0x4 | 0x800);
    }
    paint_end(dc);
}

CTL_PROC(combobox_proc) {
    Wnd *w = wnd_get(hwnd);
    if (!w) return 0;
    Combo *c = w->ctl;
    if (msg == WM_CREATE && !c) {
        c = calloc(1, sizeof(Combo));
        w->ctl = c;
        w->ctl_free = combo_free;
        c->drop_h = w->h;
        /* the list is a popup owned by the dialog's top level */
        Wnd *top = w;
        while (top && (top->style & WS_CHILD)) top = wnd_get(top->parent);
        c->list = wm_create(cpu, 0, "ComboLBox", 0, "", WS_POPUP | WS_BORDER | WS_VSCROLL, 0, 0, w->w, 40,
                            top ? top->hwnd : 0, 0, w->hinst, 0);
        Wnd *lw = wnd_get(c->list);
        if (lw) {
            ListBox *l = calloc(1, sizeof(ListBox));
            l->sel = -1;
            l->owner_combo = hwnd;
            uint32_t dc = gdi_dc_new(DC_WINDOW, hwnd, 0);
            gdi_select(dc, ctl_font(w));
            l->ih = gdi_font_height(dc) + 4;
            gdi_dc_free(dc);
            lw->ctl = l;
            lw->ctl_free = lb_free;
            /* combo height: one text line (touch-friendly a bit taller) */
            w->h = l->ih + 8;
        }
        return 0;
    }
    if (!c) return wm_def_proc(cpu, hwnd, msg, wp, lp);
    ListBox *l = combo_lb(c);
    Wnd *lw = wnd_get(c->list);
    switch (msg) {
    case WM_DESTROY: if (c->list) { wm_destroy(cpu, c->list); c->list = 0; } return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_PAINT: combo_paint(cpu, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETFONT:
        w->font = wp;
        if (lw) { lw->font = wp; uint32_t dc = gdi_dc_new(DC_WINDOW, hwnd, 0); gdi_select(dc, wp); if (l) l->ih = gdi_font_height(dc) + 4; gdi_dc_free(dc); }
        if (l) w->h = l->ih + 8;
        return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        if (msg == WM_KILLFOCUS && c->dropped) combo_closeup(cpu, w, 0);
        wm_invalidate(hwnd, NULL, 1);
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        wm_set_focus(cpu, hwnd);
        if (c->dropped) combo_closeup(cpu, w, 0);
        else combo_dropdown(cpu, w);
        return 0;
    case WM_KEYDOWN:
        if (!l) return 0;
        if (c->dropped) return listbox_common(cpu, c->list, msg, wp, lp, 1);
        if (wp == 0x73 || (wp == 0x28 && input_vk_down(0x12))) { combo_dropdown(cpu, w); return 0; } /* F4 / Alt+Down */
        if (wp == 0x26 || wp == 0x28 || wp == 0x24 || wp == 0x23 || wp == 0x21 || wp == 0x22) {
            int i = l->sel < 0 ? 0 : l->sel;
            if (wp == 0x26) i--;
            else if (wp == 0x28) i = l->sel < 0 ? 0 : i + 1;
            else if (wp == 0x24) i = 0;
            else if (wp == 0x23) i = l->n - 1;
            else if (wp == 0x21) i -= 8;
            else i += 8;
            if (i < 0) i = 0;
            if (i >= l->n) i = l->n - 1;
            if (i != l->sel && i >= 0) {
                l->sel = i;
                wm_invalidate(hwnd, NULL, 1);
                wm_notify_parent(cpu, w, 1); /* CBN_SELCHANGE */
            }
        }
        return 0;
    case WM_CHAR:
        if (l && wp > 32) {
            char s[2] = {(char)wp, 0};
            int i = lb_find(l, l->sel, s, 0);
            if (i >= 0) { l->sel = i; wm_invalidate(hwnd, NULL, 1); wm_notify_parent(cpu, w, 1); }
        }
        return 0;
    case WM_GETTEXT: {
        if (!wp) return 0;
        const char *t = l && l->sel >= 0 ? l->it[l->sel].text : "";
        size_t n = strlen(t);
        if (n >= wp) n = wp - 1;
        memcpy(GPTR(lp), t, n);
        WR8(lp + n, 0);
        return (uint32_t)n;
    }
    case WM_GETTEXTLENGTH: return l && l->sel >= 0 ? (uint32_t)strlen(l->it[l->sel].text) : 0;
    case WM_SETTEXT: if (l) { int i = lb_find(l, -1, gstr(lp), 1); l->sel = i; wm_invalidate(hwnd, NULL, 1); } return 1;
    case 0x143: { int i = l ? lb_insert(l, -1, lp ? gstr(lp) : "", (w->style & 0x100) != 0) : -1; return (uint32_t)i; } /* CB_ADDSTRING */
    case 0x144: return lw ? listbox_common(cpu, c->list, 0x182, wp, 0, 1) : 0;                                         /* CB_DELETESTRING */
    case 0x146: return l ? (uint32_t)l->n : 0;                                                                         /* CB_GETCOUNT */
    case 0x147: return l ? (uint32_t)l->sel : 0xffffffffu;                                                             /* CB_GETCURSEL */
    case 0x148: if (!l || wp >= (uint32_t)l->n) return 0xffffffffu; strcpy(gstr(lp), l->it[wp].text); return (uint32_t)strlen(l->it[wp].text); /* CB_GETLBTEXT */
    case 0x149: return l && wp < (uint32_t)l->n ? (uint32_t)strlen(l->it[wp].text) : 0xffffffffu;                      /* CB_GETLBTEXTLEN */
    case 0x14a: return l ? (uint32_t)lb_insert(l, (int)wp, lp ? gstr(lp) : "", 0) : 0xffffffffu;                       /* CB_INSERTSTRING */
    case 0x14b: if (lw) listbox_common(cpu, c->list, 0x184, 0, 0, 1); wm_invalidate(hwnd, NULL, 1); return 0;          /* CB_RESETCONTENT */
    case 0x14c: return l ? (uint32_t)lb_find(l, (int)wp, gstr(lp), 0) : 0xffffffffu;                                   /* CB_FINDSTRING */
    case 0x14d: { int i = l ? lb_find(l, (int)wp, gstr(lp), 0) : -1; if (i >= 0) { l->sel = i; wm_invalidate(hwnd, NULL, 1); } return (uint32_t)i; } /* CB_SELECTSTRING */
    case 0x14e: /* CB_SETCURSEL */
        if (!l) return 0xffffffffu;
        l->sel = (int32_t)wp >= 0 && wp < (uint32_t)l->n ? (int)wp : -1;
        wm_invalidate(hwnd, NULL, 1);
        return l->sel < 0 ? 0xffffffffu : wp;
    case 0x14f: if (wp) combo_dropdown(cpu, w); else combo_closeup(cpu, w, 0); return 1;                              /* CB_SHOWDROPDOWN */
    case 0x150: return l && wp < (uint32_t)l->n ? l->it[wp].data : 0xffffffffu;                                        /* CB_GETITEMDATA */
    case 0x151: if (l && wp < (uint32_t)l->n) l->it[wp].data = lp; return 0;                                          /* CB_SETITEMDATA */
    case 0x153: if (l && lp) l->ih = (int)lp; return 0;                                                                /* CB_SETITEMHEIGHT */
    case 0x154: return l ? (uint32_t)l->ih : 13;                                                                       /* CB_GETITEMHEIGHT */
    case 0x157: return (uint32_t)c->dropped;                                                                           /* CB_GETDROPPEDSTATE */
    case 0x158: return l ? (uint32_t)lb_find(l, (int)wp, gstr(lp), 1) : 0xffffffffu;                                   /* CB_FINDSTRINGEXACT */
    case 0x15e: return 0;                                                                                             /* CB_LIMITTEXT */
    case WM_ENABLE: wm_invalidate(hwnd, NULL, 1); return 0;
    }
    return wm_def_proc(cpu, hwnd, msg, wp, lp);
}

/* ===================================================================================== dialogs */
static uint32_t dlg_def_proc_impl(CPU *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
static void dlg_def_proc(CPU *cpu) {
    uint32_t a0 = ARG(0), a1 = ARG(1), a2 = ARG(2), a3 = ARG(3);
    uint32_t r = dlg_def_proc_impl(cpu, a0, a1, a2, a3);
    RET_STDCALL(4, r);
}

static uint32_t g_dlg_def_thunk;
uint32_t wm_dialog_default_proc_thunk(void) { return g_dlg_def_thunk; }

void ctl_register_classes(void) {
    uint32_t arrow = 0x5002;
    wm_register_class("BUTTON", 8, thunk_register(button_proc, "BUTTON wndproc"), 0, 0, arrow);
    wm_register_class("STATIC", 0, thunk_register(static_proc, "STATIC wndproc"), 0, 0, arrow);
    wm_register_class("EDIT", 8, thunk_register(edit_proc, "EDIT wndproc"), 0, 0, arrow);
    wm_register_class("LISTBOX", 8, thunk_register(listbox_proc, "LISTBOX wndproc"), 0, 0, arrow);
    wm_register_class("COMBOBOX", 8, thunk_register(combobox_proc, "COMBOBOX wndproc"), 0, 0, arrow);
    wm_register_class("ComboLBox", 8, thunk_register(combolbox_proc, "ComboLBox wndproc"), 0, 0, arrow);
    g_dlg_def_thunk = thunk_register(dlg_def_proc, "DefDlgProc");
    wm_register_class("#32770", 8, g_dlg_def_thunk, 30, 0, arrow);
}

static int is_dlg_child(uint32_t hdlg, uint32_t h) { return h && (h == hdlg || wm_is_child_of(h, hdlg)); }

static uint32_t dlg_find_button(uint32_t hdlg, uint32_t id) {
    for (uint32_t c = wm_first_child(hdlg); c; c = wm_next_sibling(c))
        if (wnd_get(c)->id == id) return c;
    return 0;
}

static int tabbable(Wnd *w) {
    return w && (w->style & WS_VISIBLE) && !(w->style & WS_DISABLED) && (w->style & WS_TABSTOP);
}

/* next/previous WS_TABSTOP control (tab order = z order of the children) */
uint32_t dlg_next_tab(uint32_t hdlg, uint32_t from, int prev) {
    uint32_t list[256];
    int n = 0;
    for (uint32_t c = wm_first_child(hdlg); c && n < 256; c = wm_next_sibling(c)) list[n++] = c;
    if (!n) return 0;
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (list[i] == from || wm_is_child_of(from, list[i])) cur = i;
    for (int k = 1; k <= n; k++) {
        int i = cur < 0 ? (prev ? n - k : k - 1) : ((cur + (prev ? -k : k)) % n + n) % n;
        if (tabbable(wnd_get(list[i]))) return list[i];
    }
    return 0;
}

static void dlg_set_focus_ctl(CPU *cpu, uint32_t hdlg, uint32_t ctl) {
    Wnd *d = wnd_get(hdlg);
    if (!ctl) return;
    if (d) d->dlg_focus = ctl;
    Wnd *c = wnd_get(ctl);
    wm_set_focus(cpu, ctl);
    if (c && !strcasecmp(c->cls->name, "EDIT")) wm_send(cpu, ctl, 0xb1, 0, 0xffffffffu); /* select all, as Windows does on tab */
}

static uint32_t def_button_id(CPU *cpu, uint32_t hdlg) {
    uint32_t r = wm_send(cpu, hdlg, 0x400, 0, 0); /* DM_GETDEFID */
    if ((r >> 16) == 0x534b) return r & 0xffff;
    /* a focused push button wins; else the BS_DEFPUSHBUTTON; else IDOK */
    Wnd *f = wnd_get(wm_focus());
    if (f && is_dlg_child(hdlg, f->hwnd) && !strcasecmp(f->cls->name, "BUTTON") && (f->style & 0xf) <= 1) return f->id;
    for (uint32_t c = wm_first_child(hdlg); c; c = wm_next_sibling(c)) {
        Wnd *w = wnd_get(c);
        if (!strcasecmp(w->cls->name, "BUTTON") && (w->style & 0xf) == 1) return w->id;
    }
    return IDOK;
}

static uint32_t dlg_def_proc_impl(CPU *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Wnd *w = wnd_get(hwnd);
    if (!w) return 0;
    uint32_t proc = rd_dlgproc(w);
    if (proc) {
        uint32_t args[4] = {hwnd, msg, wp, lp};
        uint32_t r = guest_call(cpu, proc, 4, args);
        w = wnd_get(hwnd);
        if (!w) return r;
        if (r) {
            switch (msg) {
            case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
            case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC:
            case WM_INITDIALOG: case 0x0039: /* WM_COMPAREITEM */ case 0x002e: case 0x002f: case 0x0037:
                return r;
            }
            return rd_msgresult(w);
        }
    }
    switch (msg) {
    case WM_ERASEBKGND: {
        uint32_t br = wm_send(cpu, hwnd, WM_CTLCOLORDLG, wp, hwnd);
        if (!br) br = sys_color_brush(15);
        Rect r;
        wm_client_rect(w, &r);
        gdi_fill_rect_brush(wp, r, br);
        return 1;
    }
    case WM_CLOSE: {
        uint32_t c = dlg_find_button(hwnd, IDCANCEL);
        wm_post(hwnd, WM_COMMAND, IDCANCEL, c);
        return 0;
    }
    case WM_ACTIVATE:
        if ((wp & 0xffff) && w->dlg_focus && wnd_get(w->dlg_focus)) wm_set_focus(cpu, w->dlg_focus);
        return 0;
    case WM_SETFOCUS: {
        uint32_t c = w->dlg_focus && wnd_get(w->dlg_focus) ? w->dlg_focus : dlg_next_tab(hwnd, 0, 0);
        if (c) dlg_set_focus_ctl(cpu, hwnd, c);
        return 0;
    }
    case 0x0028: /* WM_NEXTDLGCTL */
        if (lp & 0xffff) dlg_set_focus_ctl(cpu, hwnd, wp);
        else dlg_set_focus_ctl(cpu, hwnd, dlg_next_tab(hwnd, wm_focus(), wp != 0));
        return 0;
    case 0x400: return 0; /* DM_GETDEFID: none set */
    case 0x401: return 1; /* DM_SETDEFID */
    case WM_GETDLGCODE: return 0;
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC:
        return wm_def_proc(cpu, hwnd, msg, wp, lp);
    }
    return wm_def_proc(cpu, hwnd, msg, wp, lp);
}

void imp_USER32_DefDlgProcA(CPU *cpu) {
    uint32_t a0 = ARG(0), a1 = ARG(1), a2 = ARG(2), a3 = ARG(3);
    uint32_t r = dlg_def_proc_impl(cpu, a0, a1, a2, a3);
    RET_STDCALL(4, r);
}

int dlg_is_dialog_message(CPU *cpu, uint32_t hdlg, const WinMsg *m) {
    if (!is_dlg_child(hdlg, m->hwnd)) return 0;
    if (m->message == WM_KEYDOWN) {
        uint32_t code = wm_send(cpu, m->hwnd, WM_GETDLGCODE, m->wparam, 0);
        int vk = (int)m->wparam;
        if (code & DLGC_WANTALLKEYS) goto dispatch;
        switch (vk) {
        case 0x09:
            if (code & DLGC_WANTTAB) break;
            dlg_set_focus_ctl(cpu, hdlg, dlg_next_tab(hdlg, wm_focus(), input_vk_down(0x10)));
            return 1;
        case 0x25: case 0x26: case 0x27: case 0x28:
            if (code & DLGC_WANTARROWS) break;
            dlg_set_focus_ctl(cpu, hdlg, dlg_next_tab(hdlg, wm_focus(), vk == 0x25 || vk == 0x26));
            return 1;
        case 0x0d: {
            uint32_t id = def_button_id(cpu, hdlg);
            uint32_t b = dlg_find_button(hdlg, id);
            Wnd *bw = wnd_get(b);
            if (bw && (bw->style & WS_DISABLED)) return 1;
            wm_send(cpu, hdlg, WM_COMMAND, id & 0xffff, b);
            return 1;
        }
        case 0x1b:
            wm_send(cpu, hdlg, WM_COMMAND, IDCANCEL, dlg_find_button(hdlg, IDCANCEL));
            return 1;
        }
    }
    if (m->message == WM_CHAR && (m->wparam == 0x0d || m->wparam == 0x1b || m->wparam == 0x09)) {
        uint32_t code = wm_send(cpu, m->hwnd, WM_GETDLGCODE, m->wparam, 0);
        if (!(code & DLGC_WANTALLKEYS)) return 1; /* already handled as a key */
    }
dispatch:
    wm_translate(m);
    wm_dispatch(cpu, m);
    return 1;
}

void imp_USER32_IsDialogMessageA(CPU *cpu) {
    uint32_t p = ARG(1);
    WinMsg m = {RD32(p), RD32(p + 4), RD32(p + 8), RD32(p + 12), RD32(p + 16), RD32(p + 20), RD32(p + 24)};
    RET_STDCALL(2, (uint32_t)dlg_is_dialog_message(cpu, ARG(0), &m));
}

/* ---- templates ------------------------------------------------------------------------------- */
static uint32_t rd_wstr(uint32_t p, char *out, int n) {
    int o = 0;
    uint16_t c;
    while ((c = RD16(p))) {
        if (o < n - 1) out[o++] = c < 256 ? (char)c : '?';
        p += 2;
    }
    out[o] = 0;
    return p + 2;
}

/* sz_Or_Ord: returns next pointer; *ord set when it is an ordinal */
static uint32_t rd_sz_or_ord(uint32_t p, char *out, int n, uint32_t *ord) {
    *ord = 0;
    uint16_t c = RD16(p);
    if (c == 0) { out[0] = 0; return p + 2; }
    if (c == 0xffff) { *ord = RD16(p + 2); snprintf(out, (size_t)n, "#%u", *ord); return p + 4; }
    return rd_wstr(p, out, n);
}

static const char *atom_class(uint32_t a) {
    switch (a) {
    case 0x80: return "BUTTON";
    case 0x81: return "EDIT";
    case 0x82: return "STATIC";
    case 0x83: return "LISTBOX";
    case 0x84: return "SCROLLBAR";
    case 0x85: return "COMBOBOX";
    }
    return NULL;
}

/* dialog base units for a font (Windows: average of A-Z a-z, and the cell height) */
static void base_units(uint32_t font, int *bx, int *by) {
    static const char az[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    uint32_t dc = gdi_dc_new(DC_SCREEN, 0, 0);
    gdi_select(dc, font);
    int w = gdi_text_width(dc, az, 52);
    *bx = (w / 26 + 1) / 2;
    *by = gdi_font_height(dc);
    gdi_dc_free(dc);
    if (*bx < 4) *bx = 4;
    if (*by < 8) *by = 8;
}

uint32_t dlg_create_indirect(CPU *cpu, uint32_t hinst, uint32_t t, uint32_t owner, uint32_t dlgproc, uint32_t param) {
    int ext = RD16(t) == 1 && RD16(t + 2) == 0xffff;
    uint32_t style, exstyle, n;
    int x, y, cx, cy;
    uint32_t p;
    if (ext) {
        exstyle = RD32(t + 8); style = RD32(t + 12); n = RD16(t + 16);
        x = (int16_t)RD16(t + 18); y = (int16_t)RD16(t + 20); cx = (int16_t)RD16(t + 22); cy = (int16_t)RD16(t + 24);
        p = t + 26;
    } else {
        style = RD32(t); exstyle = RD32(t + 4); n = RD16(t + 8);
        x = (int16_t)RD16(t + 10); y = (int16_t)RD16(t + 12); cx = (int16_t)RD16(t + 14); cy = (int16_t)RD16(t + 16);
        p = t + 18;
    }
    char menu[64], cls[64], title[256], face[64];
    uint32_t ord;
    p = rd_sz_or_ord(p, menu, sizeof menu, &ord);
    p = rd_sz_or_ord(p, cls, sizeof cls, &ord);
    p = rd_wstr(p, title, sizeof title);
    int pt = 8, weight = 400;
    if (style & 0x48) { /* DS_SETFONT | DS_SHELLFONT */
        pt = RD16(p);
        p += 2;
        if (ext) { weight = RD16(p); p += 4; }
        p = rd_wstr(p, face, sizeof face);
    }
    (void)weight;
    uint32_t font = gdi_dialog_font(pt);
    int bx, by;
    base_units(font, &bx, &by);
    int pw = cx * bx / 4, ph = cy * by / 8;
    /* window size = client + frame */
    Wnd tmp = {0};
    tmp.style = style;
    tmp.exstyle = exstyle;
    tmp.is_dialog = 1;
    extern void wm_nc_size(Wnd * w, int *dx, int *dy);
    int ncx, ncy;
    wm_nc_size(&tmp, &ncx, &ncy);
    int ww = pw + ncx, wh = ph + ncy;
    int px = x * bx / 4, py = y * by / 8;
    Canvas *dk = desktop_canvas();
    if (style & DS_CENTER) {
        px = (dk->w - ww) / 2;
        py = (dk->h - wh) / 2;
    } else if (!(style & 1) && owner && !(style & WS_CHILD)) { /* not DS_ABSALIGN: relative to the owner's client */
        Wnd *o = wnd_get(owner);
        if (o) { int ox, oy; wm_client_origin(o, &ox, &oy); px += ox; py += oy; }
    }
    if (!(style & WS_CHILD)) {
        if (px + ww > dk->w) px = dk->w - ww;
        if (py + wh > dk->h) py = dk->h - wh;
        if (px < 0) px = 0;
        if (py < 0) py = 0;
    }
    const char *clsname = cls[0] ? cls : "#32770";
    uint32_t clsatom = 0;
    if (cls[0] == '#' && ord) { clsname = NULL; clsatom = ord; }
    uint32_t hdlg = wm_create(cpu, exstyle, clsname, clsatom, title, style & ~WS_VISIBLE, px, py, ww, wh, owner, 0, hinst, param);
    Wnd *d = wnd_get(hdlg);
    if (!d) return 0;
    d->is_dialog = 1;
    d->font = font;
    memcpy(d->extra + DWLP_DLGPROC, &dlgproc, 4);
    /* controls */
    for (uint32_t i = 0; i < n; i++) {
        p = (p + 3) & ~3u;
        uint32_t cst, cex, id;
        int ix, iy, icx, icy;
        if (ext) {
            cex = RD32(p + 4); cst = RD32(p + 8);
            ix = (int16_t)RD16(p + 12); iy = (int16_t)RD16(p + 14); icx = (int16_t)RD16(p + 16); icy = (int16_t)RD16(p + 18);
            id = RD32(p + 20);
            p += 24;
        } else {
            cst = RD32(p); cex = RD32(p + 4);
            ix = (int16_t)RD16(p + 8); iy = (int16_t)RD16(p + 10); icx = (int16_t)RD16(p + 12); icy = (int16_t)RD16(p + 14);
            id = RD16(p + 16);
            p += 18;
        }
        char ccls[64], ctext[512];
        uint32_t cord, tord;
        p = rd_sz_or_ord(p, ccls, sizeof ccls, &cord);
        p = rd_sz_or_ord(p, ctext, sizeof ctext, &tord);
        uint32_t xl = RD16(p);
        uint32_t cdata = xl ? p + 2 : 0;
        p += 2 + xl;
        const char *cn = cord ? atom_class(cord) : ccls;
        if (!cn) { rt_log("dialog: unknown control class atom %x", cord); continue; }
        uint32_t h = wm_create(cpu, cex, cn, 0, ctext, cst | WS_CHILD, ix * bx / 4, iy * by / 8, icx * bx / 4, icy * by / 8,
                               hdlg, id, hinst, cdata);
        if (h) wm_send(cpu, h, WM_SETFONT, font, 0);
    }
    uint32_t first = dlg_next_tab(hdlg, 0, 0);
    if (wm_send(cpu, hdlg, WM_INITDIALOG, first, param) && wnd_get(hdlg)) {
        uint32_t f = dlg_next_tab(hdlg, 0, 0);
        if (f) { d = wnd_get(hdlg); d->dlg_focus = f; }
    }
    d = wnd_get(hdlg);
    if (!d) return 0;
    if (style & WS_VISIBLE) {
        wm_show(cpu, hdlg, 1);
        extern int wm_set_active(CPU *cpu, uint32_t hwnd);
        wm_set_active(cpu, hdlg);
        if (d->dlg_focus) dlg_set_focus_ctl(cpu, hdlg, d->dlg_focus);
    }
    TRACE("dialog \"%s\" %dx%d -> %x", title, ww, wh, hdlg);
    return hdlg;
}

void imp_USER32_CreateDialogIndirectParamA(CPU *cpu) {
    RET_STDCALL(5, dlg_create_indirect(cpu, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)));
}

void imp_USER32_CreateDialogParamA(CPU *cpu) {
    uint32_t e = res_find(5, ARG(1));
    RET_STDCALL(5, e ? dlg_create_indirect(cpu, ARG(0), res_data(e, NULL), ARG(2), ARG(3), ARG(4)) : 0);
}

static uint32_t run_modal(CPU *cpu, uint32_t hdlg, uint32_t owner) {
    Wnd *d = wnd_get(hdlg);
    if (!d) return 0xffffffffu;
    Wnd *o = wnd_get(owner);
    int owner_was_enabled = o && !(o->style & WS_DISABLED);
    if (owner_was_enabled) wm_enable(cpu, owner, 0);
    if (!(d->style & WS_VISIBLE)) {
        wm_show(cpu, hdlg, 1);
        extern int wm_set_active(CPU *cpu, uint32_t hwnd);
        wm_set_active(cpu, hdlg);
        if (d->dlg_focus) dlg_set_focus_ctl(cpu, hdlg, d->dlg_focus);
    }
    WinMsg m;
    for (;;) {
        d = wnd_get(hdlg);
        if (!d || d->dlg_ended) break;
        wm_get_message(cpu, &m, 0, 0, 0, 1, 1);
        if (m.message == WM_QUIT) { wm_post(0, WM_QUIT, m.wparam, 0); break; }
        if (!dlg_is_dialog_message(cpu, hdlg, &m)) { wm_translate(&m); wm_dispatch(cpu, &m); }
    }
    d = wnd_get(hdlg);
    uint32_t r = d ? d->dlg_result : 0xffffffffu;
    if (owner_was_enabled) wm_enable(cpu, owner, 1);
    if (d) wm_destroy(cpu, hdlg);
    extern int wm_set_active(CPU *cpu, uint32_t hwnd);
    if (o && wnd_get(owner)) wm_set_active(cpu, owner);
    return r;
}

void imp_USER32_DialogBoxParamA(CPU *cpu) {
    uint32_t hinst = ARG(0), name = ARG(1), owner = ARG(2), proc = ARG(3), param = ARG(4);
    uint32_t e = res_find(5, name);
    if (!e) RET_STDCALL(5, 0xffffffffu);
    uint32_t h = dlg_create_indirect(cpu, hinst, res_data(e, NULL), owner, proc, param);
    RET_STDCALL(5, run_modal(cpu, h, owner));
}

void imp_USER32_DialogBoxIndirectParamA(CPU *cpu) {
    uint32_t owner = ARG(2);
    uint32_t h = dlg_create_indirect(cpu, ARG(0), ARG(1), owner, ARG(3), ARG(4));
    RET_STDCALL(5, run_modal(cpu, h, owner));
}

void imp_USER32_EndDialog(CPU *cpu) {
    Wnd *d = wnd_get(ARG(0));
    if (d) {
        d->dlg_ended = 1;
        d->dlg_result = ARG(1);
        /* modal dialogs created by DialogBoxParam are destroyed by its loop; MFC
         * destroys its own after its loop sees the dialog hidden */
        wm_show(cpu, ARG(0), 0);
        uint32_t owner = d->owner;
        Wnd *o = wnd_get(owner);
        if (o && (o->style & WS_DISABLED)) wm_enable(cpu, owner, 1);
    }
    RET_STDCALL(2, 1);
}

/* ---- dialog item helpers ---------------------------------------------------------------------- */
void imp_USER32_SetDlgItemTextA(CPU *cpu) {
    uint32_t c = dlg_find_button(ARG(0), ARG(1));
    if (c) wm_send(cpu, c, WM_SETTEXT, 0, ARG(2));
    RET_STDCALL(3, c != 0);
}

void imp_USER32_GetDlgItemTextA(CPU *cpu) {
    uint32_t c = dlg_find_button(ARG(0), ARG(1));
    uint32_t r = 0;
    if (c) r = wm_send(cpu, c, WM_GETTEXT, ARG(3), ARG(2));
    else if (ARG(3)) WR8(ARG(2), 0);
    RET_STDCALL(4, r);
}

void imp_USER32_SetDlgItemInt(CPU *cpu) {
    uint32_t c = dlg_find_button(ARG(0), ARG(1));
    char b[32];
    if (ARG(3)) snprintf(b, sizeof b, "%d", (int)ARG(2));
    else snprintf(b, sizeof b, "%u", ARG(2));
    if (c) {
        uint32_t g = guest_strdup(b);
        wm_send(cpu, c, WM_SETTEXT, 0, g);
        heap_free(g);
    }
    RET_STDCALL(4, c != 0);
}

void imp_USER32_GetDlgItemInt(CPU *cpu) {
    uint32_t c = dlg_find_button(ARG(0), ARG(1));
    const char *t = c ? wm_text(c) : "";
    Wnd *w = wnd_get(c);
    if (w && w->ctl && !strcasecmp(w->cls->name, "EDIT")) t = ((Edit *)w->ctl)->buf;
    char *end;
    long v = strtol(t, &end, 10);
    int ok = c && end != t;
    if (ARG(2)) WR32(ARG(2), (uint32_t)ok);
    RET_STDCALL(4, ok ? (uint32_t)v : 0);
}

void imp_USER32_CheckDlgButton(CPU *cpu) {
    uint32_t c = dlg_find_button(ARG(0), ARG(1));
    if (c) wm_send(cpu, c, 0xf1, ARG(2), 0);
    RET_STDCALL(3, 1);
}

void imp_USER32_IsDlgButtonChecked(CPU *cpu) {
    uint32_t c = dlg_find_button(ARG(0), ARG(1));
    RET_STDCALL(2, c ? wm_send(cpu, c, 0xf0, 0, 0) : 0);
}

void imp_USER32_GetNextDlgTabItem(CPU *cpu) { RET_STDCALL(3, dlg_next_tab(ARG(0), ARG(1), ARG(2) != 0)); }

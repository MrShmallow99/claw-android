/* GDI32 emulation: device contexts over memory bitmaps, DirectDraw surfaces and
 * windows; pens, brushes, regions, bitmaps, fonts (Liberation Sans through
 * stb_truetype), text, lines, rectangles and blits with the classic raster ops. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <pthread.h>
#include "gdi.h"

/* ---- canvas pixels --------------------------------------------------------------- */
static inline uint8_t *px_at(const Canvas *c, int x, int y) {
    return c->bits + (intptr_t)y * c->pitch + (intptr_t)x * (c->bpp / 8);
}

/* nearest palette entry, cached (direct mapped; entries validated against the palette) */
typedef struct { const uint32_t *pal; uint32_t rgb, val; uint8_t idx; } NearEnt;
static NearEnt g_near[4096];
static uint8_t nearest_index(const Canvas *c, uint32_t rgb) {
    uint32_t h = ((rgb * 2654435761u) ^ (uint32_t)(uintptr_t)c->pal) >> 20;
    NearEnt *e = &g_near[h & 4095];
    if (e->pal == c->pal && e->rgb == rgb && c->pal[e->idx] == e->val) return e->idx;
    int best = 0, bd = 1 << 30;
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    for (int i = 0; i < c->pal_n; i++) {
        uint32_t p = c->pal[i];
        int dr = (int)((p >> 16) & 255) - r, dg = (int)((p >> 8) & 255) - g, db = (int)(p & 255) - b;
        int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) { bd = d; best = i; if (!d) break; }
    }
    e->pal = c->pal; e->rgb = rgb; e->idx = (uint8_t)best; e->val = c->pal[best];
    return (uint8_t)best;
}

uint32_t canvas_pixel_for(const Canvas *c, uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    switch (c->bpp) {
    case 1: return (r + g + b) >= 384 ? 1 : 0;
    case 8: return c->pal ? nearest_index(c, rgb) : (r + g + b) / 3;
    case 16: return c->rgb565 ? ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) : ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    default: return rgb;
    }
}

static uint32_t rgb_of_pixel(const Canvas *c, uint32_t v) {
    switch (c->bpp) {
    case 1: return v ? 0xffffff : 0;
    case 8: return c->pal ? c->pal[v & 255] : v * 0x010101u;
    case 16:
        if (c->rgb565) {
            uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            return (r << 19 | (r >> 2) << 16) | (g << 10 | (g >> 4) << 8) | (b << 3 | b >> 2);
        } else {
            uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
            return (r << 19 | (r >> 2) << 16) | (g << 11 | (g >> 2) << 8) | (b << 3 | b >> 2);
        }
    default: return v & 0xffffff;
    }
}

static inline uint32_t raw_get(const Canvas *c, int x, int y) {
    if (c->bpp == 1) {
        uint8_t *p = c->bits + (intptr_t)y * c->pitch + (x >> 3);
        return (*p >> (7 - (x & 7))) & 1;
    }
    uint8_t *p = px_at(c, x, y);
    switch (c->bpp) {
    case 8: return *p;
    case 16: return *(uint16_t *)p;
    case 24: return p[0] | p[1] << 8 | (uint32_t)p[2] << 16;
    default: return *(uint32_t *)p & 0xffffff;
    }
}

static inline void raw_put(const Canvas *c, int x, int y, uint32_t v) {
    if (c->bpp == 1) {
        uint8_t *p = c->bits + (intptr_t)y * c->pitch + (x >> 3);
        uint8_t m = (uint8_t)(0x80 >> (x & 7));
        *p = v ? (*p | m) : (*p & ~m);
        return;
    }
    uint8_t *p = px_at(c, x, y);
    switch (c->bpp) {
    case 8: *p = (uint8_t)v; break;
    case 16: *(uint16_t *)p = (uint16_t)v; break;
    case 24: p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); break;
    default: *(uint32_t *)p = v | 0xff000000u; break;
    }
}

uint32_t canvas_get(const Canvas *c, int x, int y) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return 0;
    return rgb_of_pixel(c, raw_get(c, x, y));
}

void canvas_put(const Canvas *c, int x, int y, uint32_t rgb) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    raw_put(c, x, y, canvas_pixel_for(c, rgb));
}

static Rect rect_isect(Rect a, Rect b) {
    Rect r = {a.l > b.l ? a.l : b.l, a.t > b.t ? a.t : b.t, a.r < b.r ? a.r : b.r, a.b < b.b ? a.b : b.b};
    if (r.r < r.l) r.r = r.l;
    if (r.b < r.t) r.b = r.t;
    return r;
}
static int rect_empty(Rect r) { return r.r <= r.l || r.b <= r.t; }

static void fill_raw(const Canvas *c, Rect r, uint32_t v) {
    r = rect_isect(r, (Rect){0, 0, c->w, c->h});
    for (int y = r.t; y < r.b; y++) {
        if (c->bpp == 8) { memset(px_at(c, r.l, y), (int)v, (size_t)(r.r - r.l)); continue; }
        if (c->bpp == 32) { uint32_t *p = (uint32_t *)px_at(c, r.l, y); for (int x = r.l; x < r.r; x++) *p++ = v | 0xff000000u; continue; }
        if (c->bpp == 16) { uint16_t *p = (uint16_t *)px_at(c, r.l, y); for (int x = r.l; x < r.r; x++) *p++ = (uint16_t)v; continue; }
        for (int x = r.l; x < r.r; x++) raw_put(c, x, y, v);
    }
}

void canvas_fill(const Canvas *c, Rect r, uint32_t rgb) { fill_raw(c, r, canvas_pixel_for(c, rgb)); }

/* ---- objects ------------------------------------------------------------------------- */
enum { OBJ_PEN = 1, OBJ_BRUSH, OBJ_FONT, OBJ_BITMAP, OBJ_REGION, OBJ_PAL };
#define MAX_OBJ 4096
#define OBJ_BASE 0x03000000u

typedef struct {
    int type;
    int stock;
    union {
        struct { int style, width; uint32_t color; } pen;
        struct { int style, hatch; uint32_t color; uint32_t pattern; } brush;
        struct { int32_t height, width, weight; uint8_t italic, underline, strike, charset; char face[32]; } font;
        struct {
            int w, h, bpp, pitch;   /* pitch < 0 for bottom-up */
            uint32_t bits;          /* guest address of row 0 (top row) */
            uint32_t alloc;         /* guest allocation to free (0 if not owned) */
            uint32_t pal[256];
            int pal_n;
            int dib;
        } bmp;
        struct { Rect *r; int n; } rgn;
        struct { uint32_t e[256]; int n; } pal;
    } u;
} GdiObj;

static GdiObj *g_obj[MAX_OBJ];
static pthread_mutex_t g_gdi_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t obj_new(int type) {
    pthread_mutex_lock(&g_gdi_lock);
    for (int i = 1; i < MAX_OBJ; i++)
        if (!g_obj[i]) {
            g_obj[i] = calloc(1, sizeof(GdiObj));
            g_obj[i]->type = type;
            pthread_mutex_unlock(&g_gdi_lock);
            return OBJ_BASE + (uint32_t)i * 4;
        }
    pthread_mutex_unlock(&g_gdi_lock);
    rt_log("gdi: out of objects");
    return 0;
}

static GdiObj *obj_get(uint32_t h, int type) {
    if (h < OBJ_BASE || h >= OBJ_BASE + MAX_OBJ * 4 || (h & 3)) return NULL;
    GdiObj *o = g_obj[(h - OBJ_BASE) / 4];
    if (!o || (type && o->type != type)) return NULL;
    return o;
}

static void obj_free(uint32_t h) {
    GdiObj *o = obj_get(h, 0);
    if (!o || o->stock) return;
    if (o->type == OBJ_BITMAP && o->u.bmp.alloc) heap_free(o->u.bmp.alloc);
    if (o->type == OBJ_REGION) free(o->u.rgn.r);
    free(o);
    g_obj[(h - OBJ_BASE) / 4] = NULL;
}

static uint32_t g_stock[20];

static uint32_t make_pen(int style, int width, uint32_t color) {
    uint32_t h = obj_new(OBJ_PEN);
    GdiObj *o = obj_get(h, OBJ_PEN);
    o->u.pen.style = style; o->u.pen.width = width; o->u.pen.color = color;
    return h;
}
static uint32_t make_brush(int style, uint32_t color, int hatch) {
    uint32_t h = obj_new(OBJ_BRUSH);
    GdiObj *o = obj_get(h, OBJ_BRUSH);
    o->u.brush.style = style; o->u.brush.color = color; o->u.brush.hatch = hatch;
    return h;
}
static uint32_t make_font(int32_t height, int weight, const char *face) {
    uint32_t h = obj_new(OBJ_FONT);
    GdiObj *o = obj_get(h, OBJ_FONT);
    o->u.font.height = height; o->u.font.weight = weight;
    snprintf(o->u.font.face, sizeof o->u.font.face, "%s", face);
    return h;
}

static void stock_init(void) {
    if (g_stock[0]) return;
    g_stock[0] = make_brush(0, 0xffffff, 0);  /* WHITE_BRUSH */
    g_stock[1] = make_brush(0, 0xc0c0c0, 0);  /* LTGRAY_BRUSH */
    g_stock[2] = make_brush(0, 0x808080, 0);  /* GRAY_BRUSH */
    g_stock[3] = make_brush(0, 0x404040, 0);  /* DKGRAY_BRUSH */
    g_stock[4] = make_brush(0, 0x000000, 0);  /* BLACK_BRUSH */
    g_stock[5] = make_brush(1, 0, 0);         /* NULL_BRUSH */
    g_stock[6] = make_pen(0, 1, 0xffffff);    /* WHITE_PEN */
    g_stock[7] = make_pen(0, 1, 0x000000);    /* BLACK_PEN */
    g_stock[8] = make_pen(5, 1, 0);           /* NULL_PEN */
    g_stock[10] = make_font(12, 400, "Courier");     /* OEM_FIXED_FONT */
    g_stock[11] = make_font(12, 400, "Courier");     /* ANSI_FIXED_FONT */
    g_stock[12] = make_font(-11, 400, "MS Sans Serif"); /* ANSI_VAR_FONT */
    g_stock[13] = make_font(16, 700, "System");      /* SYSTEM_FONT */
    g_stock[14] = make_font(16, 700, "System");      /* DEVICE_DEFAULT_FONT */
    g_stock[15] = obj_new(OBJ_PAL);                  /* DEFAULT_PALETTE */
    g_stock[16] = make_font(16, 400, "Courier");     /* SYSTEM_FIXED_FONT */
    g_stock[17] = make_font(-11, 400, "MS Sans Serif"); /* DEFAULT_GUI_FONT */
    g_stock[19] = obj_new(OBJ_BITMAP);               /* default 1x1 mono bitmap of memory DCs */
    GdiObj *b = obj_get(g_stock[19], OBJ_BITMAP);
    b->u.bmp.w = b->u.bmp.h = 1; b->u.bmp.bpp = 1; b->u.bmp.pitch = 2;
    b->u.bmp.bits = b->u.bmp.alloc = heap_alloc(4, 1);
    for (int i = 0; i < 20; i++) {
        GdiObj *o = obj_get(g_stock[i], 0);
        if (o) o->stock = 1;
    }
}

uint32_t gdi_stock(int n) { stock_init(); return n >= 0 && n < 20 ? g_stock[n] : 0; }

uint32_t gdi_create_solid_brush(uint32_t colorref) { stock_init(); return make_brush(0, RGB_OF(colorref), 0); }

uint32_t gdi_brush_color(uint32_t hbrush, int *is_null) {
    GdiObj *o = obj_get(hbrush, OBJ_BRUSH);
    if (is_null) *is_null = !o || o->u.brush.style == 1;
    return o ? CR_OF(o->u.brush.color) : 0;
}

/* system colors: a classic Windows 98 scheme */
static const uint32_t g_syscolors[31] = {
    0xc0c0c0, 0x008080, 0x000080, 0x808080, 0xc0c0c0, 0xffffff, 0x000000, 0x000000,
    0x000000, 0xffffff, 0xc0c0c0, 0xc0c0c0, 0x808080, 0x000080, 0xffffff, 0xc0c0c0,
    0x808080, 0x808080, 0x000000, 0xc0c0c0, 0xffffff, 0x404040, 0xdfdfdf, 0x000000,
    0xffffe1, 0xb5b5b5, 0x0000ff, 0x1084d0, 0xb5b5b5, 0x000080, 0x808080};
static uint32_t g_sysbrush[31];
uint32_t sys_color(int idx) { return idx >= 0 && idx < 31 ? CR_OF(g_syscolors[idx]) : 0; }
uint32_t sys_color_brush(int idx) {
    stock_init();
    if (idx < 0 || idx >= 31) idx = 15;
    if (!g_sysbrush[idx]) {
        g_sysbrush[idx] = make_brush(0, g_syscolors[idx], 0);
        obj_get(g_sysbrush[idx], 0)->stock = 1;
    }
    return g_sysbrush[idx];
}

/* ---- fonts (stb_truetype) ------------------------------------------------------------------ */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb/stb_truetype.h"

extern const unsigned char g_font_sans[], g_font_sans_bold[];
extern const unsigned int g_font_sans_size, g_font_sans_bold_size;
static stbtt_fontinfo g_ttf[2];
static int g_ttf_ok;

typedef struct {
    int bold, px;          /* key: face weight and pixel height (cell) */
    float scale;
    int ascent, descent, linegap, avgw, maxw;
    struct Glyph { uint8_t *a; int w, h, xo, yo, adv, ok; } g[256];
} RFont;
#define MAX_RFONT 64
static RFont *g_rfonts[MAX_RFONT];
static int g_nrfont;

static RFont *rfont_get(GdiObj *f) {
    if (!g_ttf_ok) {
        stbtt_InitFont(&g_ttf[0], g_font_sans, stbtt_GetFontOffsetForIndex(g_font_sans, 0));
        stbtt_InitFont(&g_ttf[1], g_font_sans_bold, stbtt_GetFontOffsetForIndex(g_font_sans_bold, 0));
        g_ttf_ok = 1;
    }
    int32_t h = f ? f->u.font.height : -11;
    int bold = f && f->u.font.weight >= 600;
    stbtt_fontinfo *ti = &g_ttf[bold];
    int asc, desc, gap;
    stbtt_GetFontVMetrics(ti, &asc, &desc, &gap);
    float scale;
    if (h < 0) scale = stbtt_ScaleForMappingEmToPixels(ti, (float)-h);
    else if (h > 0) scale = stbtt_ScaleForPixelHeight(ti, (float)h);
    else scale = stbtt_ScaleForMappingEmToPixels(ti, 11.0f);
    int key = (int)(scale * 100000.0f);
    for (int i = 0; i < g_nrfont; i++)
        if (g_rfonts[i]->bold == bold && g_rfonts[i]->px == key) return g_rfonts[i];
    RFont *r = calloc(1, sizeof *r);
    r->bold = bold;
    r->px = key;
    r->scale = scale;
    r->ascent = (int)(asc * scale + 0.99f);
    r->descent = (int)(-desc * scale + 0.99f);
    r->linegap = (int)(gap * scale + 0.5f);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(ti, 'x', &adv, &lsb);
    r->avgw = (int)(adv * scale + 0.5f);
    stbtt_GetCodepointHMetrics(ti, 'W', &adv, &lsb);
    r->maxw = (int)(adv * scale + 0.5f);
    if (g_nrfont < MAX_RFONT) g_rfonts[g_nrfont++] = r;
    return r;
}

static struct Glyph *glyph(RFont *r, unsigned char ch) {
    struct Glyph *g = &r->g[ch];
    if (g->ok) return g;
    stbtt_fontinfo *ti = &g_ttf[r->bold];
    int cp = ch; /* Latin-1 == Unicode for 0..255; cp1252 extras map close enough */
    int adv, lsb;
    stbtt_GetCodepointHMetrics(ti, cp, &adv, &lsb);
    g->adv = (int)(adv * r->scale + 0.5f);
    if (ch > ' ') g->a = stbtt_GetCodepointBitmap(ti, r->scale, r->scale, cp, &g->w, &g->h, &g->xo, &g->yo);
    g->ok = 1;
    return g;
}

/* ---- DCs ----------------------------------------------------------------------------------- */
#define MAX_DC 256
#define DC_BASE 0x02000000u

typedef struct DCState {
    uint32_t pen, brush, font, bitmap, pal;
    uint32_t textcolor, bkcolor; /* COLORREF */
    int bkmode, rop2, textalign, mapmode, stretchmode;
    int vorgx, vorgy, worgx, worgy, vextx, vexty, wextx, wexty;
    int curx, cury;
    Rect *uclip; int nuclip; int has_uclip; /* user clip (device coords, relative to DC origin) */
} DCState;

typedef struct {
    int used, kind;
    uint32_t hwnd, surface;
    int client;
    DCState s;
    DCState saved[8];
    int nsaved;
} DC;

static DC g_dc[MAX_DC];

static DC *dc_get(uint32_t h) {
    if (h < DC_BASE || h >= DC_BASE + MAX_DC * 4 || (h & 3)) return NULL;
    DC *d = &g_dc[(h - DC_BASE) / 4];
    return d->used ? d : NULL;
}

uint32_t gdi_dc_new(int kind, uint32_t hwnd, uint32_t surface) {
    stock_init();
    pthread_mutex_lock(&g_gdi_lock);
    for (int i = 1; i < MAX_DC; i++)
        if (!g_dc[i].used) {
            DC *d = &g_dc[i];
            memset(d, 0, sizeof *d);
            d->used = 1;
            d->kind = kind;
            d->hwnd = hwnd;
            d->surface = surface;
            d->client = 1;
            d->s.pen = g_stock[7];
            d->s.brush = g_stock[0];
            d->s.font = g_stock[13];
            d->s.bitmap = kind == DC_MEMORY ? g_stock[19] : 0;
            d->s.pal = g_stock[15];
            d->s.textcolor = 0;
            d->s.bkcolor = 0xffffff;
            d->s.bkmode = 2; /* OPAQUE */
            d->s.rop2 = 13;  /* R2_COPYPEN */
            d->s.mapmode = 1;
            d->s.vextx = d->s.vexty = d->s.wextx = d->s.wexty = 1;
            pthread_mutex_unlock(&g_gdi_lock);
            return DC_BASE + (uint32_t)i * 4;
        }
    pthread_mutex_unlock(&g_gdi_lock);
    rt_log("gdi: out of DCs");
    return 0;
}

void gdi_dc_free(uint32_t h) {
    DC *d = dc_get(h);
    if (!d) return;
    free(d->s.uclip);
    for (int i = 0; i < d->nsaved; i++) free(d->saved[i].uclip);
    d->used = 0;
}

void gdi_dc_set_window_part(uint32_t hdc, int client) {
    DC *d = dc_get(hdc);
    if (d) d->client = client;
}

/* resolves a DC to its canvas, device origin and base clip */
static int dc_target(DC *d, Canvas *c, int *ox, int *oy, Rect *clip) {
    *ox = *oy = 0;
    switch (d->kind) {
    case DC_MEMORY: {
        GdiObj *b = obj_get(d->s.bitmap, OBJ_BITMAP);
        if (!b || !b->u.bmp.bits) return -1;
        c->bits = (uint8_t *)GPTR(b->u.bmp.bits);
        c->w = b->u.bmp.w; c->h = b->u.bmp.h; c->pitch = b->u.bmp.pitch; c->bpp = b->u.bmp.bpp;
        c->rgb565 = 0;
        c->pal = b->u.bmp.pal_n ? b->u.bmp.pal : NULL;
        c->pal_n = b->u.bmp.pal_n;
        *clip = (Rect){0, 0, c->w, c->h};
        return 0;
    }
    case DC_SURFACE:
        if (ddraw_surface_canvas(d->surface, c)) return -1;
        *clip = (Rect){0, 0, c->w, c->h};
        return 0;
    case DC_WINDOW:
    case DC_SCREEN: {
        *c = *desktop_canvas();
        if (d->kind == DC_SCREEN || !d->hwnd) { *clip = (Rect){0, 0, c->w, c->h}; return 0; }
        if (user32_dc_geometry(d->hwnd, d->client, ox, oy, clip)) return -1;
        *clip = rect_isect(*clip, (Rect){0, 0, c->w, c->h});
        return 0;
    }
    }
    return -1;
}

/* logical -> device (relative to DC origin) */
static int lpx(DC *d, int x) {
    if (d->s.mapmode == 1) return x - d->s.worgx + d->s.vorgx;
    return (int)((int64_t)(x - d->s.worgx) * d->s.vextx / (d->s.wextx ? d->s.wextx : 1)) + d->s.vorgx;
}
static int lpy(DC *d, int y) {
    if (d->s.mapmode == 1) return y - d->s.worgy + d->s.vorgy;
    return (int)((int64_t)(y - d->s.worgy) * d->s.vexty / (d->s.wexty ? d->s.wexty : 1)) + d->s.vorgy;
}
static int lpw(DC *d, int w) { return d->s.mapmode == 1 ? w : (int)((int64_t)w * d->s.vextx / (d->s.wextx ? d->s.wextx : 1)); }

/* A drawing context: canvas + clip rects in canvas coordinates. */
typedef struct {
    Canvas c;
    int ox, oy;
    Rect clip[64];
    int nclip;
} Target;

static int target_get(uint32_t hdc, Target *t) {
    DC *d = dc_get(hdc);
    if (!d) return -1;
    Rect base;
    if (dc_target(d, &t->c, &t->ox, &t->oy, &base)) return -1;
    t->nclip = 0;
    if (d->s.has_uclip) {
        for (int i = 0; i < d->s.nuclip && t->nclip < 64; i++) {
            Rect u = d->s.uclip[i];
            Rect r = rect_isect(base, (Rect){u.l + t->ox, u.t + t->oy, u.r + t->ox, u.b + t->oy});
            if (!rect_empty(r)) t->clip[t->nclip++] = r;
        }
    } else if (!rect_empty(base)) {
        t->clip[t->nclip++] = base;
    }
    return 0;
}

static inline int clip_has(const Target *t, int x, int y) {
    for (int i = 0; i < t->nclip; i++)
        if (x >= t->clip[i].l && x < t->clip[i].r && y >= t->clip[i].t && y < t->clip[i].b) return 1;
    return 0;
}

/* fills device rect r (relative to DC origin) with a raw pixel / rgb, honouring clip */
static void t_fill_rgb(Target *t, Rect r, uint32_t rgb) {
    r.l += t->ox; r.r += t->ox; r.t += t->oy; r.b += t->oy;
    uint32_t v = canvas_pixel_for(&t->c, rgb);
    for (int i = 0; i < t->nclip; i++) fill_raw(&t->c, rect_isect(r, t->clip[i]), v);
}

static const uint8_t g_hatch[6][8] = {
    {0, 0, 0, 0xff, 0, 0, 0, 0},                          /* HS_HORIZONTAL */
    {8, 8, 8, 8, 8, 8, 8, 8},                             /* HS_VERTICAL */
    {0x80, 0x40, 0x20, 0x10, 8, 4, 2, 1},                 /* HS_FDIAGONAL */
    {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80},                 /* HS_BDIAGONAL */
    {8, 8, 8, 0xff, 8, 8, 8, 8},                          /* HS_CROSS */
    {0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81}};    /* HS_DIAGCROSS */

static void t_fill_brush(Target *t, DC *d, Rect r, uint32_t hbrush) {
    GdiObj *b = obj_get(hbrush, OBJ_BRUSH);
    if (!b || b->u.brush.style == 1) return;
    if (b->u.brush.style != 2) { t_fill_rgb(t, r, b->u.brush.color); return; }
    /* hatched: foreground in brush colour, background per bk mode */
    uint32_t fg = canvas_pixel_for(&t->c, b->u.brush.color), bg = canvas_pixel_for(&t->c, RGB_OF(d->s.bkcolor));
    int h = b->u.brush.hatch < 0 || b->u.brush.hatch > 5 ? 0 : b->u.brush.hatch;
    r.l += t->ox; r.r += t->ox; r.t += t->oy; r.b += t->oy;
    for (int i = 0; i < t->nclip; i++) {
        Rect q = rect_isect(r, t->clip[i]);
        for (int y = q.t; y < q.b; y++)
            for (int x = q.l; x < q.r; x++) {
                if ((g_hatch[h][y & 7] >> (7 - (x & 7))) & 1) raw_put(&t->c, x, y, fg);
                else if (d->s.bkmode == 2) raw_put(&t->c, x, y, bg);
            }
    }
}

static void t_pixel(Target *t, int x, int y, uint32_t v, int rop2) {
    x += t->ox; y += t->oy;
    if (!clip_has(t, x, y)) return;
    if (rop2 == 6) raw_put(&t->c, x, y, ~raw_get(&t->c, x, y) & (t->c.bpp == 8 ? 0xff : 0xffffff)); /* R2_NOT */
    else if (rop2 == 7) raw_put(&t->c, x, y, raw_get(&t->c, x, y) ^ v);                           /* R2_XORPEN */
    else raw_put(&t->c, x, y, v);
}

static void t_line(Target *t, DC *d, int x0, int y0, int x1, int y1) {
    GdiObj *p = obj_get(d->s.pen, OBJ_PEN);
    if (!p || p->u.pen.style == 5) return;
    uint32_t v = canvas_pixel_for(&t->c, p->u.pen.color);
    int w = p->u.pen.width > 1 ? lpw(d, p->u.pen.width) : 1;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    int n = 0;
    for (;;) {
        /* the end point is not drawn (Windows convention) */
        if (x0 == x1 && y0 == y1) break;
        int dot = p->u.pen.style == 0 || p->u.pen.style == 6 || ((n >> 2) & 1) == 0;
        n++;
        if (dot) {
            if (w <= 1) t_pixel(t, x0, y0, v, d->s.rop2);
            else
                for (int a = -w / 2; a < w - w / 2; a++)
                    for (int b = -w / 2; b < w - w / 2; b++) t_pixel(t, x0 + a, y0 + b, v, d->s.rop2);
        }
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ---- text ---------------------------------------------------------------------------------- */
static RFont *dc_rfont(DC *d) { return rfont_get(obj_get(d->s.font, OBJ_FONT)); }

static int text_width_rf(RFont *rf, const char *s, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) w += glyph(rf, (unsigned char)s[i])->adv;
    return w;
}

/* draws text with its top-left at device (x,y) relative to the DC origin */
static void t_text(Target *t, DC *d, int x, int y, const char *s, int n, int underline_at) {
    RFont *rf = dc_rfont(d);
    uint32_t col = RGB_OF(d->s.textcolor);
    uint32_t v = canvas_pixel_for(&t->c, col);
    int bx = x + t->ox, by = y + rf->ascent + t->oy;
    for (int i = 0; i < n; i++) {
        struct Glyph *g = glyph(rf, (unsigned char)s[i]);
        if (g->a) {
            for (int gy = 0; gy < g->h; gy++)
                for (int gx = 0; gx < g->w; gx++) {
                    int a = g->a[gy * g->w + gx];
                    if (!a) continue;
                    int px = bx + g->xo + gx, py = by + g->yo + gy;
                    if (!clip_has(t, px, py)) continue;
                    if (t->c.bpp <= 8 || a >= 250) {
                        if (a >= 112) raw_put(&t->c, px, py, v);
                    } else {
                        uint32_t o = rgb_of_pixel(&t->c, raw_get(&t->c, px, py));
                        uint32_t r = (((col >> 16) & 255) * a + ((o >> 16) & 255) * (255 - a)) / 255;
                        uint32_t gg = (((col >> 8) & 255) * a + ((o >> 8) & 255) * (255 - a)) / 255;
                        uint32_t b = ((col & 255) * a + (o & 255) * (255 - a)) / 255;
                        raw_put(&t->c, px, py, canvas_pixel_for(&t->c, r << 16 | gg << 8 | b));
                    }
                }
        }
        if (i == underline_at) {
            for (int k = 0; k < g->adv; k++) {
                int px = bx + k, py = by + 1;
                if (clip_has(t, px, py)) raw_put(&t->c, px, py, v);
            }
        }
        bx += g->adv;
    }
}

int gdi_text_width(uint32_t hdc, const char *s, int n) {
    DC *d = dc_get(hdc);
    RFont *rf = d ? dc_rfont(d) : rfont_get(NULL);
    return text_width_rf(rf, s, n);
}

int gdi_font_height(uint32_t hdc) {
    DC *d = dc_get(hdc);
    RFont *rf = d ? dc_rfont(d) : rfont_get(NULL);
    return rf->ascent + rf->descent;
}

#define DT_CENTER 1
#define DT_RIGHT 2
#define DT_VCENTER 4
#define DT_BOTTOM 8
#define DT_WORDBREAK 0x10
#define DT_SINGLELINE 0x20
#define DT_EXPANDTABS 0x40
#define DT_NOCLIP 0x100
#define DT_CALCRECT 0x400
#define DT_NOPREFIX 0x800
#define DT_END_ELLIPSIS 0x8000

/* DrawText: lays out lines (word wrap, '&' prefixes) inside r */
int gdi_draw_text(uint32_t hdc, const char *s, int n, Rect *r, uint32_t flags) {
    DC *d = dc_get(hdc);
    if (!d) return 0;
    if (n < 0) n = (int)strlen(s);
    RFont *rf = dc_rfont(d);
    int lh = rf->ascent + rf->descent;
    /* strip prefixes into a working copy, remember underline positions */
    char buf[4096];
    int und[4096];
    int m = 0, pending = 0;
    for (int i = 0; i < n && m < 4095; i++) {
        if (!(flags & DT_NOPREFIX) && s[i] == '&') {
            if (i + 1 < n && s[i + 1] == '&') { und[m] = 0; buf[m++] = '&'; i++; continue; }
            pending = 1; /* underline the next character */
            continue;
        }
        if (s[i] == '\t') { for (int k = 0; k < 4 && m < 4095; k++) { und[m] = 0; buf[m++] = ' '; } continue; }
        und[m] = pending;
        pending = 0;
        buf[m++] = s[i];
    }
    int width = r->r - r->l;
    struct { int s, e; } lines[256];
    int nl = 0;
    int i = 0;
    while (i <= m && nl < 256) {
        int st = i, last_space = -1, w = 0, e = i;
        while (e < m && buf[e] != '\n' && buf[e] != '\r') {
            if (flags & DT_SINGLELINE) { e++; continue; }
            int cw = glyph(rf, (unsigned char)buf[e])->adv;
            if ((flags & DT_WORDBREAK) && w + cw > width && e > st) {
                if (buf[e] == ' ') break;
                if (last_space > st) { e = last_space; break; }
                break;
            }
            if (buf[e] == ' ') last_space = e;
            w += cw;
            e++;
        }
        lines[nl].s = st;
        lines[nl].e = e;
        nl++;
        i = e;
        if (i < m && buf[i] == '\r') i++;
        if (i < m && buf[i] == '\n') i++;
        else if (i < m && buf[i] == ' ') i++;
        if (i >= m) break;
        if (flags & DT_SINGLELINE) break;
    }
    int total = nl * lh;
    if (flags & DT_CALCRECT) {
        int maxw = 0;
        for (int k = 0; k < nl; k++) {
            int w = text_width_rf(rf, buf + lines[k].s, lines[k].e - lines[k].s);
            if (w > maxw) maxw = w;
        }
        if (!(flags & DT_WORDBREAK) || maxw < width) r->r = r->l + maxw;
        r->b = r->t + total;
        return total;
    }
    Target t;
    if (target_get(hdc, &t)) return total;
    int y = r->t;
    if (flags & DT_SINGLELINE) {
        if (flags & DT_VCENTER) y = (r->t + r->b - lh) / 2;
        else if (flags & DT_BOTTOM) y = r->b - lh;
    }
    if (!(flags & DT_NOCLIP)) {
        Rect cr = {lpx(d, r->l) + t.ox, lpy(d, r->t) + t.oy, lpx(d, r->r) + t.ox, lpy(d, r->b) + t.oy};
        for (int k = 0; k < t.nclip; k++) t.clip[k] = rect_isect(t.clip[k], cr);
    }
    for (int k = 0; k < nl; k++, y += lh) {
        int len = lines[k].e - lines[k].s;
        int w = text_width_rf(rf, buf + lines[k].s, len);
        int x = r->l;
        if (flags & DT_CENTER) x = (r->l + r->r - w) / 2;
        else if (flags & DT_RIGHT) x = r->r - w;
        int ul = -1;
        for (int q = 0; q < len; q++) if (und[lines[k].s + q]) ul = q;
        if (d->s.bkmode == 2) t_fill_rgb(&t, (Rect){lpx(d, x), lpy(d, y), lpx(d, x) + w, lpy(d, y) + lh}, RGB_OF(d->s.bkcolor));
        t_text(&t, d, lpx(d, x), lpy(d, y), buf + lines[k].s, len, ul);
    }
    return total;
}

/* ---- blits --------------------------------------------------------------------------------- */
#define SRCCOPY 0x00CC0020u
#define SRCPAINT 0x00EE0086u
#define SRCAND 0x008800C6u
#define SRCINVERT 0x00660046u
#define SRCERASE 0x00440328u
#define NOTSRCCOPY 0x00330008u
#define NOTSRCERASE 0x001100A6u
#define MERGECOPY 0x00C000CAu
#define MERGEPAINT 0x00BB0226u
#define PATCOPY 0x00F00021u
#define PATPAINT 0x00FB0A09u
#define PATINVERT 0x005A0049u
#define DSTINVERT 0x00550009u
#define BLACKNESS 0x00000042u
#define WHITENESS 0x00FF0062u

static uint32_t apply_rop(uint32_t rop, uint32_t s, uint32_t dpx, uint32_t p, uint32_t mask) {
    switch (rop) {
    case SRCCOPY: return s;
    case SRCPAINT: return s | dpx;
    case SRCAND: return s & dpx;
    case SRCINVERT: return s ^ dpx;
    case SRCERASE: return s & ~dpx & mask;
    case NOTSRCCOPY: return ~s & mask;
    case NOTSRCERASE: return ~(s | dpx) & mask;
    case MERGECOPY: return s & p;
    case MERGEPAINT: return (~s & mask) | dpx;
    case PATCOPY: return p;
    case PATPAINT: return p | (~s & mask) | dpx;
    case PATINVERT: return p ^ dpx;
    case DSTINVERT: return ~dpx & mask;
    case BLACKNESS: return 0;
    case WHITENESS: return mask;
    }
    return s;
}

static int rop_uses_src(uint32_t rop) { return ((rop >> 2) ^ rop) & 0x330000u; }

/* converts a source pixel to the destination format (Windows rules for mono) */
static uint32_t convert_px(const Canvas *s, uint32_t v, const Canvas *d, DC *sdc, DC *ddc) {
    if (s->bpp == d->bpp && (s->bpp != 8 || s->pal == d->pal || !s->pal || !d->pal)) return v;
    if (s->bpp == 1) /* mono source: 0 -> text colour, 1 -> background colour of the dest DC */
        return canvas_pixel_for(d, RGB_OF(v ? ddc->s.bkcolor : ddc->s.textcolor));
    uint32_t rgb = rgb_of_pixel(s, v);
    if (d->bpp == 1) return rgb == RGB_OF(sdc->s.bkcolor) ? 1 : 0;
    return canvas_pixel_for(d, rgb);
}

static int do_blit(uint32_t hdst, int x, int y, int w, int h, uint32_t hsrc, int sx, int sy, int sw, int sh, uint32_t rop) {
    DC *dd = dc_get(hdst);
    if (!dd) return 0;
    Target t;
    if (target_get(hdst, &t)) return 0;
    int dx0 = lpx(dd, x), dy0 = lpy(dd, y), dx1 = lpx(dd, x + w), dy1 = lpy(dd, y + h);
    if (dx1 < dx0) { int q = dx0; dx0 = dx1; dx1 = q; }
    if (dy1 < dy0) { int q = dy0; dy0 = dy1; dy1 = q; }
    uint32_t mask = t.c.bpp == 8 ? 0xff : t.c.bpp == 16 ? 0xffff : t.c.bpp == 1 ? 1 : 0xffffff;
    GdiObj *br = obj_get(dd->s.brush, OBJ_BRUSH);
    uint32_t pat = br && br->u.brush.style != 1 ? canvas_pixel_for(&t.c, br->u.brush.color) : 0;
    Rect dr = {dx0 + t.ox, dy0 + t.oy, dx1 + t.ox, dy1 + t.oy};
    if (!rop_uses_src(rop)) {
        for (int i = 0; i < t.nclip; i++) {
            Rect q = rect_isect(dr, t.clip[i]);
            if (rop == PATCOPY || rop == BLACKNESS || rop == WHITENESS) {
                fill_raw(&t.c, q, apply_rop(rop, 0, 0, pat, mask));
                continue;
            }
            for (int yy = q.t; yy < q.b; yy++)
                for (int xx = q.l; xx < q.r; xx++) raw_put(&t.c, xx, yy, apply_rop(rop, 0, raw_get(&t.c, xx, yy), pat, mask));
        }
        return 1;
    }
    DC *sd = dc_get(hsrc);
    if (!sd) return 0;
    Canvas sc;
    int sox, soy;
    Rect sclip;
    if (dc_target(sd, &sc, &sox, &soy, &sclip)) return 0;
    int ssx = lpx(sd, sx) + sox, ssy = lpy(sd, sy) + soy;
    int ssw = lpw(sd, sw), ssh = sd->s.mapmode == 1 ? sh : (int)((int64_t)sh * sd->s.vexty / (sd->s.wexty ? sd->s.wexty : 1));
    int dw = dx1 - dx0, dh = dy1 - dy0;
    int flipx = (w < 0) != (sw < 0), flipy = (h < 0) != (sh < 0);
    if (ssw < 0) ssw = -ssw;
    if (ssh < 0) ssh = -ssh;
    if (!dw || !dh || !ssw || !ssh) return 1;
    int same = sc.bpp == t.c.bpp && (sc.bpp != 8 || sc.pal == t.c.pal || !sc.pal || !t.c.pal);
    for (int i = 0; i < t.nclip; i++) {
        Rect q = rect_isect(dr, t.clip[i]);
        for (int yy = q.t; yy < q.b; yy++) {
            int ry = yy - dr.t;
            int syy = ssy + (int)((int64_t)(flipy ? dh - 1 - ry : ry) * ssh / dh);
            if (syy < 0 || syy >= sc.h) continue;
            /* fast path: unscaled same-format copy */
            if (rop == SRCCOPY && same && dw == ssw && !flipx && sc.bpp >= 8) {
                int sx0 = ssx + (q.l - dr.l), n = q.r - q.l, off = 0;
                if (sx0 < 0) { off = -sx0; sx0 = 0; }
                if (sx0 + n - off > sc.w) n = sc.w - sx0 + off;
                if (n - off > 0) memmove(px_at(&t.c, q.l + off, yy), px_at(&sc, sx0, syy), (size_t)(n - off) * (sc.bpp / 8));
                if (t.c.bpp == 32) for (int xx = q.l + off; xx < q.l + n; xx++) *(uint32_t *)px_at(&t.c, xx, yy) |= 0xff000000u;
                continue;
            }
            for (int xx = q.l; xx < q.r; xx++) {
                int rx = xx - dr.l;
                int sxx = ssx + (int)((int64_t)(flipx ? dw - 1 - rx : rx) * ssw / dw);
                if (sxx < 0 || sxx >= sc.w) continue;
                uint32_t s = convert_px(&sc, raw_get(&sc, sxx, syy), &t.c, sd, dd);
                uint32_t dpx = rop == SRCCOPY ? 0 : raw_get(&t.c, xx, yy);
                raw_put(&t.c, xx, yy, apply_rop(rop, s, dpx, pat, mask));
            }
        }
    }
    return 1;
}

void gdi_blit_simple(uint32_t dst, int x, int y, int w, int h, uint32_t src) {
    do_blit(dst, x, y, w, h, src, 0, 0, w, h, SRCCOPY);
}

/* ---- helpers for user32 ----------------------------------------------------------------- */
uint32_t gdi_select(uint32_t hdc, uint32_t obj);

void gdi_fill_rect_rgb(uint32_t hdc, Rect r, uint32_t colorref) {
    DC *d = dc_get(hdc);
    Target t;
    if (!d || target_get(hdc, &t)) return;
    t_fill_rgb(&t, (Rect){lpx(d, r.l), lpy(d, r.t), lpx(d, r.r), lpy(d, r.b)}, RGB_OF(colorref));
}

void gdi_fill_rect_brush(uint32_t hdc, Rect r, uint32_t hbrush) {
    DC *d = dc_get(hdc);
    Target t;
    if (!d || target_get(hdc, &t)) return;
    if (hbrush >= 1 && hbrush <= 31) hbrush = sys_color_brush((int)hbrush - 1); /* COLOR_x + 1 */
    t_fill_brush(&t, d, (Rect){lpx(d, r.l), lpy(d, r.t), lpx(d, r.r), lpy(d, r.b)}, hbrush);
}

void gdi_frame_rect_rgb(uint32_t hdc, Rect r, uint32_t cr) {
    gdi_fill_rect_rgb(hdc, (Rect){r.l, r.t, r.r, r.t + 1}, cr);
    gdi_fill_rect_rgb(hdc, (Rect){r.l, r.b - 1, r.r, r.b}, cr);
    gdi_fill_rect_rgb(hdc, (Rect){r.l, r.t, r.l + 1, r.b}, cr);
    gdi_fill_rect_rgb(hdc, (Rect){r.r - 1, r.t, r.r, r.b}, cr);
}

/* classic 3D edge: raised (buttons) or sunken (edits), 2 px deep or 1 */
void gdi_draw_edge(uint32_t hdc, Rect r, int raised, int deep) {
    uint32_t hi = sys_color(20), lo = sys_color(16), dk = sys_color(21), lt = sys_color(22);
    uint32_t a1 = raised ? lt : lo, b1 = raised ? dk : hi;
    uint32_t a2 = raised ? hi : dk, b2 = raised ? lo : lt;
    if (deep) {
        gdi_fill_rect_rgb(hdc, (Rect){r.l, r.t, r.r - 1, r.t + 1}, a1);
        gdi_fill_rect_rgb(hdc, (Rect){r.l, r.t, r.l + 1, r.b - 1}, a1);
        gdi_fill_rect_rgb(hdc, (Rect){r.l, r.b - 1, r.r, r.b}, b1);
        gdi_fill_rect_rgb(hdc, (Rect){r.r - 1, r.t, r.r, r.b}, b1);
        r.l++; r.t++; r.r--; r.b--;
    }
    gdi_fill_rect_rgb(hdc, (Rect){r.l, r.t, r.r - 1, r.t + 1}, a2);
    gdi_fill_rect_rgb(hdc, (Rect){r.l, r.t, r.l + 1, r.b - 1}, a2);
    gdi_fill_rect_rgb(hdc, (Rect){r.l, r.b - 1, r.r, r.b}, b2);
    gdi_fill_rect_rgb(hdc, (Rect){r.r - 1, r.t, r.r, r.b}, b2);
}

void gdi_set_text_color(uint32_t hdc, uint32_t c) { DC *d = dc_get(hdc); if (d) d->s.textcolor = c; }
void gdi_set_bk_mode(uint32_t hdc, int mode) { DC *d = dc_get(hdc); if (d) d->s.bkmode = mode; }

uint32_t gdi_dialog_font(int pt) {
    static uint32_t f[32];
    if (pt < 1 || pt > 31) pt = 8;
    if (!f[pt]) {
        stock_init();
        f[pt] = make_font(-(pt * 96 + 36) / 72, 400, "MS Sans Serif");
        obj_get(f[pt], 0)->stock = 1;
    }
    return f[pt];
}

/* ---- icons ------------------------------------------------------------------------------------ */
/* An icon handle is a bitmap object holding a 32 bpp copy plus alpha (from the AND mask). */
#define MAX_ICONS 64
static struct { uint32_t h; int w, hgt; uint32_t *px; } g_icons[MAX_ICONS];
static int g_nicons;

uint32_t gdi_load_icon(const char *name) {
    uint32_t grp = res_find_host(14, name);
    if (!grp) return 0;
    uint32_t d = res_data(grp, NULL);
    int n = RD16(d + 4), best = -1, bestw = 0;
    for (int i = 0; i < n; i++) {
        uint32_t e = d + 6 + 14 * i;
        int w = RD8(e) ? RD8(e) : 256, bpp = RD16(e + 6);
        int score = (w == 32 ? 1000 : w) + bpp;
        if (score > bestw) { bestw = score; best = i; }
    }
    if (best < 0) return 0;
    char nm[16];
    snprintf(nm, sizeof nm, "#%u", RD16(d + 6 + 14 * best + 12));
    uint32_t ie = res_find_host(3, nm);
    if (!ie) return 0;
    uint32_t b = res_data(ie, NULL);
    int w = (int)RD32(b + 4), h = (int)RD32(b + 8) / 2, bpp = RD16(b + 14);
    uint32_t ncol = RD32(b + 32);
    if (!ncol && bpp <= 8) ncol = 1u << bpp;
    uint32_t pal = b + RD32(b), xor_bits = pal + ncol * 4;
    int xpitch = ((w * bpp + 31) / 32) * 4, apitch = ((w + 31) / 32) * 4;
    uint32_t and_bits = xor_bits + (uint32_t)(xpitch * h);
    uint32_t *px = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        int sy = h - 1 - y;
        for (int x = 0; x < w; x++) {
            uint32_t c = 0;
            uint32_t row = xor_bits + (uint32_t)(sy * xpitch);
            if (bpp == 32) c = RD32(row + x * 4) & 0xffffff;
            else if (bpp == 24) c = RD8(row + x * 3) | RD8(row + x * 3 + 1) << 8 | (uint32_t)RD8(row + x * 3 + 2) << 16;
            else {
                int bit = x * bpp;
                int v = (RD8(row + (uint32_t)(bit / 8)) >> (8 - bpp - (bit % 8))) & ((1 << bpp) - 1);
                c = RD32(pal + 4 * (uint32_t)v) & 0xffffff;
            }
            int transparent = (RD8(and_bits + (uint32_t)(sy * apitch + x / 8)) >> (7 - (x & 7))) & 1;
            px[y * w + x] = transparent ? 0 : (0xff000000u | c);
        }
    }
    if (g_nicons >= MAX_ICONS) { free(px); return 0; }
    uint32_t hicon = 0x04000000u + (uint32_t)g_nicons * 4;
    g_icons[g_nicons].h = hicon;
    g_icons[g_nicons].w = w;
    g_icons[g_nicons].hgt = h;
    g_icons[g_nicons].px = px;
    g_nicons++;
    return hicon;
}

int gdi_draw_icon_handle(uint32_t hdc, int x, int y, uint32_t hicon) {
    DC *d = dc_get(hdc);
    Target t;
    if (!d || target_get(hdc, &t)) return 0;
    for (int i = 0; i < g_nicons; i++)
        if (g_icons[i].h == hicon) {
            for (int yy = 0; yy < g_icons[i].hgt; yy++)
                for (int xx = 0; xx < g_icons[i].w; xx++) {
                    uint32_t c = g_icons[i].px[yy * g_icons[i].w + xx];
                    if (!(c >> 24)) continue;
                    int px = lpx(d, x) + xx + t.ox, py = lpy(d, y) + yy + t.oy;
                    if (clip_has(&t, px, py)) raw_put(&t.c, px, py, canvas_pixel_for(&t.c, c & 0xffffff));
                }
            return 1;
        }
    return 0;
}

/* ---- bitmaps ------------------------------------------------------------------------------------ */
static uint32_t bitmap_new(int w, int h, int bpp, const uint32_t *pal, int pal_n) {
    uint32_t hb = obj_new(OBJ_BITMAP);
    GdiObj *b = obj_get(hb, OBJ_BITMAP);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    b->u.bmp.w = w;
    b->u.bmp.h = h;
    b->u.bmp.bpp = bpp;
    b->u.bmp.pitch = bpp == 1 ? ((w + 15) / 16) * 2 : ((w * bpp / 8 + 3) & ~3);
    b->u.bmp.alloc = b->u.bmp.bits = heap_alloc((uint32_t)(b->u.bmp.pitch * h), 1);
    if (pal && pal_n) { memcpy(b->u.bmp.pal, pal, (size_t)pal_n * 4); b->u.bmp.pal_n = pal_n; }
    return hb;
}

/* builds a bitmap object over a guest BITMAPINFO + bits (DIB sections, resources) */
static uint32_t bitmap_from_dib(uint32_t bmi, uint32_t bits, int own) {
    int32_t w = (int32_t)RD32(bmi + 4), h = (int32_t)RD32(bmi + 8);
    int bpp = RD16(bmi + 14);
    uint32_t comp = RD32(bmi + 16), ncol = RD32(bmi + 32);
    if (!ncol && bpp <= 8) ncol = 1u << bpp;
    uint32_t hb = obj_new(OBJ_BITMAP);
    GdiObj *b = obj_get(hb, OBJ_BITMAP);
    int ah = h < 0 ? -h : h;
    int pitch = ((w * bpp + 31) / 32) * 4;
    b->u.bmp.w = w;
    b->u.bmp.h = ah;
    b->u.bmp.bpp = bpp;
    b->u.bmp.dib = 1;
    if (h > 0) { b->u.bmp.bits = bits + (uint32_t)(pitch * (ah - 1)); b->u.bmp.pitch = -pitch; }
    else { b->u.bmp.bits = bits; b->u.bmp.pitch = pitch; }
    if (own) b->u.bmp.alloc = bits;
    if (bpp <= 8) {
        uint32_t pal = bmi + RD32(bmi);
        for (uint32_t i = 0; i < ncol && i < 256; i++) b->u.bmp.pal[i] = RD32(pal + 4 * i) & 0xffffff;
        b->u.bmp.pal_n = (int)(ncol > 256 ? 256 : ncol);
    }
    (void)comp;
    return hb;
}

/* ---- GDI32 exports ---------------------------------------------------------------------------- */
void imp_GDI32_GetStockObject(CPU *cpu) { RET_STDCALL(1, gdi_stock((int)ARG(0))); }

void imp_GDI32_DeleteObject(CPU *cpu) {
    pthread_mutex_lock(&g_gdi_lock);
    obj_free(ARG(0));
    pthread_mutex_unlock(&g_gdi_lock);
    RET_STDCALL(1, 1);
}

uint32_t gdi_select(uint32_t hdc, uint32_t h) {
    DC *d = dc_get(hdc);
    GdiObj *o = obj_get(h, 0);
    if (!d || !o) return 0;
    uint32_t old = 0;
    switch (o->type) {
    case OBJ_PEN: old = d->s.pen; d->s.pen = h; break;
    case OBJ_BRUSH: old = d->s.brush; d->s.brush = h; break;
    case OBJ_FONT: old = d->s.font; d->s.font = h; break;
    case OBJ_BITMAP:
        if (d->kind != DC_MEMORY) return 0;
        old = d->s.bitmap; d->s.bitmap = h; break;
    case OBJ_REGION: {
        free(d->s.uclip);
        d->s.nuclip = o->u.rgn.n;
        d->s.uclip = malloc(sizeof(Rect) * (size_t)(o->u.rgn.n ? o->u.rgn.n : 1));
        memcpy(d->s.uclip, o->u.rgn.r, sizeof(Rect) * (size_t)o->u.rgn.n);
        d->s.has_uclip = 1;
        return 1; /* SIMPLEREGION */
    }
    case OBJ_PAL: old = d->s.pal; d->s.pal = h; break;
    }
    return old;
}

void imp_GDI32_SelectObject(CPU *cpu) { RET_STDCALL(2, gdi_select(ARG(0), ARG(1))); }

void imp_GDI32_CreatePen(CPU *cpu) { stock_init(); RET_STDCALL(3, make_pen((int)ARG(0), (int)ARG(1), RGB_OF(ARG(2)))); }
void imp_GDI32_CreateSolidBrush(CPU *cpu) { RET_STDCALL(1, gdi_create_solid_brush(ARG(0))); }
void imp_GDI32_CreateHatchBrush(CPU *cpu) { stock_init(); RET_STDCALL(2, make_brush(2, RGB_OF(ARG(1)), (int)ARG(0))); }

static uint32_t font_from_logfont(uint32_t lf) {
    stock_init();
    uint32_t h = obj_new(OBJ_FONT);
    GdiObj *o = obj_get(h, OBJ_FONT);
    o->u.font.height = (int32_t)RD32(lf);
    o->u.font.width = (int32_t)RD32(lf + 4);
    o->u.font.weight = (int32_t)RD32(lf + 16);
    o->u.font.italic = RD8(lf + 20);
    o->u.font.underline = RD8(lf + 21);
    o->u.font.strike = RD8(lf + 22);
    o->u.font.charset = RD8(lf + 23);
    snprintf(o->u.font.face, sizeof o->u.font.face, "%.31s", gstr(lf + 28));
    return h;
}

void imp_GDI32_CreateFontIndirectA(CPU *cpu) { RET_STDCALL(1, font_from_logfont(ARG(0))); }

void imp_GDI32_CreateFontA(CPU *cpu) {
    stock_init();
    uint32_t h = obj_new(OBJ_FONT);
    GdiObj *o = obj_get(h, OBJ_FONT);
    o->u.font.height = (int32_t)ARG(0);
    o->u.font.width = (int32_t)ARG(1);
    o->u.font.weight = (int32_t)ARG(4);
    o->u.font.italic = (uint8_t)ARG(5);
    o->u.font.underline = (uint8_t)ARG(6);
    o->u.font.strike = (uint8_t)ARG(7);
    o->u.font.charset = (uint8_t)ARG(8);
    if (ARG(13)) snprintf(o->u.font.face, sizeof o->u.font.face, "%.31s", gstr(ARG(13)));
    RET_STDCALL(14, h);
}

void imp_GDI32_CreateBitmap(CPU *cpu) {
    stock_init();
    int w = (int)ARG(0), h = (int)ARG(1), bpp = (int)(ARG(2) * ARG(3));
    uint32_t hb = bitmap_new(w, h, bpp, NULL, 0);
    GdiObj *b = obj_get(hb, OBJ_BITMAP);
    if (ARG(4)) {
        /* CreateBitmap rows are WORD aligned */
        int src_pitch = ((w * bpp + 15) / 16) * 2;
        for (int y = 0; y < h; y++)
            memcpy(GPTR(b->u.bmp.bits + (uint32_t)(y * b->u.bmp.pitch)), GPTR(ARG(4) + (uint32_t)(y * src_pitch)),
                   (size_t)(src_pitch < b->u.bmp.pitch ? src_pitch : b->u.bmp.pitch));
    }
    RET_STDCALL(5, hb);
}

void imp_GDI32_CreateCompatibleBitmap(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    Canvas c = {0};
    int ox, oy;
    Rect clip;
    int bpp = 32;
    const uint32_t *pal = NULL;
    int pal_n = 0;
    if (d && !dc_target(d, &c, &ox, &oy, &clip)) { bpp = c.bpp; pal = c.pal; pal_n = c.pal_n; }
    if (bpp == 16) bpp = 32; /* keep memory bitmaps in a format with an exact palette-free mapping */
    RET_STDCALL(3, bitmap_new((int)ARG(1), (int)ARG(2), bpp, pal, pal_n));
}

void imp_GDI32_CreateDIBSection(CPU *cpu) {
    stock_init();
    uint32_t bmi = ARG(1), ppv = ARG(3);
    int32_t w = (int32_t)RD32(bmi + 4), h = (int32_t)RD32(bmi + 8);
    int bpp = RD16(bmi + 14);
    uint32_t pitch = (uint32_t)(((w * bpp + 31) / 32) * 4);
    uint32_t size = pitch * (uint32_t)(h < 0 ? -h : h);
    uint32_t bits = heap_alloc(size, 1);
    uint32_t hb = bitmap_from_dib(bmi, bits, 1);
    if (ppv) WR32(ppv, bits);
    rt_log("CreateDIBSection(%dx%d, %dbpp) -> %08x bits %08x", w, h, bpp, hb, bits);
    RET_STDCALL(6, hb);
}

uint32_t gdi_load_bitmap(const char *name) {
    stock_init();
    uint32_t e = res_find_host(2, name);
    if (!e) return 0;
    uint32_t bmi = res_data(e, NULL);
    int bpp = RD16(bmi + 14);
    uint32_t ncol = RD32(bmi + 32);
    if (!ncol && bpp <= 8) ncol = 1u << bpp;
    uint32_t bits = bmi + RD32(bmi) + ncol * 4;
    if (RD32(bmi + 16) == 3) bits += 12; /* BI_BITFIELDS masks */
    return bitmap_from_dib(bmi, bits, 0);
}

void imp_GDI32_GetObjectA(CPU *cpu) {
    GdiObj *o = obj_get(ARG(0), 0);
    uint32_t n = ARG(1), p = ARG(2);
    if (!o) RET_STDCALL(3, 0);
    switch (o->type) {
    case OBJ_BITMAP: {
        if (!p) RET_STDCALL(3, 24);
        uint8_t b[24] = {0};
        int pitch = o->u.bmp.pitch < 0 ? -o->u.bmp.pitch : o->u.bmp.pitch;
        uint32_t top = o->u.bmp.bits, bits = o->u.bmp.pitch < 0 ? top - (uint32_t)(pitch * (o->u.bmp.h - 1)) : top;
        *(uint32_t *)(b + 4) = (uint32_t)o->u.bmp.w;
        *(uint32_t *)(b + 8) = (uint32_t)o->u.bmp.h;
        *(uint32_t *)(b + 12) = (uint32_t)pitch;
        *(uint16_t *)(b + 16) = 1;
        *(uint16_t *)(b + 18) = (uint16_t)o->u.bmp.bpp;
        *(uint32_t *)(b + 20) = o->u.bmp.dib ? bits : 0;
        memcpy(GPTR(p), b, n < 24 ? n : 24);
        RET_STDCALL(3, n < 24 ? n : 24);
    }
    case OBJ_FONT: {
        if (!p) RET_STDCALL(3, 60);
        uint8_t b[60] = {0};
        *(int32_t *)b = o->u.font.height;
        *(int32_t *)(b + 4) = o->u.font.width;
        *(int32_t *)(b + 16) = o->u.font.weight ? o->u.font.weight : 400;
        b[20] = o->u.font.italic; b[21] = o->u.font.underline; b[22] = o->u.font.strike; b[23] = o->u.font.charset;
        memcpy(b + 28, o->u.font.face, 32);
        memcpy(GPTR(p), b, n < 60 ? n : 60);
        RET_STDCALL(3, n < 60 ? n : 60);
    }
    case OBJ_PEN: {
        if (!p) RET_STDCALL(3, 16);
        uint8_t b[16] = {0};
        *(uint32_t *)b = (uint32_t)o->u.pen.style;
        *(uint32_t *)(b + 4) = (uint32_t)o->u.pen.width;
        *(uint32_t *)(b + 12) = CR_OF(o->u.pen.color);
        memcpy(GPTR(p), b, n < 16 ? n : 16);
        RET_STDCALL(3, n < 16 ? n : 16);
    }
    case OBJ_BRUSH: {
        if (!p) RET_STDCALL(3, 12);
        uint8_t b[12] = {0};
        *(uint32_t *)b = (uint32_t)o->u.brush.style;
        *(uint32_t *)(b + 4) = CR_OF(o->u.brush.color);
        *(uint32_t *)(b + 8) = (uint32_t)o->u.brush.hatch;
        memcpy(GPTR(p), b, n < 12 ? n : 12);
        RET_STDCALL(3, n < 12 ? n : 12);
    }
    }
    RET_STDCALL(3, 0);
}

/* ---- DC functions ----------------------------------------------------------------------------- */
void imp_GDI32_CreateCompatibleDC(CPU *cpu) { RET_STDCALL(1, gdi_dc_new(DC_MEMORY, 0, 0)); }
void imp_GDI32_CreateDCA(CPU *cpu) { RET_STDCALL(4, gdi_dc_new(DC_SCREEN, 0, 0)); }
void imp_GDI32_DeleteDC(CPU *cpu) { gdi_dc_free(ARG(0)); RET_STDCALL(1, 1); }

void imp_GDI32_SaveDC(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    if (!d || d->nsaved >= 8) RET_STDCALL(1, 0);
    d->saved[d->nsaved] = d->s;
    if (d->s.uclip) {
        d->saved[d->nsaved].uclip = malloc(sizeof(Rect) * (size_t)(d->s.nuclip ? d->s.nuclip : 1));
        memcpy(d->saved[d->nsaved].uclip, d->s.uclip, sizeof(Rect) * (size_t)d->s.nuclip);
    }
    d->nsaved++;
    RET_STDCALL(1, (uint32_t)d->nsaved);
}

void imp_GDI32_RestoreDC(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    int32_t n = (int32_t)ARG(1);
    if (!d) RET_STDCALL(2, 0);
    int target = n < 0 ? d->nsaved + n : n - 1;
    if (target < 0 || target >= d->nsaved) RET_STDCALL(2, 0);
    free(d->s.uclip);
    d->s = d->saved[target];
    for (int i = target + 1; i < d->nsaved; i++) free(d->saved[i].uclip);
    d->nsaved = target;
    RET_STDCALL(2, 1);
}

void imp_GDI32_SetTextColor(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    uint32_t old = d ? d->s.textcolor : 0;
    if (d) d->s.textcolor = ARG(1) & 0xffffff;
    RET_STDCALL(2, old);
}
void imp_GDI32_SetBkColor(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    uint32_t old = d ? d->s.bkcolor : 0;
    if (d) d->s.bkcolor = ARG(1) & 0xffffff;
    RET_STDCALL(2, old);
}
void imp_GDI32_SetBkMode(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    uint32_t old = d ? (uint32_t)d->s.bkmode : 0;
    if (d) d->s.bkmode = (int)ARG(1);
    RET_STDCALL(2, old);
}
void imp_GDI32_SetTextAlign(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    uint32_t old = d ? (uint32_t)d->s.textalign : 0;
    if (d) d->s.textalign = (int)ARG(1);
    RET_STDCALL(2, old);
}
void imp_GDI32_SetROP2(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    uint32_t old = d ? (uint32_t)d->s.rop2 : 0;
    if (d) d->s.rop2 = (int)ARG(1);
    RET_STDCALL(2, old);
}
void imp_GDI32_SetStretchBltMode(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    uint32_t old = d ? (uint32_t)d->s.stretchmode : 0;
    if (d) d->s.stretchmode = (int)ARG(1);
    RET_STDCALL(2, old ? old : 1);
}
void imp_GDI32_SetMapMode(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    uint32_t old = d ? (uint32_t)d->s.mapmode : 0;
    if (d) d->s.mapmode = (int)ARG(1);
    RET_STDCALL(2, old);
}

static void write_point(uint32_t p, int x, int y) { if (p) { WR32(p, (uint32_t)x); WR32(p + 4, (uint32_t)y); } }
void imp_GDI32_SetViewportOrgEx(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    if (!d) RET_STDCALL(4, 0);
    write_point(ARG(3), d->s.vorgx, d->s.vorgy);
    d->s.vorgx = (int)ARG(1); d->s.vorgy = (int)ARG(2);
    RET_STDCALL(4, 1);
}
void imp_GDI32_OffsetViewportOrgEx(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    if (!d) RET_STDCALL(4, 0);
    write_point(ARG(3), d->s.vorgx, d->s.vorgy);
    d->s.vorgx += (int)ARG(1); d->s.vorgy += (int)ARG(2);
    RET_STDCALL(4, 1);
}
void imp_GDI32_SetViewportExtEx(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    if (!d) RET_STDCALL(4, 0);
    write_point(ARG(3), d->s.vextx, d->s.vexty);
    if (d->s.mapmode >= 7) { d->s.vextx = (int)ARG(1); d->s.vexty = (int)ARG(2); }
    RET_STDCALL(4, 1);
}
void imp_GDI32_SetWindowExtEx(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    if (!d) RET_STDCALL(4, 0);
    write_point(ARG(3), d->s.wextx, d->s.wexty);
    if (d->s.mapmode >= 7) { d->s.wextx = (int)ARG(1); d->s.wexty = (int)ARG(2); }
    RET_STDCALL(4, 1);
}
void imp_GDI32_ScaleViewportExtEx(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    if (!d) RET_STDCALL(6, 0);
    write_point(ARG(5), d->s.vextx, d->s.vexty);
    if (ARG(2) && ARG(4)) {
        d->s.vextx = d->s.vextx * (int)ARG(1) / (int)ARG(2);
        d->s.vexty = d->s.vexty * (int)ARG(3) / (int)ARG(4);
    }
    RET_STDCALL(6, 1);
}
void imp_GDI32_ScaleWindowExtEx(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    if (!d) RET_STDCALL(6, 0);
    write_point(ARG(5), d->s.wextx, d->s.wexty);
    if (ARG(2) && ARG(4)) {
        d->s.wextx = d->s.wextx * (int)ARG(1) / (int)ARG(2);
        d->s.wexty = d->s.wexty * (int)ARG(3) / (int)ARG(4);
    }
    RET_STDCALL(6, 1);
}
void imp_GDI32_DPtoLP(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    uint32_t p = ARG(1);
    int n = (int)ARG(2);
    if (!d) RET_STDCALL(3, 0);
    for (int i = 0; i < n; i++, p += 8) {
        int x = (int)RD32(p), y = (int)RD32(p + 4);
        if (d->s.mapmode != 1 && d->s.vextx && d->s.vexty) {
            x = (x - d->s.vorgx) * d->s.wextx / d->s.vextx + d->s.worgx;
            y = (y - d->s.vorgy) * d->s.wexty / d->s.vexty + d->s.worgy;
        } else {
            x = x - d->s.vorgx + d->s.worgx;
            y = y - d->s.vorgy + d->s.worgy;
        }
        write_point(p, x, y);
    }
    RET_STDCALL(3, 1);
}

void imp_GDI32_GetDeviceCaps(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    Canvas c = {0};
    int ox, oy;
    Rect clip;
    int bpp = 8;
    if (d && !dc_target(d, &c, &ox, &oy, &clip)) bpp = c.bpp;
    if (d && (d->kind == DC_WINDOW || d->kind == DC_SCREEN)) {
        extern int ddraw_mode_bpp(void);
        bpp = ddraw_mode_bpp();
    }
    Canvas *dk = desktop_canvas();
    uint32_t v = 0;
    switch (ARG(1)) {
    case 2: v = 1; break;            /* TECHNOLOGY: DT_RASDISPLAY */
    case 8: v = (uint32_t)dk->w; break;
    case 10: v = (uint32_t)dk->h; break;
    case 12: v = (uint32_t)bpp; break;
    case 14: v = 1; break;
    case 24: v = bpp <= 8 ? 20 : 0xffffffffu; break; /* NUMCOLORS */
    case 38: v = 0x7e99 | (bpp <= 8 ? 0x100 : 0); break; /* RASTERCAPS (RC_PALETTE) */
    case 88: case 90: v = 96; break;  /* LOGPIXELSX/Y */
    case 104: v = bpp <= 8 ? 256 : 0; break; /* SIZEPALETTE */
    case 106: v = bpp <= 8 ? 20 : 0; break;  /* NUMRESERVED */
    case 108: v = 18; break;          /* COLORRES */
    case 116: v = 60; break;          /* VREFRESH */
    }
    RET_STDCALL(2, v);
}

void imp_GDI32_GetSystemPaletteEntries(CPU *cpu) {
    extern int ddraw_palette_rgb(uint32_t *out);
    uint32_t pal[256];
    int n = ddraw_palette_rgb(pal);
    uint32_t start = ARG(1), cnt = ARG(2), p = ARG(3);
    if (!p) RET_STDCALL(4, 256);
    for (uint32_t i = 0; i < cnt && start + i < 256; i++) {
        uint32_t c = n ? pal[start + i] : (start + i) * 0x010101u;
        WR8(p + 4 * i, (c >> 16) & 255);
        WR8(p + 4 * i + 1, (c >> 8) & 255);
        WR8(p + 4 * i + 2, c & 255);
        WR8(p + 4 * i + 3, 0);
    }
    RET_STDCALL(4, cnt);
}

void imp_GDI32_Escape(CPU *cpu) { RET_STDCALL(5, 0); }
void imp_GDI32_GdiFlush(CPU *cpu) { RET_STDCALL(0, 1); }

/* ---- regions --------------------------------------------------------------------------------- */
static uint32_t region_new(Rect *r, int n) {
    uint32_t h = obj_new(OBJ_REGION);
    GdiObj *o = obj_get(h, OBJ_REGION);
    o->u.rgn.r = malloc(sizeof(Rect) * (size_t)(n ? n : 1));
    if (n) memcpy(o->u.rgn.r, r, sizeof(Rect) * (size_t)n);
    o->u.rgn.n = n;
    return h;
}

void imp_GDI32_CreateRectRgn(CPU *cpu) {
    stock_init();
    Rect r = {(int)ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3)};
    RET_STDCALL(4, region_new(&r, rect_empty(r) ? 0 : 1));
}
void imp_GDI32_CreateRectRgnIndirect(CPU *cpu) {
    stock_init();
    uint32_t p = ARG(0);
    Rect r = {(int)RD32(p), (int)RD32(p + 4), (int)RD32(p + 8), (int)RD32(p + 12)};
    RET_STDCALL(1, region_new(&r, rect_empty(r) ? 0 : 1));
}

/* a - b as up to 4 rectangles */
static int rect_sub(Rect a, Rect b, Rect *out) {
    Rect i = rect_isect(a, b);
    if (rect_empty(i)) { out[0] = a; return 1; }
    int n = 0;
    if (i.t > a.t) out[n++] = (Rect){a.l, a.t, a.r, i.t};
    if (i.b < a.b) out[n++] = (Rect){a.l, i.b, a.r, a.b};
    if (i.l > a.l) out[n++] = (Rect){a.l, i.t, i.l, i.b};
    if (i.r < a.r) out[n++] = (Rect){i.r, i.t, a.r, i.b};
    return n;
}

/* CombineRgn(dst, a, b, mode) */
void imp_GDI32_CombineRgn(CPU *cpu) {
    GdiObj *d = obj_get(ARG(0), OBJ_REGION), *a = obj_get(ARG(1), OBJ_REGION), *b = obj_get(ARG(2), OBJ_REGION);
    int mode = (int)ARG(3);
    if (!d || !a) RET_STDCALL(4, 0);
    Rect tmp[512];
    int n = 0;
    switch (mode) {
    case 1: /* RGN_AND */
        for (int i = 0; b && i < a->u.rgn.n; i++)
            for (int j = 0; j < b->u.rgn.n && n < 512; j++) {
                Rect q = rect_isect(a->u.rgn.r[i], b->u.rgn.r[j]);
                if (!rect_empty(q)) tmp[n++] = q;
            }
        break;
    case 2: /* RGN_OR */
    case 3: /* RGN_XOR (approximated by OR) */
        for (int i = 0; i < a->u.rgn.n && n < 512; i++) tmp[n++] = a->u.rgn.r[i];
        for (int j = 0; b && j < b->u.rgn.n && n < 512; j++) tmp[n++] = b->u.rgn.r[j];
        break;
    case 4: { /* RGN_DIFF */
        for (int i = 0; i < a->u.rgn.n && n < 512; i++) tmp[n++] = a->u.rgn.r[i];
        for (int j = 0; b && j < b->u.rgn.n; j++) {
            Rect next[512];
            int m = 0;
            for (int i = 0; i < n; i++) {
                Rect parts[4];
                int k = rect_sub(tmp[i], b->u.rgn.r[j], parts);
                for (int q = 0; q < k && m < 512; q++) next[m++] = parts[q];
            }
            memcpy(tmp, next, sizeof(Rect) * (size_t)m);
            n = m;
        }
        break;
    }
    default: /* RGN_COPY */
        for (int i = 0; i < a->u.rgn.n && n < 512; i++) tmp[n++] = a->u.rgn.r[i];
    }
    free(d->u.rgn.r);
    d->u.rgn.r = malloc(sizeof(Rect) * (size_t)(n ? n : 1));
    memcpy(d->u.rgn.r, tmp, sizeof(Rect) * (size_t)n);
    d->u.rgn.n = n;
    RET_STDCALL(4, n == 0 ? 1 : n == 1 ? 2 : 3); /* NULLREGION / SIMPLEREGION / COMPLEXREGION */
}

void imp_GDI32_SelectClipRgn(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    if (!d) RET_STDCALL(2, 0);
    if (!ARG(1)) {
        free(d->s.uclip);
        d->s.uclip = NULL;
        d->s.nuclip = 0;
        d->s.has_uclip = 0;
        RET_STDCALL(2, 2);
    }
    RET_STDCALL(2, gdi_select(ARG(0), ARG(1)) ? 2 : 0);
}

void imp_GDI32_FillRgn(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    GdiObj *r = obj_get(ARG(1), OBJ_REGION);
    Target t;
    if (!d || !r || target_get(ARG(0), &t)) RET_STDCALL(3, 0);
    for (int i = 0; i < r->u.rgn.n; i++) {
        Rect q = r->u.rgn.r[i];
        t_fill_brush(&t, d, (Rect){lpx(d, q.l), lpy(d, q.t), lpx(d, q.r), lpy(d, q.b)}, ARG(2));
    }
    RET_STDCALL(3, 1);
}

void imp_GDI32_GetClipBox(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    Target t;
    uint32_t p = ARG(1);
    if (!d || target_get(ARG(0), &t)) RET_STDCALL(2, 0);
    Rect u = {1 << 30, 1 << 30, -(1 << 30), -(1 << 30)};
    for (int i = 0; i < t.nclip; i++) {
        Rect c = t.clip[i];
        if (c.l < u.l) u.l = c.l;
        if (c.t < u.t) u.t = c.t;
        if (c.r > u.r) u.r = c.r;
        if (c.b > u.b) u.b = c.b;
    }
    if (!t.nclip) u = (Rect){0, 0, 0, 0};
    else { u.l -= t.ox; u.r -= t.ox; u.t -= t.oy; u.b -= t.oy; }
    WR32(p, (uint32_t)u.l); WR32(p + 4, (uint32_t)u.t); WR32(p + 8, (uint32_t)u.r); WR32(p + 12, (uint32_t)u.b);
    RET_STDCALL(2, t.nclip == 0 ? 1 : t.nclip == 1 ? 2 : 3);
}

void imp_GDI32_PtVisible(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    Target t;
    if (!d || target_get(ARG(0), &t)) RET_STDCALL(3, 0);
    RET_STDCALL(3, clip_has(&t, lpx(d, (int)ARG(1)) + t.ox, lpy(d, (int)ARG(2)) + t.oy));
}

void imp_GDI32_RectVisible(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    Target t;
    uint32_t p = ARG(1);
    if (!d || target_get(ARG(0), &t)) RET_STDCALL(2, 0);
    Rect r = {lpx(d, (int)RD32(p)) + t.ox, lpy(d, (int)RD32(p + 4)) + t.oy, lpx(d, (int)RD32(p + 8)) + t.ox, lpy(d, (int)RD32(p + 12)) + t.oy};
    for (int i = 0; i < t.nclip; i++)
        if (!rect_empty(rect_isect(r, t.clip[i]))) RET_STDCALL(2, 1);
    RET_STDCALL(2, 0);
}

/* ---- drawing ------------------------------------------------------------------------------------ */
void imp_GDI32_MoveToEx(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    if (!d) RET_STDCALL(4, 0);
    write_point(ARG(3), d->s.curx, d->s.cury);
    d->s.curx = (int)ARG(1); d->s.cury = (int)ARG(2);
    RET_STDCALL(4, 1);
}

void imp_GDI32_LineTo(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    Target t;
    if (!d || target_get(ARG(0), &t)) RET_STDCALL(3, 0);
    int x = (int)ARG(1), y = (int)ARG(2);
    t_line(&t, d, lpx(d, d->s.curx), lpy(d, d->s.cury), lpx(d, x), lpy(d, y));
    d->s.curx = x; d->s.cury = y;
    RET_STDCALL(3, 1);
}

static void rect_outline(Target *t, DC *d, Rect r) {
    t_line(t, d, r.l, r.t, r.r - 1, r.t);
    t_line(t, d, r.r - 1, r.t, r.r - 1, r.b - 1);
    t_line(t, d, r.r - 1, r.b - 1, r.l, r.b - 1);
    t_line(t, d, r.l, r.b - 1, r.l, r.t);
}

void imp_GDI32_Rectangle(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    Target t;
    if (!d || target_get(ARG(0), &t)) RET_STDCALL(5, 0);
    Rect r = {lpx(d, (int)ARG(1)), lpy(d, (int)ARG(2)), lpx(d, (int)ARG(3)), lpy(d, (int)ARG(4))};
    GdiObj *p = obj_get(d->s.pen, OBJ_PEN);
    int inset = p && p->u.pen.style != 5 ? 1 : 0;
    t_fill_brush(&t, d, (Rect){r.l + inset, r.t + inset, r.r - inset, r.b - inset}, d->s.brush);
    if (inset) rect_outline(&t, d, r);
    RET_STDCALL(5, 1);
}

void imp_GDI32_Ellipse(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    Target t;
    if (!d || target_get(ARG(0), &t)) RET_STDCALL(5, 0);
    int l = lpx(d, (int)ARG(1)), tp = lpy(d, (int)ARG(2)), r = lpx(d, (int)ARG(3)), b = lpy(d, (int)ARG(4));
    double cx = (l + r - 1) / 2.0, cy = (tp + b - 1) / 2.0, rx = (r - l) / 2.0, ry = (b - tp) / 2.0;
    GdiObj *br = obj_get(d->s.brush, OBJ_BRUSH);
    GdiObj *pn = obj_get(d->s.pen, OBJ_PEN);
    uint32_t pv = pn && pn->u.pen.style != 5 ? canvas_pixel_for(&t.c, pn->u.pen.color) : 0;
    for (int y = tp; y < b; y++)
        for (int x = l; x < r; x++) {
            double nx = (x - cx) / rx, ny = (y - cy) / ry, q = nx * nx + ny * ny;
            if (q > 1.0) continue;
            double inner = rx > 1 && ry > 1 ? ((x - cx) / (rx - 1)) * ((x - cx) / (rx - 1)) + ((y - cy) / (ry - 1)) * ((y - cy) / (ry - 1)) : 2;
            if (pn && pn->u.pen.style != 5 && inner > 1.0) t_pixel(&t, x, y, pv, d->s.rop2);
            else if (br && br->u.brush.style != 1) t_fill_brush(&t, d, (Rect){x, y, x + 1, y + 1}, d->s.brush);
        }
    RET_STDCALL(5, 1);
}

void imp_GDI32_PatBlt(CPU *cpu) {
    RET_STDCALL(6, do_blit(ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), 0, 0, 0, 0, 0, ARG(5)));
}

void imp_GDI32_BitBlt(CPU *cpu) {
    RET_STDCALL(9, do_blit(ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), ARG(5), (int)ARG(6), (int)ARG(7),
                           (int)ARG(3), (int)ARG(4), ARG(8)));
}

void imp_GDI32_StretchBlt(CPU *cpu) {
    RET_STDCALL(11, do_blit(ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), ARG(5), (int)ARG(6), (int)ARG(7),
                            (int)ARG(8), (int)ARG(9), ARG(10)));
}

void imp_GDI32_GetPixel(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    Target t;
    if (!d || target_get(ARG(0), &t)) RET_STDCALL(3, 0xffffffffu);
    int x = lpx(d, (int)ARG(1)) + t.ox, y = lpy(d, (int)ARG(2)) + t.oy;
    if (!clip_has(&t, x, y)) RET_STDCALL(3, 0xffffffffu);
    RET_STDCALL(3, CR_OF(canvas_get(&t.c, x, y)));
}

void imp_GDI32_SetPixel(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    Target t;
    if (!d || target_get(ARG(0), &t)) RET_STDCALL(4, 0xffffffffu);
    t_pixel(&t, lpx(d, (int)ARG(1)), lpy(d, (int)ARG(2)), canvas_pixel_for(&t.c, RGB_OF(ARG(3))), 13);
    RET_STDCALL(4, ARG(3));
}

/* SetDIBitsToDevice(hdc, x, y, w, h, sx, sy, start, lines, bits, bmi, usage) */
void imp_GDI32_SetDIBitsToDevice(CPU *cpu) {
    uint32_t hdc = ARG(0), bits = ARG(9), bmi = ARG(10);
    stock_init();
    uint32_t hb = bitmap_from_dib(bmi, bits, 0);
    uint32_t mdc = gdi_dc_new(DC_MEMORY, 0, 0);
    gdi_select(mdc, hb);
    do_blit(hdc, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), mdc, (int)ARG(5), (int)ARG(6), (int)ARG(3), (int)ARG(4), SRCCOPY);
    gdi_dc_free(mdc);
    pthread_mutex_lock(&g_gdi_lock);
    obj_free(hb);
    pthread_mutex_unlock(&g_gdi_lock);
    RET_STDCALL(12, ARG(8));
}

/* ---- text APIs ----------------------------------------------------------------------------------- */
static void text_out(uint32_t hdc, int x, int y, const char *s, int n, uint32_t opts, uint32_t prect) {
    DC *d = dc_get(hdc);
    Target t;
    if (!d || target_get(hdc, &t)) return;
    RFont *rf = dc_rfont(d);
    if (d->s.textalign & 1) { x = d->s.curx; y = d->s.cury; }
    int w = text_width_rf(rf, s, n), h = rf->ascent + rf->descent;
    int dx = lpx(d, x), dy = lpy(d, y);
    if ((d->s.textalign & 6) == 6) dx -= w / 2;
    else if (d->s.textalign & 2) dx -= w;
    if ((d->s.textalign & 24) == 24) dy -= rf->ascent;
    else if (d->s.textalign & 8) dy -= h;
    if (prect) {
        Rect r = {lpx(d, (int)RD32(prect)), lpy(d, (int)RD32(prect + 4)), lpx(d, (int)RD32(prect + 8)), lpy(d, (int)RD32(prect + 12))};
        if (opts & 2) t_fill_rgb(&t, r, RGB_OF(d->s.bkcolor));      /* ETO_OPAQUE */
        if (opts & 4)                                                 /* ETO_CLIPPED */
            for (int k = 0; k < t.nclip; k++) t.clip[k] = rect_isect(t.clip[k], (Rect){r.l + t.ox, r.t + t.oy, r.r + t.ox, r.b + t.oy});
    }
    if (d->s.bkmode == 2 && !(prect && (opts & 2))) t_fill_rgb(&t, (Rect){dx, dy, dx + w, dy + h}, RGB_OF(d->s.bkcolor));
    t_text(&t, d, dx, dy, s, n, -1);
    if (d->s.textalign & 1) d->s.curx += w;
}

void imp_GDI32_TextOutA(CPU *cpu) {
    text_out(ARG(0), (int)ARG(1), (int)ARG(2), gstr(ARG(3)), (int)ARG(4), 0, 0);
    RET_STDCALL(5, 1);
}

/* ExtTextOutA(hdc, x, y, options, rect, string, count, dx) */
void imp_GDI32_ExtTextOutA(CPU *cpu) {
    uint32_t opts = ARG(3), rect = ARG(4), str = ARG(5);
    text_out(ARG(0), (int)ARG(1), (int)ARG(2), str ? gstr(str) : "", str ? (int)ARG(6) : 0, opts,
             (opts & 6) ? rect : 0);
    RET_STDCALL(8, 1);
}

void imp_GDI32_GetTextExtentPoint32A(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    RFont *rf = d ? dc_rfont(d) : rfont_get(NULL);
    int n = (int)ARG(2);
    write_point(ARG(3), text_width_rf(rf, gstr(ARG(1)), n), rf->ascent + rf->descent);
    RET_STDCALL(4, 1);
}

void imp_GDI32_GetTextMetricsA(CPU *cpu) {
    DC *d = dc_get(ARG(0));
    RFont *rf = d ? dc_rfont(d) : rfont_get(NULL);
    GdiObj *f = d ? obj_get(d->s.font, OBJ_FONT) : NULL;
    uint8_t b[56] = {0};
    int32_t *w = (int32_t *)b;
    w[0] = rf->ascent + rf->descent;
    w[1] = rf->ascent;
    w[2] = rf->descent;
    w[3] = (rf->ascent + rf->descent) - (int)(rf->scale * 2048 + 0.5f) > 0 ? (rf->ascent + rf->descent) - (int)(rf->scale * 2048 + 0.5f) : 0;
    w[4] = rf->linegap;
    w[5] = rf->avgw;
    w[6] = rf->maxw;
    w[7] = f ? (f->u.font.weight ? f->u.font.weight : 400) : 400;
    w[9] = 96;
    w[10] = 96;
    b[44] = 32; b[45] = 255; b[46] = 31; b[47] = 32;
    b[48] = f ? f->u.font.italic : 0;
    b[49] = f ? f->u.font.underline : 0;
    b[51] = 0x26; /* VARIABLE_PITCH | FF_SWISS, TMPF_TRUETYPE */
    memcpy(GPTR(ARG(1)), b, 56);
    RET_STDCALL(2, 1);
}

/* polygon regions: kept as their bounding box plus the points for hit tests */
void imp_GDI32_CreatePolygonRgn(CPU *cpu) {
    stock_init();
    uint32_t pts = ARG(0);
    int n = (int)ARG(1);
    Rect b = {1 << 30, 1 << 30, -(1 << 30), -(1 << 30)};
    for (int i = 0; i < n; i++) {
        int x = (int)RD32(pts + 8 * i), y = (int)RD32(pts + 8 * i + 4);
        if (x < b.l) b.l = x;
        if (y < b.t) b.t = y;
        if (x > b.r) b.r = x;
        if (y > b.b) b.b = y;
    }
    RET_STDCALL(3, region_new(&b, n ? 1 : 0));
}
void imp_GDI32_PtInRegion(CPU *cpu) {
    GdiObj *r = obj_get(ARG(0), OBJ_REGION);
    int x = (int)ARG(1), y = (int)ARG(2);
    if (r)
        for (int i = 0; i < r->u.rgn.n; i++)
            if (x >= r->u.rgn.r[i].l && x < r->u.rgn.r[i].r && y >= r->u.rgn.r[i].t && y < r->u.rgn.r[i].b) RET_STDCALL(3, 1);
    RET_STDCALL(3, 0);
}
void imp_GDI32_RectInRegion(CPU *cpu) {
    GdiObj *r = obj_get(ARG(0), OBJ_REGION);
    uint32_t p = ARG(1);
    Rect q = {(int)RD32(p), (int)RD32(p + 4), (int)RD32(p + 8), (int)RD32(p + 12)};
    if (r)
        for (int i = 0; i < r->u.rgn.n; i++)
            if (!rect_empty(rect_isect(q, r->u.rgn.r[i]))) RET_STDCALL(2, 1);
    RET_STDCALL(2, 0);
}

/* a .bmp file from the game folder as a bitmap (LoadImage LR_LOADFROMFILE) */
#include "../pack/vfs.h"
uint32_t gdi_load_bitmap_file(const char *guest_path) {
    char rel[1024];
    guest_rel_path(guest_path, rel, sizeof rel);
    VFile *f = vfs_open(rel);
    if (!f) return 0;
    int64_t n = vfs_size(f);
    if (n < 54) { vfs_close(f); return 0; }
    uint32_t buf = heap_alloc((uint32_t)n, 0);
    vfs_read(f, GPTR(buf), (size_t)n);
    vfs_close(f);
    if (RD16(buf) != 0x4d42) { heap_free(buf); return 0; }
    stock_init();
    uint32_t hb = bitmap_from_dib(buf + 14, buf + RD32(buf + 10), 0);
    obj_get(hb, OBJ_BITMAP)->u.bmp.alloc = buf;
    return hb;
}

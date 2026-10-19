/* DirectDraw emulation for software renderers: 8 bpp paletted, 16 or 32 bpp
 * surfaces in guest memory, flip chains, palettes, clippers, colour-keyed and
 * stretched blits, and surface DCs for GDI drawing.  Flip (or a blit to a
 * chain-less primary) hands the frame to the window compositor (user32.c), which
 * puts any visible GDI windows on top and presents.  Direct3D is not offered, so
 * games keep their software path. */
#include <stdlib.h>
#include <string.h>
#include "win32.h"
#include "gdi.h"

#define DD_OK 0
#define DDERR_GENERIC E_FAIL
#define DDERR_INVALIDPARAMS E_INVALIDARG
#define DDERR_NOTFOUND 0x887600ffu
#define DDERR_NOCOLORKEY 0x887600d7u
#define DDERR_NOPALETTEATTACHED 0x887600e8u
#define DDERR_NOCLIPPERATTACHED 0x887600cdu

#define DDSD_CAPS 0x1
#define DDSD_HEIGHT 0x2
#define DDSD_WIDTH 0x4
#define DDSD_PITCH 0x8
#define DDSD_BACKBUFFERCOUNT 0x20
#define DDSD_CKSRCBLT 0x10000
#define DDSD_CKDESTBLT 0x4000
#define DDSD_LPSURFACE 0x800
#define DDSD_PIXELFORMAT 0x1000
#define DDSCAPS_BACKBUFFER 0x4
#define DDSCAPS_COMPLEX 0x8
#define DDSCAPS_FLIP 0x10
#define DDSCAPS_FRONTBUFFER 0x20
#define DDSCAPS_OFFSCREENPLAIN 0x40
#define DDSCAPS_PALETTE 0x100
#define DDSCAPS_PRIMARYSURFACE 0x200
#define DDSCAPS_SYSTEMMEMORY 0x800
#define DDSCAPS_VIDEOMEMORY 0x4000
#define DDSCAPS_VISIBLE 0x8000
#define DDSCAPS_LOCALVIDMEM 0x10000000
#define DDBLT_COLORFILL 0x400
#define DDBLT_DDFX 0x800
#define DDBLT_KEYDEST 0x2000
#define DDBLT_KEYSRC 0x8000
#define DDBLT_KEYSRCOVERRIDE 0x10000

#define MAX_SURF 16384
#define MAX_PAL 32

typedef struct {
    int used;
    uint32_t obj;
    uint32_t pal_entries_ptr;
    uint32_t rgb[256];  /* 0x00RRGGBB */
    int refs;
} Pal;

typedef struct {
    int used;
    uint32_t obj;
    int w, h, pitch, bpp;
    uint32_t mem;
    uint32_t caps;
    int chain;          /* flip chain id (-1 = standalone) */
    int chain_pos;      /* 0 = front, 1.. = back buffers */
    int refs;
    int has_srckey, has_dstkey;
    uint32_t srckey, dstkey;
    int pal;            /* attached palette index or -1 */
    uint32_t clipper;
    uint32_t dc;
    int own_mem;
} Surf;

typedef struct {
    int count;
    uint32_t mem[4];    /* buffer memory, rotated on Flip */
    int surf[4];        /* surfaces in the chain, by position */
} Chain;

static Surf g_surf[MAX_SURF];
static Pal g_pal[MAX_PAL];
static Chain g_chains[8];
static int g_chain_count;
static int g_primary = -1;
static uint32_t g_vt_dd, g_vt_dd2, g_vt_surf, g_vt_surf3, g_vt_pal, g_vt_clip, g_dd_obj, g_dd2_obj;
static int g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 8;
static uint32_t g_coop_hwnd, g_coop_flags;
static uint32_t g_last_flip_ms;

void wm_present(void);

uint32_t ddraw_coop_hwnd(void) { return (g_coop_flags & 0x10) ? g_coop_hwnd : 0; } /* DDSCL_FULLSCREEN */
int ddraw_mode_bpp(void) { return g_mode_bpp; }

static Surf *surf_from_obj(uint32_t obj) {
    uint32_t i = RD32(obj + 4);
    if (i >= MAX_SURF || !g_surf[i].used) rt_fatal("ddraw: bad surface object %08x", obj);
    return &g_surf[i];
}

static int surf_index_of(uint32_t obj) {
    if (!obj) return -1;
    uint32_t i = RD32(obj + 4);
    return i < MAX_SURF && g_surf[i].used && g_surf[i].obj == obj ? (int)i : -1;
}

static uint32_t surf_mem(const Surf *s) {
    if (s->chain >= 0) return g_chains[s->chain].mem[s->chain_pos];
    return s->mem;
}

static const uint32_t *surf_palette(const Surf *s) {
    if (s->pal >= 0) return g_pal[s->pal].rgb;
    if (g_primary >= 0 && g_surf[g_primary].pal >= 0) return g_pal[g_surf[g_primary].pal].rgb;
    return NULL;
}

int ddraw_palette_rgb(uint32_t *out) {
    if (g_primary < 0 || g_surf[g_primary].pal < 0) return 0;
    memcpy(out, g_pal[g_surf[g_primary].pal].rgb, 1024);
    return 256;
}

/* surface DC support for gdi32.c */
int ddraw_surface_canvas(uint32_t obj, Canvas *c) {
    int i = surf_index_of(obj);
    if (i < 0) return -1;
    Surf *s = &g_surf[i];
    c->bits = (uint8_t *)GPTR(surf_mem(s));
    c->w = s->w;
    c->h = s->h;
    c->pitch = s->pitch;
    c->bpp = s->bpp;
    c->rgb565 = 1;
    c->pal = s->bpp == 8 ? surf_palette(s) : NULL;
    c->pal_n = c->pal ? 256 : 0;
    return 0;
}

static int surf_new(int w, int h, int bpp, uint32_t caps, uint32_t mem) {
    for (int i = 0; i < MAX_SURF; i++)
        if (!g_surf[i].used) {
            Surf *s = &g_surf[i];
            memset(s, 0, sizeof *s);
            s->used = 1;
            s->w = w;
            s->h = h;
            s->bpp = bpp;
            s->pitch = ((w * bpp / 8) + 3) & ~3;
            s->caps = caps;
            s->own_mem = mem == 0;
            s->mem = mem ? mem : guest_alloc((uint32_t)(s->pitch * h) + 64);
            s->chain = -1;
            s->pal = -1;
            s->refs = 1;
            s->obj = guest_alloc(16);
            WR32(s->obj, g_vt_surf);
            WR32(s->obj + 4, (uint32_t)i);
            return i;
        }
    rt_fatal("ddraw: out of surfaces");
}

static void write_pixel_format(uint32_t p, int bpp) {
    memset(GPTR(p), 0, 32);
    WR32(p + 0, 32);
    if (bpp == 8) {
        WR32(p + 4, 0x60);  /* DDPF_RGB | DDPF_PALETTEINDEXED8 */
        WR32(p + 12, 8);
    } else if (bpp == 16) {
        WR32(p + 4, 0x40);  /* DDPF_RGB */
        WR32(p + 12, 16);
        WR32(p + 16, 0xf800);
        WR32(p + 20, 0x07e0);
        WR32(p + 24, 0x001f);
    } else {
        WR32(p + 4, 0x40);
        WR32(p + 12, (uint32_t)bpp);
        WR32(p + 16, 0xff0000);
        WR32(p + 20, 0x00ff00);
        WR32(p + 24, 0x0000ff);
    }
}

static void write_desc(uint32_t d, const Surf *s) {
    memset(GPTR(d + 4), 0, 0x6c - 4);
    WR32(d + 0, 0x6c);
    WR32(d + 4, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_LPSURFACE |
                    (s->has_srckey ? DDSD_CKSRCBLT : 0));
    WR32(d + 8, (uint32_t)s->h);
    WR32(d + 12, (uint32_t)s->w);
    WR32(d + 16, (uint32_t)s->pitch);
    if (s->chain >= 0 && s->chain_pos == 0) WR32(d + 20, (uint32_t)(g_chains[s->chain].count - 1));
    WR32(d + 36, surf_mem(s));
    WR32(d + 64, s->srckey);
    WR32(d + 68, s->srckey);
    write_pixel_format(d + 72, s->bpp);
    WR32(d + 104, s->caps);
}

/* ---- presentation --------------------------------------------------------------- */
/* front buffer -> 32 bpp (for the compositor); returns 1 when there is a primary */
int ddraw_render_frame(uint32_t *dst, int w, int h) {
    if (g_primary < 0 || !g_coop_hwnd) return 0;
    Surf *s = &g_surf[g_primary];
    uint32_t mem = surf_mem(s);
    int cw = s->w < w ? s->w : w, ch = s->h < h ? s->h : h;
    if (s->bpp == 8) {
        const uint32_t *pal = surf_palette(s);
        uint32_t lut[256];
        for (int i = 0; i < 256; i++) lut[i] = 0xff000000u | (pal ? pal[i] : (uint32_t)i * 0x010101u);
        for (int y = 0; y < ch; y++) {
            const uint8_t *row = (const uint8_t *)GPTR(mem + (uint32_t)(y * s->pitch));
            uint32_t *o = dst + y * w;
            for (int x = 0; x < cw; x++) o[x] = lut[row[x]];
        }
    } else if (s->bpp == 16) {
        static uint32_t *lut565;
        if (!lut565) {
            lut565 = malloc(65536 * 4);
            for (uint32_t p = 0; p < 65536; p++) {
                uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                lut565[p] = 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            }
        }
        for (int y = 0; y < ch; y++) {
            const uint16_t *row = (const uint16_t *)GPTR(mem + (uint32_t)(y * s->pitch));
            uint32_t *o = dst + y * w;
            for (int x = 0; x < cw; x++) o[x] = lut565[row[x]];
        }
    } else {
        for (int y = 0; y < ch; y++) {
            const uint8_t *row = (const uint8_t *)GPTR(mem + (uint32_t)(y * s->pitch));
            uint32_t *o = dst + y * w;
            for (int x = 0; x < cw; x++)
                o[x] = 0xff000000u | (s->bpp == 32 ? ((const uint32_t *)row)[x] : (uint32_t)(row[3 * x] | row[3 * x + 1] << 8 | row[3 * x + 2] << 16));
        }
    }
    return 1;
}

static void present_primary(void) {
    g_last_flip_ms = platform_ticks_ms();
    wm_present();
}

/* Flip waits for the vertical blank on real hardware: pace flips at 60 Hz */
static void wait_vblank(void) {
    static uint64_t next;
    uint64_t now = platform_ticks_us();
    if (next > now && next - now < 20000) platform_sleep_ms((uint32_t)((next - now + 999) / 1000));
    now = platform_ticks_us();
    next = (now > next + 16667 ? now : next) + 16667;
}

/* ---- IDirectDrawPalette ----------------------------------------------------------- */
static Pal *pal_from_obj(uint32_t obj) {
    uint32_t i = RD32(obj + 4);
    if (i >= MAX_PAL || !g_pal[i].used) rt_fatal("ddraw: bad palette %08x", obj);
    return &g_pal[i];
}
static void P_QueryInterface(CPU *cpu) { WR32(ARG(2), 0); RET_STDCALL(3, E_NOINTERFACE); }
static void P_AddRef(CPU *cpu) { RET_STDCALL(1, ++pal_from_obj(ARG(0))->refs); }
static void P_Release(CPU *cpu) {
    Pal *p = pal_from_obj(ARG(0));
    int r = --p->refs;
    if (r <= 0) {
        int idx = (int)(p - g_pal);
        for (int i = 0; i < MAX_SURF; i++)
            if (g_surf[i].used && g_surf[i].pal == idx) g_surf[i].pal = -1;
        heap_free(p->obj);
        p->used = 0;
    }
    RET_STDCALL(1, r < 0 ? 0 : (uint32_t)r);
}
static void P_GetCaps(CPU *cpu) { WR32(ARG(1), 0x4 | 0x40); RET_STDCALL(2, DD_OK); } /* DDPCAPS_8BIT|ALLOW256 */
static void P_GetEntries(CPU *cpu) {
    Pal *p = pal_from_obj(ARG(0));
    uint32_t start = ARG(2), n = ARG(3), out = ARG(4);
    for (uint32_t i = 0; i < n && start + i < 256; i++) {
        uint32_t c = p->rgb[start + i];
        WR8(out + 4 * i, (c >> 16) & 255);
        WR8(out + 4 * i + 1, (c >> 8) & 255);
        WR8(out + 4 * i + 2, c & 255);
        WR8(out + 4 * i + 3, 0);
    }
    RET_STDCALL(5, DD_OK);
}
static void P_Initialize(CPU *cpu) { RET_STDCALL(4, DD_OK); }
static void P_SetEntries(CPU *cpu) {
    Pal *p = pal_from_obj(ARG(0));
    uint32_t start = ARG(2), n = ARG(3), in = ARG(4);
    for (uint32_t i = 0; i < n && start + i < 256; i++)
        p->rgb[start + i] = (uint32_t)RD8(in + 4 * i) << 16 | (uint32_t)RD8(in + 4 * i + 1) << 8 | RD8(in + 4 * i + 2);
    /* a palette change on the primary is visible at once (fades on a still frame) */
    if (g_primary >= 0 && g_surf[g_primary].pal == (int)(p - g_pal) && platform_ticks_ms() - g_last_flip_ms > 30)
        present_primary();
    RET_STDCALL(5, DD_OK);
}

/* ---- IDirectDrawClipper ------------------------------------------------------------- */
static void C_QueryInterface(CPU *cpu) { WR32(ARG(2), 0); RET_STDCALL(3, E_NOINTERFACE); }
static void C_AddRef(CPU *cpu) { RET_STDCALL(1, 2); }
static void C_Release(CPU *cpu) { RET_STDCALL(1, 0); }
static void C_GetClipList(CPU *cpu) { RET_STDCALL(4, DDERR_GENERIC); }
static void C_GetHWnd(CPU *cpu) { WR32(ARG(1), RD32(ARG(0) + 4)); RET_STDCALL(2, DD_OK); }
static void C_Initialize(CPU *cpu) { RET_STDCALL(3, DD_OK); }
static void C_IsClipListChanged(CPU *cpu) { WR32(ARG(1), 0); RET_STDCALL(2, DD_OK); }
static void C_SetClipList(CPU *cpu) { RET_STDCALL(3, DD_OK); }
static void C_SetHWnd(CPU *cpu) { WR32(ARG(0) + 4, ARG(2)); RET_STDCALL(3, DD_OK); }

/* ---- IDirectDraw -------------------------------------------------------------------- */
static void ddraw_init(void);
static const uint8_t IID_IDirectDraw2[16] = {0xe0, 0xf3, 0xa6, 0xb3, 0x43, 0x2b, 0xcf, 0x11, 0xa2, 0xde, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}; /* B3A6F3E0-2B43-11CF-A2DE-00AA00B93356 */
static const uint8_t IID_IDirectDraw[16] = {0x80, 0xdb, 0x14, 0x6c, 0x33, 0xa7, 0xce, 0x11, 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}; /* 6C14DB80-A733-11CE-A521-0020AF0BE560 */

static void DD_QueryInterface(CPU *cpu) {
    const uint8_t *iid = GPTR(ARG(1));
    if (!memcmp(iid, IID_IDirectDraw2, 16)) { WR32(ARG(2), g_dd2_obj); RET_STDCALL(3, DD_OK); }
    if (!memcmp(iid, IID_IDirectDraw, 16)) { WR32(ARG(2), g_dd_obj); RET_STDCALL(3, DD_OK); }
    WR32(ARG(2), 0);
    RET_STDCALL(3, E_NOINTERFACE);
}
static void DD_AddRef(CPU *cpu) { RET_STDCALL(1, 2); }
static void DD_Release(CPU *cpu) { RET_STDCALL(1, 0); }
static void DD_Compact(CPU *cpu) { RET_STDCALL(1, DD_OK); }

static void DD_CreateClipper(CPU *cpu) {
    uint32_t c = guest_alloc(16);
    WR32(c, g_vt_clip);
    WR32(ARG(2), c);
    RET_STDCALL(4, DD_OK);
}

static void DD_CreatePalette(CPU *cpu) {
    uint32_t entries = ARG(2), pp = ARG(3);
    for (int i = 0; i < MAX_PAL; i++)
        if (!g_pal[i].used) {
            Pal *p = &g_pal[i];
            memset(p, 0, sizeof *p);
            p->used = 1;
            p->refs = 1;
            p->obj = guest_alloc(16);
            WR32(p->obj, g_vt_pal);
            WR32(p->obj + 4, (uint32_t)i);
            if (entries)
                for (int k = 0; k < 256; k++)
                    p->rgb[k] = (uint32_t)RD8(entries + 4 * k) << 16 | (uint32_t)RD8(entries + 4 * k + 1) << 8 | RD8(entries + 4 * k + 2);
            WR32(pp, p->obj);
            RET_STDCALL(5, DD_OK);
        }
    RET_STDCALL(5, DDERR_GENERIC);
}

static void DD_CreateSurface(CPU *cpu) {
    uint32_t d = ARG(1), pp = ARG(2);
    uint32_t flags = RD32(d + 4), caps = (flags & DDSD_CAPS) ? RD32(d + 104) : 0;
    int bpp = g_mode_bpp;
    if (flags & DDSD_PIXELFORMAT) {
        uint32_t pf = d + 72;
        bpp = (RD32(pf + 4) & 0x20) ? 8 : (int)RD32(pf + 12);
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        int backs = (flags & DDSD_BACKBUFFERCOUNT) ? (int)RD32(d + 20) : 0;
        if (backs > 3) backs = 3;
        int ci = g_chain_count++ & 7;
        Chain *c = &g_chains[ci];
        c->count = backs + 1;
        for (int k = 0; k < c->count; k++) {
            int si = surf_new(g_mode_w, g_mode_h, g_mode_bpp,
                              k == 0 ? (caps | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE) : (caps & ~DDSCAPS_PRIMARYSURFACE) | DDSCAPS_BACKBUFFER | DDSCAPS_FLIP, 0);
            g_surf[si].chain = ci;
            g_surf[si].chain_pos = k;
            c->mem[k] = g_surf[si].mem;
            c->surf[k] = si;
        }
        g_primary = c->surf[0];
        rt_log("ddraw: primary %dx%dx%d with %d back buffer(s)", g_mode_w, g_mode_h, g_mode_bpp, backs);
        WR32(pp, g_surf[c->surf[0]].obj);
        RET_STDCALL(4, DD_OK);
    }
    int w = (flags & DDSD_WIDTH) ? (int)RD32(d + 12) : g_mode_w;
    int h = (flags & DDSD_HEIGHT) ? (int)RD32(d + 8) : g_mode_h;
    uint32_t mem = (flags & DDSD_LPSURFACE) ? RD32(d + 36) : 0;
    int si = surf_new(w, h, bpp, caps | ((caps & DDSCAPS_SYSTEMMEMORY) ? 0 : DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM), mem);
    if ((flags & DDSD_PITCH) && mem) g_surf[si].pitch = (int)RD32(d + 16);
    if (flags & DDSD_CKSRCBLT) { g_surf[si].has_srckey = 1; g_surf[si].srckey = RD32(d + 64); }
    WR32(pp, g_surf[si].obj);
    RET_STDCALL(4, DD_OK);
}

static void DD_DuplicateSurface(CPU *cpu) { RET_STDCALL(3, DDERR_GENERIC); }

static const int g_modes[][2] = {{320, 240}, {640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 960}, {1280, 1024}, {1920, 1080}};

static void enum_modes(CPU *cpu, uint32_t ctx, uint32_t cb, int nargs_ret) {
    uint32_t desc = guest_alloc(0x6c);
    static const int bpps[] = {8, 16, 32};
    for (int b = 0; b < 3; b++)
        for (unsigned i = 0; i < sizeof g_modes / sizeof g_modes[0]; i++) {
            Surf tmp = {0};
            tmp.w = g_modes[i][0];
            tmp.h = g_modes[i][1];
            tmp.bpp = bpps[b];
            tmp.pitch = tmp.w * bpps[b] / 8;
            tmp.chain = -1;
            write_desc(desc, &tmp);
            WR32(desc + 4, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | 0x40000 /* DDSD_REFRESHRATE */);
            WR32(desc + 24, 60);
            uint32_t args[2] = {desc, ctx};
            if (guest_call(cpu, cb, 2, args) == 0) goto done; /* DDENUMRET_CANCEL */
        }
done:
    heap_free(desc);
    (void)nargs_ret;
}

static void DD_EnumDisplayModes(CPU *cpu) {
    enum_modes(cpu, ARG(3), ARG(4), 5);
    RET_STDCALL(5, DD_OK);
}

static void DD_EnumSurfaces(CPU *cpu) { RET_STDCALL(5, DD_OK); }
static void DD_FlipToGDISurface(CPU *cpu) { RET_STDCALL(1, DD_OK); }

static void DD_GetCaps(CPU *cpu) {
    for (int k = 1; k <= 2; k++) {
        uint32_t c = ARG(k);
        if (!c) continue;
        uint32_t sz = RD32(c);
        memset(GPTR(c + 4), 0, sz > 4 ? sz - 4 : 0);
        WR32(c + 4, 0x40 | 0x200 | 0x4000000); /* DDCAPS_BLT | DDCAPS_COLORKEY | DDCAPS_BLTSTRETCH */
        WR32(c + 8, 0);
        WR32(c + 12, 0x200);                   /* DDCKEYCAPS_SRCBLT */
        WR32(c + 60, 64u << 20);               /* dwVidMemTotal */
        WR32(c + 64, 64u << 20);               /* dwVidMemFree */
        if (sz >= 108) WR32(c + 104, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PRIMARYSURFACE | DDSCAPS_PALETTE | DDSCAPS_VIDEOMEMORY);
    }
    RET_STDCALL(3, DD_OK);
}

static void DD_GetDisplayMode(CPU *cpu) {
    Surf tmp = {0};
    tmp.w = g_mode_w;
    tmp.h = g_mode_h;
    tmp.bpp = g_mode_bpp;
    tmp.pitch = g_mode_w * g_mode_bpp / 8;
    tmp.chain = -1;
    write_desc(ARG(1), &tmp);
    RET_STDCALL(2, DD_OK);
}

static void DD_GetFourCCCodes(CPU *cpu) { if (ARG(1)) WR32(ARG(1), 0); RET_STDCALL(3, DD_OK); }
static void DD_GetGDISurface(CPU *cpu) {
    if (g_primary < 0) RET_STDCALL(2, DDERR_NOTFOUND);
    g_surf[g_primary].refs++;
    WR32(ARG(1), g_surf[g_primary].obj);
    RET_STDCALL(2, DD_OK);
}
static void DD_GetMonitorFrequency(CPU *cpu) { WR32(ARG(1), 60); RET_STDCALL(2, DD_OK); }
static void DD_GetScanLine(CPU *cpu) { WR32(ARG(1), 0); RET_STDCALL(2, DD_OK); }
static void DD_GetVerticalBlankStatus(CPU *cpu) { WR32(ARG(1), 1); RET_STDCALL(2, DD_OK); }
static void DD_Initialize(CPU *cpu) { RET_STDCALL(2, DD_OK); }
static void DD_RestoreDisplayMode(CPU *cpu) { RET_STDCALL(1, DD_OK); }
static void DD_SetCooperativeLevel(CPU *cpu) {
    g_coop_hwnd = ARG(1);
    g_coop_flags = ARG(2);
    rt_log("ddraw: SetCooperativeLevel(%x, %x)", ARG(1), ARG(2));
    RET_STDCALL(3, DD_OK);
}

static void set_mode(int w, int h, int bpp) {
    g_mode_w = w;
    g_mode_h = h;
    g_mode_bpp = bpp;
    desktop_resize(w, h);
    rt_log("ddraw: SetDisplayMode(%d, %d, %d)", w, h, bpp);
}
static void DD_SetDisplayMode(CPU *cpu) {
    set_mode((int)ARG(1), (int)ARG(2), (int)ARG(3));
    RET_STDCALL(4, DD_OK);
}
/* IDirectDraw2::SetDisplayMode has refresh rate and flags too */
static void DD2_SetDisplayMode(CPU *cpu) {
    set_mode((int)ARG(1), (int)ARG(2), (int)ARG(3));
    RET_STDCALL(6, DD_OK);
}

static void DD_WaitForVerticalBlank(CPU *cpu) {
    /* paces games that sync to vblank: about 60 Hz */
    static uint64_t next;
    uint64_t now = platform_ticks_us();
    if (next > now && next - now < 17000) platform_sleep_ms((uint32_t)((next - now) / 1000));
    next = (now > next ? now : next) + 16667;
    RET_STDCALL(3, DD_OK);
}

static void DD2_GetAvailableVidMem(CPU *cpu) {
    if (ARG(2)) WR32(ARG(2), 64u << 20);
    if (ARG(3)) WR32(ARG(3), 64u << 20);
    RET_STDCALL(4, DD_OK);
}

/* ---- IDirectDrawSurface ---------------------------------------------------------------- */
static const uint8_t IID_IDirectDrawSurface2[16] = {0x85, 0x58, 0x80, 0x57, 0xec, 0x6e, 0xcf, 0x11, 0x94, 0x41, 0xa8, 0x23, 0x03, 0xc1, 0x0e, 0x27}; /* 57805885-6EEC-11CF-9441-A82303C10E27 */
static const uint8_t IID_IDirectDrawSurface3[16] = {0x00, 0x4e, 0x04, 0xda, 0xb2, 0x69, 0xd0, 0x11, 0xa1, 0xd5, 0x00, 0xaa, 0x00, 0xb8, 0xdf, 0xbb}; /* DA044E00-69B2-11D0-A1D5-00AA00B8DFBB */
static const uint8_t IID_IDirectDrawSurface[16] = {0x81, 0xdb, 0x14, 0x6c, 0x33, 0xa7, 0xce, 0x11, 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}; /* 6C14DB81-A733-11CE-A521-0020AF0BE560 */

static void S_QueryInterface(CPU *cpu) {
    const uint8_t *iid = GPTR(ARG(1));
    Surf *s = surf_from_obj(ARG(0));
    if (!memcmp(iid, IID_IDirectDrawSurface2, 16) || !memcmp(iid, IID_IDirectDrawSurface3, 16) ||
        !memcmp(iid, IID_IDirectDrawSurface, 16)) {
        /* one object serves all versions: the v3 vtable extends v1 */
        s->refs++;
        WR32(s->obj, g_vt_surf3);
        WR32(ARG(2), s->obj);
        RET_STDCALL(3, DD_OK);
    }
    WR32(ARG(2), 0);
    RET_STDCALL(3, E_NOINTERFACE);
}
static void S_AddRef(CPU *cpu) { RET_STDCALL(1, (uint32_t)++surf_from_obj(ARG(0))->refs); }

static void surf_free(Surf *s) {
    if (s->dc) gdi_dc_free(s->dc);
    if (s->own_mem) heap_free(s->mem);
    heap_free(s->obj);
    if ((int)(s - g_surf) == g_primary) g_primary = -1;
    s->used = 0;
}

static void S_Release(CPU *cpu) {
    Surf *s = surf_from_obj(ARG(0));
    int r = --s->refs;
    if (r <= 0) {
        if (s->chain >= 0 && s->chain_pos == 0) {
            /* releasing the primary releases the whole flip chain */
            Chain *c = &g_chains[s->chain];
            for (int k = 0; k < c->count; k++) {
                Surf *o = &g_surf[c->surf[k]];
                o->own_mem = 0;
                heap_free(c->mem[k]);
                surf_free(o);
            }
            c->count = 0;
        } else if (s->chain < 0) {
            surf_free(s);
        }
    }
    RET_STDCALL(1, r < 0 ? 0 : (uint32_t)r);
}

static void S_AddAttachedSurface(CPU *cpu) { RET_STDCALL(2, DD_OK); }
static void S_AddOverlayDirtyRect(CPU *cpu) { RET_STDCALL(2, DD_OK); }

static void read_rect(uint32_t r, const Surf *s, int *x0, int *y0, int *x1, int *y1) {
    if (r) {
        *x0 = (int)RD32(r); *y0 = (int)RD32(r + 4); *x1 = (int)RD32(r + 8); *y1 = (int)RD32(r + 12);
    } else {
        *x0 = 0; *y0 = 0; *x1 = s->w; *y1 = s->h;
    }
}

static inline uint32_t px_get(uint32_t mem, const Surf *s, int x, int y) {
    uint32_t a = mem + (uint32_t)(y * s->pitch);
    switch (s->bpp) {
    case 8: return RD8(a + (uint32_t)x);
    case 16: return RD16(a + (uint32_t)x * 2);
    case 24: return RD8(a + (uint32_t)x * 3) | RD8(a + (uint32_t)x * 3 + 1) << 8 | (uint32_t)RD8(a + (uint32_t)x * 3 + 2) << 16;
    default: return RD32(a + (uint32_t)x * 4);
    }
}
static inline void px_put(uint32_t mem, const Surf *s, int x, int y, uint32_t v) {
    uint32_t a = mem + (uint32_t)(y * s->pitch);
    switch (s->bpp) {
    case 8: WR8(a + (uint32_t)x, v); break;
    case 16: WR16(a + (uint32_t)x * 2, v); break;
    case 24: WR8(a + (uint32_t)x * 3, v); WR8(a + (uint32_t)x * 3 + 1, v >> 8); WR8(a + (uint32_t)x * 3 + 2, v >> 16); break;
    default: WR32(a + (uint32_t)x * 4, v);
    }
}

static void fill(Surf *d, int x0, int y0, int x1, int y1, uint32_t col) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > d->w) x1 = d->w;
    if (y1 > d->h) y1 = d->h;
    uint32_t dm = surf_mem(d);
    for (int y = y0; y < y1; y++) {
        if (d->bpp == 8) { memset(GPTR(dm + (uint32_t)(y * d->pitch + x0)), (int)col, (size_t)(x1 - x0 > 0 ? x1 - x0 : 0)); continue; }
        for (int x = x0; x < x1; x++) px_put(dm, d, x, y, col);
    }
}

/* the general blit: optional stretch, mirror and source/dest colour keys */
static void blit(Surf *d, int dx0, int dy0, int dx1, int dy1, Surf *s, int sx0, int sy0, int sx1, int sy1, int use_srckey,
                 uint32_t srckey, int use_dstkey, uint32_t dstkey, int mirror_lr, int mirror_ud) {
    int dw = dx1 - dx0, dh = dy1 - dy0, sw = sx1 - sx0, sh = sy1 - sy0;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    uint32_t dm = surf_mem(d), sm = surf_mem(s);
    int same = d->bpp == s->bpp;
    int bypp = d->bpp / 8;
    int unscaled = dw == sw && dh == sh && !mirror_lr;
    for (int y = 0; y < dh; y++) {
        int ty = dy0 + y;
        if (ty < 0 || ty >= d->h) continue;
        int ry = mirror_ud ? dh - 1 - y : y;
        int syy = sy0 + (int)((int64_t)ry * sh / dh);
        if (syy < 0 || syy >= s->h) continue;
        if (unscaled && same && !use_srckey && !use_dstkey) {
            int x0 = dx0, xs = sx0, n = dw;
            if (x0 < 0) { xs -= x0; n += x0; x0 = 0; }
            if (xs < 0) { x0 -= xs; n += xs; xs = 0; }
            if (x0 + n > d->w) n = d->w - x0;
            if (xs + n > s->w) n = s->w - xs;
            if (n > 0)
                memmove(GPTR(dm + (uint32_t)(ty * d->pitch + x0 * bypp)), GPTR(sm + (uint32_t)(syy * s->pitch + xs * bypp)), (size_t)(n * bypp));
            continue;
        }
        if (unscaled && same && d->bpp == 8 && use_srckey && !use_dstkey) {
            /* the hot path of 8-bit sprite engines */
            const uint8_t *sp = GPTR(sm + (uint32_t)(syy * s->pitch));
            uint8_t *dp = GPTR(dm + (uint32_t)(ty * d->pitch));
            uint8_t key = (uint8_t)srckey;
            for (int x = 0; x < dw; x++) {
                int tx = dx0 + x, sx = sx0 + x;
                if (tx < 0 || tx >= d->w || sx < 0 || sx >= s->w) continue;
                uint8_t v = sp[sx];
                if (v != key) dp[tx] = v;
            }
            continue;
        }
        for (int x = 0; x < dw; x++) {
            int tx = dx0 + x;
            if (tx < 0 || tx >= d->w) continue;
            int rx = mirror_lr ? dw - 1 - x : x;
            int sxx = sx0 + (int)((int64_t)rx * sw / dw);
            if (sxx < 0 || sxx >= s->w) continue;
            uint32_t v = px_get(sm, s, sxx, syy);
            if (use_srckey && v == srckey) continue;
            if (use_dstkey && px_get(dm, d, tx, ty) != dstkey) continue;
            if (!same) {
                /* format conversion through RGB (rare: 8-bit games use one format) */
                uint32_t rgb;
                if (s->bpp == 8) { const uint32_t *p = surf_palette(s); rgb = p ? p[v & 255] : v * 0x010101u; }
                else if (s->bpp == 16) rgb = ((v >> 11) & 31) << 19 | ((v >> 5) & 63) << 10 | (v & 31) << 3;
                else rgb = v & 0xffffff;
                if (d->bpp == 16) v = ((rgb >> 19) & 31) << 11 | ((rgb >> 10) & 63) << 5 | ((rgb >> 3) & 31);
                else if (d->bpp == 8) {
                    Canvas c;
                    ddraw_surface_canvas(d->obj, &c);
                    v = canvas_pixel_for(&c, rgb);
                } else v = rgb;
            }
            px_put(dm, d, tx, ty, v);
        }
    }
}

static int is_primary_front(Surf *s) { return (int)(s - g_surf) == g_primary; }

/* A write to the visible front buffer. Without back buffers it is the frame itself.
 * With a flip chain it is drawn straight to the screen (Claw's faders: the death iris,
 * the menu scroll): show it too, at most at 60 Hz. */
static void front_written(Surf *s) {
    if (!is_primary_front(s) || s->chain < 0) return;
    if (g_chains[s->chain].count == 1 || platform_ticks_ms() - g_last_flip_ms >= 16) present_primary();
}

static void S_Blt(CPU *cpu) {
    Surf *d = surf_from_obj(ARG(0));
    uint32_t drect = ARG(1), src = ARG(2), srect = ARG(3), flags = ARG(4), fx = ARG(5);
    int x0, y0, x1, y1;
    read_rect(drect, d, &x0, &y0, &x1, &y1);
    if (flags & DDBLT_COLORFILL) {
        fill(d, x0, y0, x1, y1, RD32(fx + 0x50));
    } else if (src) {
        Surf *s = surf_from_obj(src);
        int sx0, sy0, sx1, sy1;
        read_rect(srect, s, &sx0, &sy0, &sx1, &sy1);
        int ks = (flags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)) != 0;
        uint32_t key = (flags & DDBLT_KEYSRCOVERRIDE) ? RD32(fx + 0x5c) : s->srckey;
        if ((flags & DDBLT_KEYSRC) && !s->has_srckey) ks = 0;
        int kd = (flags & DDBLT_KEYDEST) && d->has_dstkey;
        uint32_t ddfx = (flags & DDBLT_DDFX) && fx ? RD32(fx + 4) : 0;
        /* clip to the destination */
        if (x0 < 0 || y0 < 0 || x1 > d->w || y1 > d->h) {
            int dw = x1 - x0, dh = y1 - y0, sw = sx1 - sx0, sh = sy1 - sy0;
            if (dw == sw && dh == sh && !ddfx) {
                if (x0 < 0) { sx0 -= x0; x0 = 0; }
                if (y0 < 0) { sy0 -= y0; y0 = 0; }
                if (x1 > d->w) { sx1 -= x1 - d->w; x1 = d->w; }
                if (y1 > d->h) { sy1 -= y1 - d->h; y1 = d->h; }
            }
        }
        blit(d, x0, y0, x1, y1, s, sx0, sy0, sx1, sy1, ks, key, kd, d->dstkey, (ddfx & 2) != 0, (ddfx & 4) != 0);
    }
    front_written(d);
    RET_STDCALL(6, DD_OK);
}

static void S_BltBatch(CPU *cpu) { RET_STDCALL(4, DD_OK); }

/* BltFast(this, x, y, src, srcrect, trans) */
static void S_BltFast(CPU *cpu) {
    Surf *d = surf_from_obj(ARG(0));
    int dx = (int)ARG(1), dy = (int)ARG(2);
    Surf *s = surf_from_obj(ARG(3));
    uint32_t trans = ARG(5);
    int sx0, sy0, sx1, sy1;
    read_rect(ARG(4), s, &sx0, &sy0, &sx1, &sy1);
    blit(d, dx, dy, dx + sx1 - sx0, dy + sy1 - sy0, s, sx0, sy0, sx1, sy1, (trans & 1) && s->has_srckey, s->srckey,
         (trans & 2) && d->has_dstkey, d->dstkey, 0, 0);
    front_written(d);
    RET_STDCALL(6, DD_OK);
}

static void S_DeleteAttachedSurface(CPU *cpu) { RET_STDCALL(3, DD_OK); }
static void S_EnumAttachedSurfaces(CPU *cpu) { RET_STDCALL(3, DD_OK); }
static void S_EnumOverlayZOrders(CPU *cpu) { RET_STDCALL(4, DD_OK); }

static void S_Flip(CPU *cpu) {
    Surf *s = surf_from_obj(ARG(0));
    if (s->chain < 0) RET_STDCALL(3, DDERR_INVALIDPARAMS);
    Chain *c = &g_chains[s->chain];
    if (c->count > 1) {
        /* the first back buffer becomes visible; buffers rotate towards the front */
        uint32_t front = c->mem[0];
        for (int k = 0; k < c->count - 1; k++) c->mem[k] = c->mem[k + 1];
        c->mem[c->count - 1] = front;
        for (int k = 0; k < c->count; k++) g_surf[c->surf[k]].mem = c->mem[k];
    }
    present_primary();
    wait_vblank();
    RET_STDCALL(3, DD_OK);
}

static void S_GetAttachedSurface(CPU *cpu) {
    Surf *s = surf_from_obj(ARG(0));
    uint32_t caps = RD32(ARG(1)), pp = ARG(2);
    if (s->chain >= 0 && (caps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP))) {
        Chain *c = &g_chains[s->chain];
        int next = s->chain_pos + 1;
        if (next < c->count) {
            Surf *b = &g_surf[c->surf[next]];
            b->refs++;
            WR32(pp, b->obj);
            RET_STDCALL(3, DD_OK);
        }
    }
    WR32(pp, 0);
    RET_STDCALL(3, DDERR_NOTFOUND);
}

static void S_GetBltStatus(CPU *cpu) { RET_STDCALL(2, DD_OK); }
static void S_GetCaps(CPU *cpu) { WR32(ARG(1), surf_from_obj(ARG(0))->caps); RET_STDCALL(2, DD_OK); }
static void S_GetClipper(CPU *cpu) {
    Surf *s = surf_from_obj(ARG(0));
    WR32(ARG(1), s->clipper);
    RET_STDCALL(2, s->clipper ? DD_OK : DDERR_NOCLIPPERATTACHED);
}
static void S_GetColorKey(CPU *cpu) {
    Surf *s = surf_from_obj(ARG(0));
    uint32_t f = ARG(1), p = ARG(2);
    if ((f & 8) && s->has_srckey) { WR32(p, s->srckey); WR32(p + 4, s->srckey); RET_STDCALL(3, DD_OK); }
    if ((f & 2) && s->has_dstkey) { WR32(p, s->dstkey); WR32(p + 4, s->dstkey); RET_STDCALL(3, DD_OK); }
    RET_STDCALL(3, DDERR_NOCOLORKEY);
}

static void S_GetDC(CPU *cpu) {
    Surf *s = surf_from_obj(ARG(0));
    if (!s->dc) s->dc = gdi_dc_new(DC_SURFACE, 0, s->obj);
    WR32(ARG(1), s->dc);
    RET_STDCALL(2, DD_OK);
}
static void S_ReleaseDC(CPU *cpu) {
    Surf *s = surf_from_obj(ARG(0));
    if (s->dc) { gdi_dc_free(s->dc); s->dc = 0; }
    front_written(s);
    RET_STDCALL(2, DD_OK);
}

static void S_GetFlipStatus(CPU *cpu) { RET_STDCALL(2, DD_OK); }
static void S_GetOverlayPosition(CPU *cpu) { RET_STDCALL(3, DDERR_GENERIC); }
static void S_GetPalette(CPU *cpu) {
    Surf *s = surf_from_obj(ARG(0));
    if (s->pal < 0) { WR32(ARG(1), 0); RET_STDCALL(2, DDERR_NOPALETTEATTACHED); }
    g_pal[s->pal].refs++;
    WR32(ARG(1), g_pal[s->pal].obj);
    RET_STDCALL(2, DD_OK);
}
static void S_GetPixelFormat(CPU *cpu) { write_pixel_format(ARG(1), surf_from_obj(ARG(0))->bpp); RET_STDCALL(2, DD_OK); }
static void S_GetSurfaceDesc(CPU *cpu) { write_desc(ARG(1), surf_from_obj(ARG(0))); RET_STDCALL(2, DD_OK); }
static void S_Initialize(CPU *cpu) { RET_STDCALL(3, DD_OK); }
static void S_IsLost(CPU *cpu) { RET_STDCALL(1, DD_OK); }

static void S_Lock(CPU *cpu) {
    Surf *s = surf_from_obj(ARG(0));
    uint32_t r = ARG(1), d = ARG(2);
    write_desc(d, s);
    if (r) WR32(d + 36, surf_mem(s) + RD32(r + 4) * (uint32_t)s->pitch + RD32(r) * (uint32_t)(s->bpp / 8));
    RET_STDCALL(5, DD_OK);
}

static void S_Unlock(CPU *cpu) {
    Surf *s = surf_from_obj(ARG(0));
    front_written(s);
    RET_STDCALL(2, DD_OK);
}
static void S_Restore(CPU *cpu) { RET_STDCALL(1, DD_OK); }
static void S_SetClipper(CPU *cpu) { surf_from_obj(ARG(0))->clipper = ARG(1); RET_STDCALL(2, DD_OK); }
static void S_SetColorKey(CPU *cpu) {
    Surf *s = surf_from_obj(ARG(0));
    uint32_t f = ARG(1), p = ARG(2);
    if (f & 8) { s->has_srckey = p != 0; s->srckey = p ? RD32(p) : 0; }  /* DDCKEY_SRCBLT */
    if (f & 2) { s->has_dstkey = p != 0; s->dstkey = p ? RD32(p) : 0; }  /* DDCKEY_DESTBLT */
    RET_STDCALL(3, DD_OK);
}
static void S_SetOverlayPosition(CPU *cpu) { RET_STDCALL(3, DD_OK); }
static void S_SetPalette(CPU *cpu) {
    Surf *s = surf_from_obj(ARG(0));
    uint32_t p = ARG(1);
    int idx = -1;
    if (p) {
        idx = (int)RD32(p + 4);
        g_pal[idx].refs++;
    }
    s->pal = idx;
    /* palettes attach to the whole flip chain */
    if (s->chain >= 0)
        for (int k = 0; k < g_chains[s->chain].count; k++) g_surf[g_chains[s->chain].surf[k]].pal = idx;
    RET_STDCALL(2, DD_OK);
}
static void S_UpdateOverlay(CPU *cpu) { RET_STDCALL(6, DD_OK); }
static void S_UpdateOverlayDisplay(CPU *cpu) { RET_STDCALL(2, DD_OK); }
static void S_UpdateOverlayZOrder(CPU *cpu) { RET_STDCALL(3, DD_OK); }
/* IDirectDrawSurface2/3 */
static void S_GetDDInterface(CPU *cpu) { WR32(ARG(1), g_dd_obj); RET_STDCALL(2, DD_OK); }
static void S_PageLock(CPU *cpu) { RET_STDCALL(2, DD_OK); }
static void S_PageUnlock(CPU *cpu) { RET_STDCALL(2, DD_OK); }
static void S_SetSurfaceDesc(CPU *cpu) {
    Surf *s = surf_from_obj(ARG(0));
    uint32_t d = ARG(1);
    if (RD32(d + 4) & DDSD_LPSURFACE) { s->mem = RD32(d + 36); s->own_mem = 0; }
    RET_STDCALL(3, DD_OK);
}

#define VT(name, ...) do { \
    static const GuestFn fns[] = {__VA_ARGS__}; \
    name = com_make_vtable(fns, NULL, (int)(sizeof fns / sizeof fns[0])); } while (0)

#define SURF_V1 S_QueryInterface, S_AddRef, S_Release, S_AddAttachedSurface, S_AddOverlayDirtyRect, \
       S_Blt, S_BltBatch, S_BltFast, S_DeleteAttachedSurface, S_EnumAttachedSurfaces, S_EnumOverlayZOrders, \
       S_Flip, S_GetAttachedSurface, S_GetBltStatus, S_GetCaps, S_GetClipper, S_GetColorKey, S_GetDC, \
       S_GetFlipStatus, S_GetOverlayPosition, S_GetPalette, S_GetPixelFormat, S_GetSurfaceDesc, \
       S_Initialize, S_IsLost, S_Lock, S_ReleaseDC, S_Restore, S_SetClipper, S_SetColorKey, \
       S_SetOverlayPosition, S_SetPalette, S_Unlock, S_UpdateOverlay, S_UpdateOverlayDisplay, \
       S_UpdateOverlayZOrder

static void ddraw_init(void) {
    if (g_vt_dd) return;
    VT(g_vt_dd, DD_QueryInterface, DD_AddRef, DD_Release, DD_Compact, DD_CreateClipper, DD_CreatePalette,
       DD_CreateSurface, DD_DuplicateSurface, DD_EnumDisplayModes, DD_EnumSurfaces, DD_FlipToGDISurface,
       DD_GetCaps, DD_GetDisplayMode, DD_GetFourCCCodes, DD_GetGDISurface, DD_GetMonitorFrequency,
       DD_GetScanLine, DD_GetVerticalBlankStatus, DD_Initialize, DD_RestoreDisplayMode,
       DD_SetCooperativeLevel, DD_SetDisplayMode, DD_WaitForVerticalBlank);
    VT(g_vt_dd2, DD_QueryInterface, DD_AddRef, DD_Release, DD_Compact, DD_CreateClipper, DD_CreatePalette,
       DD_CreateSurface, DD_DuplicateSurface, DD_EnumDisplayModes, DD_EnumSurfaces, DD_FlipToGDISurface,
       DD_GetCaps, DD_GetDisplayMode, DD_GetFourCCCodes, DD_GetGDISurface, DD_GetMonitorFrequency,
       DD_GetScanLine, DD_GetVerticalBlankStatus, DD_Initialize, DD_RestoreDisplayMode,
       DD_SetCooperativeLevel, DD2_SetDisplayMode, DD_WaitForVerticalBlank, DD2_GetAvailableVidMem);
    VT(g_vt_surf, SURF_V1);
    VT(g_vt_surf3, SURF_V1, S_GetDDInterface, S_PageLock, S_PageUnlock, S_SetSurfaceDesc);
    VT(g_vt_pal, P_QueryInterface, P_AddRef, P_Release, P_GetCaps, P_GetEntries, P_Initialize, P_SetEntries);
    VT(g_vt_clip, C_QueryInterface, C_AddRef, C_Release, C_GetClipList, C_GetHWnd, C_Initialize, C_IsClipListChanged,
       C_SetClipList, C_SetHWnd);
    g_dd_obj = guest_alloc(16);
    WR32(g_dd_obj, g_vt_dd);
    g_dd2_obj = guest_alloc(16);
    WR32(g_dd2_obj, g_vt_dd2);
}

/* HRESULT DirectDrawCreate(GUID *, LPDIRECTDRAW *, IUnknown *) */
void ddraw_DirectDrawCreate(CPU *cpu) {
    ddraw_init();
    WR32(ARG(1), g_dd_obj);
    RET_STDCALL(3, DD_OK);
}
void imp_DDRAW_DirectDrawCreate(CPU *cpu) { ddraw_DirectDrawCreate(cpu); }

/* HRESULT DirectDrawEnumerateA(LPDDENUMCALLBACKA, LPVOID): the primary display only */
void ddraw_DirectDrawEnumerateA(CPU *cpu) {
    uint32_t cb = ARG(0), ctx = ARG(1);
    uint32_t desc = guest_strdup("Primary Display Driver"), name = guest_strdup("display");
    uint32_t args[4] = {0, desc, name, ctx};
    guest_call(cpu, cb, 4, args);
    heap_free(desc);
    heap_free(name);
    RET_STDCALL(2, DD_OK);
}

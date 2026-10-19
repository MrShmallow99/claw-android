/* GDI internals shared by gdi32.c, user32.c (window painting) and ddraw.c (surface DCs). */
#pragma once
#include <stdint.h>
#include "win32.h"

/* A pixel buffer GDI can draw into.  bits may be guest or host memory; pitch may be
 * negative (bottom-up DIBs).  8 bpp canvases carry a palette of 0x00RRGGBB entries. */
typedef struct Canvas {
    uint8_t *bits;
    int w, h, pitch;
    int bpp;            /* 8, 16, 24, 32 */
    int rgb565;         /* 16 bpp: 1 = 5-6-5, 0 = 5-5-5 */
    const uint32_t *pal;
    int pal_n;
} Canvas;

typedef struct { int l, t, r, b; } Rect;

/* COLORREF (0x00BBGGRR) <-> canvas pixels */
uint32_t canvas_get(const Canvas *c, int x, int y);              /* 0x00RRGGBB */
void canvas_put(const Canvas *c, int x, int y, uint32_t rgb);     /* 0x00RRGGBB */
uint32_t canvas_pixel_for(const Canvas *c, uint32_t rgb);         /* raw pixel value */
void canvas_fill(const Canvas *c, Rect r, uint32_t rgb);

/* The window-system framebuffer (32 bpp, screen size), owned by user32.c. */
Canvas *desktop_canvas(void);
void desktop_resize(int w, int h);

/* DCs */
enum { DC_MEMORY = 1, DC_WINDOW, DC_SURFACE, DC_SCREEN };
uint32_t gdi_dc_new(int kind, uint32_t hwnd, uint32_t surface);
void gdi_dc_free(uint32_t hdc);
void gdi_dc_set_window_part(uint32_t hdc, int client); /* 0: whole window (GetWindowDC) */
/* Window DCs: origin and clip of a window's client area on the desktop (user32.c). */
int user32_dc_geometry(uint32_t hwnd, int client, int *ox, int *oy, Rect *clip);
/* Surface DCs: the canvas of a DirectDraw surface object (ddraw.c). */
int ddraw_surface_canvas(uint32_t surface, Canvas *out);

/* Painting helpers user32 needs (controls, frames) */
void gdi_fill_rect_rgb(uint32_t hdc, Rect r, uint32_t colorref);
void gdi_fill_rect_brush(uint32_t hdc, Rect r, uint32_t hbrush);
void gdi_frame_rect_rgb(uint32_t hdc, Rect r, uint32_t colorref);
void gdi_draw_edge(uint32_t hdc, Rect r, int raised, int deep);
/* draws text with the DC's font; flags are DT_* ; returns the height */
int gdi_draw_text(uint32_t hdc, const char *s, int n, Rect *r, uint32_t flags);
int gdi_text_width(uint32_t hdc, const char *s, int n);
int gdi_font_height(uint32_t hdc);
uint32_t gdi_stock(int n);
uint32_t gdi_select(uint32_t hdc, uint32_t obj);
void gdi_set_text_color(uint32_t hdc, uint32_t c);
void gdi_set_bk_mode(uint32_t hdc, int mode);
uint32_t gdi_create_solid_brush(uint32_t colorref);
uint32_t gdi_brush_color(uint32_t hbrush, int *is_null);
/* system colors (COLOR_*) */
uint32_t sys_color(int idx);
uint32_t sys_color_brush(int idx);
/* draws an icon resource (group id string "#n" or name) scaled into r */
int gdi_draw_icon_handle(uint32_t hdc, int x, int y, uint32_t hicon);
uint32_t gdi_load_icon(const char *name);
/* default dialog font (MS Sans Serif 8 -> Liberation Sans) */
uint32_t gdi_dialog_font(int pt);

#define RGB_OF(cr) ((((cr) & 0xff) << 16) | ((cr) & 0xff00) | (((cr) >> 16) & 0xff))
#define CR_OF(rgb) ((((rgb) & 0xff) << 16) | ((rgb) & 0xff00) | (((rgb) >> 16) & 0xff))

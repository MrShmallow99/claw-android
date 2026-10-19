/* Widescreen: a port option, not in the original game. Games that can run at any
 * resolution (Claw: CrazyHook's ChangeResolution) are switched, during gameplay, to a
 * resolution as wide as the screen at the game's own height: more of the world is visible,
 * nothing is stretched. Configured by [widescreen] in game.ini:
 *   object  = pointer to the object holding the resolution
 *   width / height = offsets of the current width and height in it
 *   command = WM_COMMAND id that applies them (posted to the game window)
 *   base    = the game's own resolution (WxH); only that one is replaced, so levels that
 *             pick their own resolution keep it
 * The choice is saved in <save>/display.ini. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "runtime.h"
#include "gamecfg.h"
#include "platform.h"

uint32_t ddraw_coop_hwnd(void);
void wm_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
int input_in_gameplay(void);
int cheatmenu_is_open(void);
float platform_display_aspect(void);

static int g_avail = -1, g_on;
static uint32_t g_obj, g_off_w, g_off_h, g_cmd;
static int g_base_w = 640, g_base_h = 480;
static uint32_t g_next_ms;
static char g_path[512];

static void ws_init(void) {
    if (g_avail >= 0) return;
    g_obj = (uint32_t)cfg_int("widescreen", "object", 0);
    g_off_w = (uint32_t)cfg_int("widescreen", "width", 0);
    g_off_h = (uint32_t)cfg_int("widescreen", "height", 0);
    g_cmd = (uint32_t)cfg_int("widescreen", "command", 0);
    sscanf(cfg_str("widescreen", "base", "640x480"), "%dx%d", &g_base_w, &g_base_h);
    g_avail = g_obj && g_off_h && g_cmd;
    g_on = cfg_int("widescreen", "default", 0);
    snprintf(g_path, sizeof g_path, "%s/display.ini", platform_save_dir());
    FILE *f = fopen(g_path, "r");
    if (f) {
        char line[64];
        int v;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "widescreen=%d", &v) == 1) g_on = v != 0;
        fclose(f);
    }
    if (getenv("PORT_WIDESCREEN")) g_on = atoi(getenv("PORT_WIDESCREEN")) != 0;
}

int widescreen_available(void) { ws_init(); return g_avail; }
int widescreen_enabled(void) { ws_init(); return g_on; }

void widescreen_set(int on) {
    ws_init();
    g_on = on != 0;
    g_next_ms = 0;
    FILE *f = fopen(g_path, "w");
    if (f) { fprintf(f, "widescreen=%d\n", g_on); fclose(f); }
    rt_log("widescreen %s", g_on ? "on" : "off");
}

/* the widescreen size for this screen: the game's height, the screen's shape */
void widescreen_size(int *w, int *h) {
    ws_init();
    float a = platform_display_aspect();
    if (getenv("PORT_WIDESCREEN_ASPECT")) a = (float)atof(getenv("PORT_WIDESCREEN_ASPECT"));
    float base_a = (float)g_base_w / (float)g_base_h;
    if (a < base_a) a = base_a;
    if (a > 2.4f) a = 2.4f; /* beyond that the HUD and the levels' edges show */
    *h = g_base_h;
    *w = (int)lroundf(g_base_h * a / 8.0f) * 8; /* surfaces like widths in multiples of 8 */
}

/* from the main loop: keep the game at the chosen resolution while it is played */
void widescreen_tick(void) {
    ws_init();
    if (!g_avail) return;
    uint32_t now = platform_ticks_ms();
    if (now < g_next_ms || cheatmenu_is_open() || !input_in_gameplay()) return;
    g_next_ms = now + 250;
    uint32_t obj = RD32(g_obj);
    uint32_t hwnd = ddraw_coop_hwnd();
    if (!obj || !hwnd) return;
    int cw = (int)RD32(obj + g_off_w), ch = (int)RD32(obj + g_off_h);
    int ww, wh;
    widescreen_size(&ww, &wh);
    int tw, th;
    if (g_on && cw == g_base_w && ch == g_base_h && ww != g_base_w) { tw = ww; th = wh; }
    else if (!g_on && cw == ww && ch == wh && ww != g_base_w) { tw = g_base_w; th = g_base_h; }
    else return;
    rt_log("widescreen: %dx%d -> %dx%d", cw, ch, tw, th);
    WR32(obj + g_off_w, (uint32_t)tw);
    WR32(obj + g_off_h, (uint32_t)th);
    wm_post(hwnd, 0x111, g_cmd, 0); /* WM_COMMAND */
    g_next_ms = now + 1000;
}

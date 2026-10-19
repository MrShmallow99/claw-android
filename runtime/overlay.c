/* Floating on-screen touch controls.
 *
 * Left half: a floating joystick that appears where the thumb lands (arrow keys).
 * Right side: action buttons from game.ini [touch]; each button belongs to the
 * gameplay layout, the menu layout or both (see input_in_gameplay).
 * A small toggle button (top right) shows or hides the controls; the choice is
 * remembered.  The controls also hide while a physical controller is in use. */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "platform.h"
#include "runtime.h"
#include "gamecfg.h"

void platform_get_game_rect(int *x, int *y, int *w, int *h);

/* ---- tiny 5x7 font ------------------------------------------------------------- */
static const uint8_t FONT[][7] = {
    /* A */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* B */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    /* C */ {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* D */ {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    /* E */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* F */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    /* G */ {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* H */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* I */ {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* J */ {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    /* K */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* L */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    /* M */ {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* N */ {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    /* O */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* P */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    /* Q */ {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* R */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    /* S */ {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* T */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    /* U */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* V */ {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    /* W */ {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* X */ {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    /* Y */ {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, /* Z */ {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
};
static const uint8_t GLYPH_LT[7] = {0x02,0x04,0x08,0x10,0x08,0x04,0x02};
static const uint8_t GLYPH_GT[7] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08};
static const uint8_t GLYPH_BAR2[7] = {0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B}; /* pause */

static const uint8_t *glyph(char c) {
    if (c >= 'A' && c <= 'Z') return FONT[c - 'A'];
    if (c == '<') return GLYPH_LT;
    if (c == '>') return GLYPH_GT;
    if (c == '|') return GLYPH_BAR2;
    return NULL;
}

/* Render a text label into a white-on-transparent texture (scale px per font pixel). */
static SDL_Texture *make_label(SDL_Renderer *r, const char *text, int scale) {
    int n = (int)strlen(text);
    int w = (n * 6 - 1) * scale + 2 * scale, h = 9 * scale;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_FillRect(s, NULL, 0);
    uint32_t *px = s->pixels;
    int pitch = s->pitch / 4;
    for (int i = 0; i < n; i++) {
        const uint8_t *g = glyph(text[i]);
        if (!g) continue;
        for (int y = 0; y < 7; y++)
            for (int x = 0; x < 5; x++)
                if (g[y] & (0x10 >> x)) {
                    /* 1px dark outline for readability on bright scenes */
                    for (int oy = -1; oy <= 1; oy++)
                        for (int ox = -1; ox <= 1; ox++) {
                            int bx = scale + (i * 6 + x) * scale + ox * (scale / 2 + 1);
                            int by = scale + y * scale + oy * (scale / 2 + 1);
                            for (int yy = 0; yy < scale; yy++)
                                for (int xx = 0; xx < scale; xx++) {
                                    int X = bx + xx, Y = by + yy;
                                    if (X >= 0 && Y >= 0 && X < w && Y < h && !(px[Y * pitch + X] & 0x00ffffff))
                                        px[Y * pitch + X] = 0xa0000000u;
                                }
                        }
                }
        for (int y = 0; y < 7; y++)
            for (int x = 0; x < 5; x++)
                if (g[y] & (0x10 >> x))
                    for (int yy = 0; yy < scale; yy++)
                        for (int xx = 0; xx < scale; xx++)
                            px[(scale + y * scale + yy) * pitch + scale + (i * 6 + x) * scale + xx] = 0xffffffffu;
    }
    SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(s);
    return t;
}

/* Anti-aliased disc with a ring, used for every button and the joystick. */
static SDL_Texture *make_disc(SDL_Renderer *r, int size, uint32_t fill_a, uint32_t ring_a, float ring_w) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
    uint32_t *px = s->pixels;
    float c = size / 2.0f, rad = size / 2.0f - 1.0f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float d = sqrtf((x + 0.5f - c) * (x + 0.5f - c) + (y + 0.5f - c) * (y + 0.5f - c));
            float inside = fminf(fmaxf(rad - d + 0.5f, 0.0f), 1.0f);
            float ring = fminf(fmaxf(d - (rad - ring_w) + 0.5f, 0.0f), 1.0f) * inside;
            float a = inside * fill_a * (1 - ring) + ring * ring_a;
            uint32_t col = ring > 0.5f ? 0xffffff : 0x202020;
            px[y * (s->pitch / 4) + x] = ((uint32_t)a << 24) | col;
        }
    SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(s);
    return t;
}

/* ---- controls ------------------------------------------------------------------------ */
enum { CTX_GAME = 1, CTX_MENU = 2, CTX_ALL = 3 };

typedef struct {
    char label[16];
    int vk;
    int ctx;
    float x, y, r;      /* layout in units of screen height, x measured from the right edge */
    SDL_Texture *tex;
    int fingers;        /* number of fingers on it */
    int sent_vk;        /* VK actually pressed (context may change while held) */
} Button;

#define MAX_BUTTONS 32
static Button g_buttons[MAX_BUTTONS];
static int NBUTTONS;

/* "button = LABEL KEY CONTEXT X Y RADIUS" */
static void add_button(const char *v, void *ctx) {
    (void)ctx;
    char label[16], key[32], c[16];
    float x, y, r;
    if (NBUTTONS >= MAX_BUTTONS || sscanf(v, "%15s %31s %15s %f %f %f", label, key, c, &x, &y, &r) != 6) {
        rt_log("game.ini: bad button \"%s\"", v);
        return;
    }
    Button *b = &g_buttons[NBUTTONS++];
    snprintf(b->label, sizeof b->label, "%s", label);
    b->vk = cfg_vk(key);
    b->ctx = !strcasecmp(c, "game") ? CTX_GAME : !strcasecmp(c, "menu") ? CTX_MENU : CTX_ALL;
    b->x = x; b->y = y; b->r = r;
}

/* Optional resolution toggle: "res_toggle = LABEL WIDE_FROM KEY_WHEN_WIDE KEY_WHEN_NARROW". */
static char g_res_label[16];
static int g_res_wide = 640, g_res_vk_wide, g_res_vk_narrow;

static SDL_Texture *g_disc, *g_disc_pressed, *g_stick_base, *g_stick_knob, *g_toggle_on, *g_toggle_off;
static SDL_Texture *g_hd_label;
static int g_hd_finger_vk;         /* key held by the resolution toggle */
int platform_frame_width(void);
static int g_enabled = 1;         /* user preference */
static int g_pad_hidden;          /* hidden because a controller is in use */
static float g_opacity = 0.75f;

/* joystick state */
static SDL_FingerID g_stick_finger = -1;
static int g_stick_active;
static float g_stick_cx, g_stick_cy, g_stick_x, g_stick_y;
static int g_dir[4]; /* up down left right */

/* finger -> button tracking */
#define MAX_FINGERS 10
static struct { SDL_FingerID id; int used; int button; int toggle; } g_fingers[MAX_FINGERS];

static char g_cfg_path[1200];

static void save_settings(void) {
    FILE *f = fopen(g_cfg_path, "w");
    if (!f) return;
    fprintf(f, "touch_controls=%d\nopacity=%.2f\n", g_enabled, g_opacity);
    fclose(f);
}

static void load_settings(void) {
    snprintf(g_cfg_path, sizeof g_cfg_path, "%s/controls.ini", platform_save_dir());
    FILE *f = fopen(g_cfg_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int v;
        float fv;
        if (sscanf(line, "touch_controls=%d", &v) == 1) g_enabled = v != 0;
        if (sscanf(line, "opacity=%f", &fv) == 1 && fv > 0.1f && fv <= 1.0f) g_opacity = fv;
    }
    fclose(f);
}

static int g_has_touch;

void overlay_init(SDL_Renderer *r, int w, int h) {
    (void)w;
    load_settings();
    NBUTTONS = 0;
    cfg_each("touch", "button", add_button, NULL);
    char key_w[32], key_n[32];
    g_res_label[0] = 0;
    if (sscanf(cfg_str("touch", "res_toggle", ""), "%15s %d %31s %31s", g_res_label, &g_res_wide, key_w, key_n) == 4) {
        g_res_vk_wide = cfg_vk(key_w);
        g_res_vk_narrow = cfg_vk(key_n);
    } else {
        g_res_label[0] = 0;
    }
    int ds = h / 4 > 64 ? h / 4 : 64;
    g_disc = make_disc(r, ds, 70, 150, ds * 0.045f);
    g_disc_pressed = make_disc(r, ds, 150, 230, ds * 0.06f);
    g_stick_base = make_disc(r, ds, 35, 110, ds * 0.03f);
    g_stick_knob = make_disc(r, ds, 120, 200, ds * 0.06f);
    int scale = h / 180 > 2 ? h / 180 : 2;
    for (int i = 0; i < NBUTTONS; i++) g_buttons[i].tex = make_label(r, g_buttons[i].label, scale);
    g_hd_label = g_res_label[0] ? make_label(r, g_res_label, scale) : NULL;
    g_toggle_on = make_label(r, "PAD", scale);
    g_toggle_off = make_label(r, "PAD", scale);
    SDL_SetTextureAlphaMod(g_toggle_off, 110);
    g_has_touch = SDL_GetNumTouchDevices() > 0;
#ifdef __ANDROID__
    g_has_touch = 1;
#endif
    if (getenv("PORT_TOUCH")) g_has_touch = 1;
}

void overlay_set_gamepad_active(int active) {
    if (active != g_pad_hidden) {
        g_pad_hidden = active;
        rt_log("touch controls %s (controller %s)", active ? "hidden" : "shown", active ? "in use" : "idle");
    }
}

static int visible(void) { return g_has_touch && g_enabled && !g_pad_hidden; }

static int current_ctx(void) { return input_in_gameplay() ? CTX_GAME : CTX_MENU; }

static void button_rect(const Button *b, int w, int h, SDL_FRect *out, float *cx, float *cy, float *rad) {
    float H = (float)h;
    float x = b->x < 0 ? 0.06f * H + b->r * H : (float)w - b->x * H;
    float y = b->y * H;
    if (b->y > 0.12f) y = H - b->y * H; /* measured from the bottom */
    *cx = x; *cy = y; *rad = b->r * H;
    out->x = x - *rad; out->y = y - *rad; out->w = out->h = 2 * *rad;
}

static void toggle_rect(int w, int h, SDL_Rect *out) {
    int s = h / 12;
    *out = (SDL_Rect){w - s * 2 - h / 40, h / 40, s * 2, s};
}

/* Resolution toggle: sends the game's own resolution hotkeys ([touch] res_toggle). */
static void hd_rect(int w, int h, SDL_Rect *out) {
    SDL_Rect t;
    toggle_rect(w, h, &t);
    *out = (SDL_Rect){t.x - t.w - h / 40, t.y, t.w, t.h};
}

/* ---- input ----------------------------------------------------------------------------------- */
static void set_dirs(int up, int down, int left, int right) {
    int nd[4] = {up, down, left, right};
    int vks[4];
    input_dir_vks(vks);
    for (int i = 0; i < 4; i++)
        if (nd[i] != g_dir[i]) {
            g_dir[i] = nd[i];
            input_set_vk(vks[i], INPUT_SRC_TOUCH, nd[i]);
        }
}

static void update_stick(int h) {
    float dx = g_stick_x - g_stick_cx, dy = g_stick_y - g_stick_cy;
    float maxr = 0.12f * h;
    float d = sqrtf(dx * dx + dy * dy);
    if (d > maxr) {
        /* drag the base along so the thumb never leaves the stick */
        g_stick_cx = g_stick_x - dx / d * maxr;
        g_stick_cy = g_stick_y - dy / d * maxr;
        dx = g_stick_x - g_stick_cx;
        dy = g_stick_y - g_stick_cy;
        d = maxr;
    }
    if (d < 0.25f * maxr) {
        set_dirs(0, 0, 0, 0);
        return;
    }
    float a = atan2f(-dy, dx); /* 0 = right, counter-clockwise */
    int sector = (int)floorf((a + (float)M_PI / 8) / ((float)M_PI / 4)) & 7;
    static const int map[8][4] = {
        {0, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 0}, {1, 0, 1, 0},
        {0, 0, 1, 0}, {0, 1, 1, 0}, {0, 1, 0, 0}, {0, 1, 0, 1},
    };
    set_dirs(map[sector][0], map[sector][1], map[sector][2], map[sector][3]);
}

static void press_button(int i, int down) {
    Button *b = &g_buttons[i];
    if (down) {
        if (b->fingers++ == 0) {
            b->sent_vk = b->vk;
            input_set_vk(b->sent_vk, INPUT_SRC_TOUCH, 1);
        }
    } else if (b->fingers > 0 && --b->fingers == 0) {
        input_set_vk(b->sent_vk, INPUT_SRC_TOUCH, 0);
    }
}

static int hit_button(float x, float y, int w, int h) {
    int ctx = current_ctx();
    int best = -1;
    float bestd = 1e9f;
    for (int i = 0; i < NBUTTONS; i++) {
        Button *b = &g_buttons[i];
        if (!(b->ctx & ctx)) continue;
        SDL_FRect r;
        float cx, cy, rad;
        button_rect(b, w, h, &r, &cx, &cy, &rad);
        float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
        /* generous hit area: 1.25x radius, nearest wins */
        if (d < rad * 1.25f && d / rad < bestd) {
            bestd = d / rad;
            best = i;
        }
    }
    return best;
}

static int finger_slot(SDL_FingerID id, int create) {
    for (int i = 0; i < MAX_FINGERS; i++)
        if (g_fingers[i].used && g_fingers[i].id == id) return i;
    if (!create) return -1;
    for (int i = 0; i < MAX_FINGERS; i++)
        if (!g_fingers[i].used) {
            g_fingers[i] = (typeof(g_fingers[0])){id, 1, -1, 0};
            return i;
        }
    return -1;
}

static void release_all(void) {
    for (int i = 0; i < NBUTTONS; i++) {
        if (g_buttons[i].fingers) input_set_vk(g_buttons[i].sent_vk, INPUT_SRC_TOUCH, 0);
        g_buttons[i].fingers = 0;
    }
    memset(g_fingers, 0, sizeof g_fingers);
    g_stick_active = 0;
    g_stick_finger = -1;
    set_dirs(0, 0, 0, 0);
    input_clear_source(INPUT_SRC_TOUCH);
}

void overlay_release_all(void) { release_all(); }

/* returns 1 when the overlay keeps the finger; other touches act as the mouse (dialogs) */
static int finger_down(SDL_FingerID id, float x, float y, int w, int h) {
    g_has_touch = 1;
    if (g_pad_hidden) overlay_set_gamepad_active(0); /* touching the screen brings the controls back */
    int s = finger_slot(id, 1);
    if (s < 0) return 1;
    SDL_Rect tr;
    toggle_rect(w, h, &tr);
    if (x >= tr.x - h / 40 && x <= tr.x + tr.w + h / 40 && y <= tr.y + tr.h + h / 40) {
        g_fingers[s].toggle = 1;
        g_enabled = !g_enabled;
        save_settings();
        if (!g_enabled) release_all();
        return 1;
    }
    if (!visible()) { g_fingers[s].used = 0; return 0; }
    SDL_Rect hr;
    hd_rect(w, h, &hr);
    if (g_hd_label && x >= hr.x && x <= hr.x + hr.w && y <= hr.y + hr.h + h / 40) {
        g_fingers[s].toggle = 2;
        g_hd_finger_vk = platform_frame_width() >= g_res_wide ? g_res_vk_wide : g_res_vk_narrow;
        input_set_vk(g_hd_finger_vk, INPUT_SRC_TOUCH, 1);
        return 1;
    }
    int b = hit_button(x, y, w, h);
    if (b >= 0) {
        g_fingers[s].button = b;
        press_button(b, 1);
        return 1;
    }
    /* no joystick over a Windows dialog: there the touch is the mouse */
    extern int wm_gdi_overlay_active(void);
    if (x < w * 0.5f && g_stick_finger == (SDL_FingerID)-1 && !wm_gdi_overlay_active()) {
        g_stick_finger = id;
        g_stick_active = 1;
        g_stick_cx = g_stick_x = x;
        g_stick_cy = g_stick_y = y;
        update_stick(h);
        return 1;
    }
    g_fingers[s].used = 0;
    return 0;
}

static int finger_is_ours(SDL_FingerID id) { return id == g_stick_finger || finger_slot(id, 0) >= 0; }

static void finger_motion(SDL_FingerID id, float x, float y, int w, int h) {
    if (id == g_stick_finger && g_stick_active) {
        g_stick_x = x;
        g_stick_y = y;
        update_stick(h);
        return;
    }
    int s = finger_slot(id, 0);
    if (s < 0 || g_fingers[s].toggle || g_fingers[s].button < 0) return;
    /* allow sliding a thumb from one button to another */
    int b = hit_button(x, y, w, h);
    if (b >= 0 && b != g_fingers[s].button) {
        press_button(g_fingers[s].button, 0);
        press_button(b, 1);
        g_fingers[s].button = b;
    }
}

static void finger_up(SDL_FingerID id) {
    if (id == g_stick_finger) {
        g_stick_finger = -1;
        g_stick_active = 0;
        set_dirs(0, 0, 0, 0);
    }
    int s = finger_slot(id, 0);
    if (s < 0) return;
    if (g_fingers[s].toggle == 2 && g_hd_finger_vk) {
        input_set_vk(g_hd_finger_vk, INPUT_SRC_TOUCH, 0);
        g_hd_finger_vk = 0;
    }
    if (g_fingers[s].button >= 0) press_button(g_fingers[s].button, 0);
    g_fingers[s].used = 0;
}

int overlay_handle_event(const SDL_Event *e, int w, int h) {
    switch (e->type) {
    case SDL_FINGERDOWN:
        return finger_down(e->tfinger.fingerId, e->tfinger.x * w, e->tfinger.y * h, w, h);
    case SDL_FINGERMOTION:
        if (!finger_is_ours(e->tfinger.fingerId)) return 0;
        finger_motion(e->tfinger.fingerId, e->tfinger.x * w, e->tfinger.y * h, w, h);
        return 1;
    case SDL_FINGERUP:
        if (!finger_is_ours(e->tfinger.fingerId)) return 0;
        finger_up(e->tfinger.fingerId);
        return 1;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION: {
        /* desktop testing: the left mouse button acts as a finger */
        if (!getenv("PORT_TOUCH") || e->motion.which == SDL_TOUCH_MOUSEID) return 0;
        int ow, oh, ww, wh;
        SDL_Renderer *r = SDL_GetRenderer(SDL_GetWindowFromID(e->button.windowID));
        if (!r) return 0;
        SDL_GetRendererOutputSize(r, &ow, &oh);
        SDL_GetWindowSize(SDL_GetWindowFromID(e->button.windowID), &ww, &wh);
        float sx = (float)ow / ww, sy = (float)oh / wh;
        if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT)
            finger_down(1000, e->button.x * sx, e->button.y * sy, w, h);
        else if (e->type == SDL_MOUSEBUTTONUP && e->button.button == SDL_BUTTON_LEFT)
            finger_up(1000);
        else if (e->type == SDL_MOUSEMOTION && (e->motion.state & SDL_BUTTON_LMASK))
            finger_motion(1000, e->motion.x * sx, e->motion.y * sy, w, h);
        return 1;
    }
    case SDL_APP_WILLENTERBACKGROUND:
        release_all();
        return 0;
    }
    return 0;
}

/* ---- drawing -------------------------------------------------------------------------------- */
static void draw_tex_centered(SDL_Renderer *r, SDL_Texture *t, float cx, float cy, float maxw, float maxh) {
    int tw, th;
    SDL_QueryTexture(t, NULL, NULL, &tw, &th);
    float s = fminf(maxw / tw, maxh / th);
    if (s > 1.0f) s = floorf(s);
    SDL_FRect d = {cx - tw * s / 2, cy - th * s / 2, tw * s, th * s};
    SDL_RenderCopyF(r, t, NULL, &d);
}

void overlay_draw(SDL_Renderer *r, int w, int h) {
    if (!g_has_touch) return;
    uint8_t alpha = (uint8_t)(255 * g_opacity);
    /* the toggle button: hidden while a controller is used (a touch brings everything
     * back), faint while switched off, since a widescreen game's HUD lies under it */
    if (g_pad_hidden) return;
    SDL_Rect tr;
    toggle_rect(w, h, &tr);
    SDL_Texture *tt = g_enabled ? g_toggle_on : g_toggle_off;
    SDL_SetTextureAlphaMod(tt, g_enabled ? alpha : 60);
    if (g_enabled) {
        SDL_SetRenderDrawColor(r, 0, 0, 0, 90);
        SDL_RenderFillRect(r, &tr);
    }
    draw_tex_centered(r, tt, tr.x + tr.w / 2.0f, tr.y + tr.h / 2.0f, tr.w * 0.8f, tr.h * 0.6f);
    if (!visible()) return;

    /* resolution toggle: lit while the game's frame is wide */
    SDL_Rect hr;
    hd_rect(w, h, &hr);
    int hd = platform_frame_width() >= g_res_wide;
    if (g_hd_label) {
    SDL_Texture *ht = g_hd_label;
    SDL_SetTextureAlphaMod(ht, hd ? alpha : alpha / 2);
    if (hd) SDL_SetRenderDrawColor(r, 60, 140, 60, 170);
    else SDL_SetRenderDrawColor(r, 0, 0, 0, g_hd_finger_vk ? 160 : 70);
    SDL_RenderFillRect(r, &hr);
    draw_tex_centered(r, ht, hr.x + hr.w / 2.0f, hr.y + hr.h / 2.0f, hr.w * 0.8f, hr.h * 0.6f);
    }

    int ctx = current_ctx();
    for (int i = 0; i < NBUTTONS; i++) {
        Button *b = &g_buttons[i];
        if (!(b->ctx & ctx)) continue;
        SDL_FRect rc;
        float cx, cy, rad;
        button_rect(b, w, h, &rc, &cx, &cy, &rad);
        SDL_Texture *d = b->fingers ? g_disc_pressed : g_disc;
        SDL_SetTextureAlphaMod(d, alpha);
        SDL_RenderCopyF(r, d, NULL, &rc);
        SDL_SetTextureAlphaMod(b->tex, alpha);
        draw_tex_centered(r, b->tex, cx, cy, rad * 1.5f, rad * 0.9f);
    }
    /* joystick: at the touch point while held, otherwise a faint hint */
    float br = 0.15f * h, kr = 0.065f * h;
    float bx = g_stick_active ? g_stick_cx : 0.25f * h + br * 0.2f;
    float by = g_stick_active ? g_stick_cy : h - 0.30f * h;
    if (bx < br) bx = br;
    SDL_SetTextureAlphaMod(g_stick_base, g_stick_active ? alpha : alpha / 2);
    SDL_RenderCopyF(r, g_stick_base, NULL, &(SDL_FRect){bx - br, by - br, 2 * br, 2 * br});
    float kx = g_stick_active ? g_stick_x : bx, ky = g_stick_active ? g_stick_y : by;
    SDL_SetTextureAlphaMod(g_stick_knob, g_stick_active ? alpha : alpha / 2);
    SDL_RenderCopyF(r, g_stick_knob, NULL, &(SDL_FRect){kx - kr, ky - kr, 2 * kr, 2 * kr});
}

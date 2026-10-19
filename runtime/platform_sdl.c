/* SDL2 platform layer: window, presentation, events, audio, timing. */
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "platform.h"
#include "gamecfg.h"
#include "runtime.h"
#include "pack/vfs.h"

void user32_set_active(int active);
void user32_post_close(void);
void user32_mouse_event(int x, int y, int button, int type);
void user32_key_event(int vk, int down);
void user32_text_input_active(int active);
void dlg_text_input(const char *utf8);
void overlay_init(SDL_Renderer *r, int w, int h);
void overlay_draw(SDL_Renderer *r, int w, int h);
int overlay_handle_event(const SDL_Event *e, int w, int h);
void gamepad_handle_event(const SDL_Event *e);
void gamepad_init(void);

static void debug_script(void);
static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static uint32_t *g_conv;
static int g_quit;
static int g_active = 1;
static char g_game_dir[1024], g_save_dir[1024];
static uint64_t g_perf_freq, g_perf_start;
static SDL_AudioDeviceID g_audio_dev;
static AudioMixFn g_mix_fn;

const char *platform_game_dir(void) { return g_game_dir; }
/* for code running without platform_init (the on-device packer) */
void platform_set_dirs(const char *game, const char *save) {
    snprintf(g_game_dir, sizeof g_game_dir, "%s", game);
    snprintf(g_save_dir, sizeof g_save_dir, "%s", save);
}
const char *platform_save_dir(void) { return g_save_dir; }

static int dir_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

#ifdef __ANDROID__
#include <jni.h>
/* path of the installed APK (Context.getPackageCodePath) */
static int android_apk_path(char *out, size_t n) {
    JNIEnv *env = SDL_AndroidGetJNIEnv();
    jobject act = SDL_AndroidGetActivity();
    if (!env || !act) return -1;
    int ret = -1;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetMethodID(env, cls, "getPackageCodePath", "()Ljava/lang/String;");
    jstring s = m ? (jstring)(*env)->CallObjectMethod(env, act, m) : NULL;
    if (s) {
        const char *c = (*env)->GetStringUTFChars(env, s, NULL);
        if (c) {
            snprintf(out, n, "%s", c);
            (*env)->ReleaseStringUTFChars(env, s, c);
            ret = 0;
        }
        (*env)->DeleteLocalRef(env, s);
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    return ret;
}
#endif

/* The game files can come from a pack (game.pak): inside the APK on Android,
 * otherwise $PORT_PACK or <game dir>/game.pak. */
static void open_pack(void) {
    char path[1200];
    uint64_t off = 0, size = 0;
#ifdef __ANDROID__
    if (android_apk_path(path, sizeof path) == 0 &&
        vfs_zip_find_stored(path, "assets/game.pak", &off, &size) == 0)
        vfs_pack_open(path, off);
#endif
    if (vfs_have_pack()) return;
    const char *env = getenv("PORT_PACK");
    if (env) snprintf(path, sizeof path, "%s", env);
    else snprintf(path, sizeof path, "%s/game.pak", g_game_dir);
    if (vfs_zip_find_stored(path, "assets/game.pak", &off, &size) == 0) vfs_pack_open(path, off);
    else vfs_pack_open(path, 0);
}

/* Files the port itself supplies (e.g. the General MIDI soundfont): assets/port/<name>
 * stored in the APK on Android; on desktop $PORT_DATA/<name> or port_data/<name> next
 * to the executable. Returns a malloc'd buffer. */
static char g_exe_dir[1024];
void *platform_read_port_file(const char *name, int64_t *size) {
    char path[1400];
    FILE *f = NULL;
    uint64_t off = 0, n = 0;
#ifdef __ANDROID__
    char rel[300];
    snprintf(rel, sizeof rel, "assets/port/%s", name);
    if (android_apk_path(path, sizeof path) == 0 && vfs_zip_find_stored(path, rel, &off, &n) == 0) f = fopen(path, "rb");
#endif
    if (!f) {
        const char *env = getenv("PORT_DATA");
        snprintf(path, sizeof path, "%s/%s", env ? env : g_exe_dir, name);
        if (!env && !(f = fopen(path, "rb"))) snprintf(path, sizeof path, "%s/port_data/%s", g_exe_dir, name);
        if (!f) f = fopen(path, "rb");
        if (!f) return NULL;
        fseek(f, 0, SEEK_END);
        n = (uint64_t)ftell(f);
        off = 0;
    }
    void *buf = malloc(n ? n : 1);
    fseek(f, (long)off, SEEK_SET);
    if (!buf || fread(buf, 1, n, f) != n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (int64_t)n;
    return buf;
}

int platform_init(int argc, char **argv) {
    {
        const char *b = SDL_GetBasePath();
        if (b) { snprintf(g_exe_dir, sizeof g_exe_dir, "%s", b); SDL_free((void *)b); }
        size_t l = strlen(g_exe_dir);
        if (l && g_exe_dir[l - 1] == '/') g_exe_dir[l - 1] = 0;
    }
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
#ifdef __ANDROID__
    const char *ext = SDL_AndroidGetExternalStoragePath();
    snprintf(g_game_dir, sizeof g_game_dir, "%s/game", ext ? ext : ".");
    snprintf(g_save_dir, sizeof g_save_dir, "%s/save", ext ? ext : ".");
    (void)argc; (void)argv;
#else
    const char *gd = argc > 1 ? argv[1] : getenv("PORT_GAME_DIR");
    const char *sd = argc > 2 ? argv[2] : getenv("PORT_SAVE_DIR");
    snprintf(g_game_dir, sizeof g_game_dir, "%s", gd ? gd : "game");
    snprintf(g_save_dir, sizeof g_save_dir, "%s", sd ? sd : "save");
#endif
    mkdir(g_save_dir, 0755);
    cfg_init();
    input_init();
    open_pack();
    if (!vfs_have_pack() && !dir_exists(g_game_dir)) {
        char msg[1400];
        snprintf(msg, sizeof msg,
                 "Game files not found.\n\nCopy the contents of the Windows %s folder "
                 "(%s) to:\n%s", cfg_str("game", "short_name", "game"),
                 cfg_str("game", "files_hint", cfg_str("game", "exe", "game.exe")), g_game_dir);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, cfg_str("game", "short_name", "Game"), msg, NULL);
        return -1;
    }

    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#ifdef __ANDROID__
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
    g_window = SDL_CreateWindow(cfg_str("game", "title", "Game"), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                960, 720, flags);
    if (!g_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, 0);
    if (!g_renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_RendererInfo ri;
    if (SDL_GetRendererInfo(g_renderer, &ri) == 0) fprintf(stderr, "renderer: %s\n", ri.name);
    g_perf_freq = SDL_GetPerformanceFrequency();
    g_perf_start = SDL_GetPerformanceCounter();
    int w, h;
    SDL_GetRendererOutputSize(g_renderer, &w, &h);
    overlay_init(g_renderer, w, h);
    gamepad_init();
    SDL_StopTextInput(); /* on only while an edit control has the focus */
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderPresent(g_renderer);
    return 0;
}

void platform_shutdown(void) {
    if (g_audio_dev) SDL_CloseAudioDevice(g_audio_dev);
    SDL_Quit();
}

/* ---- time -------------------------------------------------------------------------- */
uint64_t platform_ticks_us(void) {
    uint64_t t = SDL_GetPerformanceCounter() - g_perf_start;
    return (uint64_t)((double)t * 1000000.0 / (double)g_perf_freq);
}
uint32_t platform_ticks_ms(void) { return (uint32_t)(platform_ticks_us() / 1000); }
void platform_sleep_ms(uint32_t ms) { SDL_Delay(ms); }

/* ---- keyboard ------------------------------------------------------------------------ */
static int scancode_to_vk(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 0x70 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return 0x61 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0d;
    case SDL_SCANCODE_ESCAPE: return 0x1b;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_INSERT: return 0x2d;
    case SDL_SCANCODE_DELETE: return 0x2e;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_SEMICOLON: return 0xba;
    case SDL_SCANCODE_EQUALS: return 0xbb;
    case SDL_SCANCODE_COMMA: return 0xbc;
    case SDL_SCANCODE_MINUS: return 0xbd;
    case SDL_SCANCODE_PERIOD: return 0xbe;
    case SDL_SCANCODE_SLASH: return 0xbf;
    default: return 0;
    }
}

static void set_active(int a) {
    if (a == g_active) return;
    g_active = a;
    if (!a) {
        input_clear_source(INPUT_SRC_KEYBOARD);
        input_clear_source(INPUT_SRC_TOUCH);
        input_clear_source(INPUT_SRC_PAD);
    }
    if (g_audio_dev) SDL_PauseAudioDevice(g_audio_dev, !a);
    user32_set_active(a);
}

static SDL_Rect g_game_rect;
static int g_tex_w, g_tex_h;

/* renderer output pixels -> game frame pixels; nonzero when outside the picture */
static int window_to_game(int x, int y, int *gx, int *gy) {
    if (!g_game_rect.w || !g_game_rect.h || !g_tex_w) { *gx = x; *gy = y; return 0; }
    *gx = (int)((x - g_game_rect.x) * (long)g_tex_w / g_game_rect.w);
    *gy = (int)((y - g_game_rect.y) * (long)g_tex_h / g_game_rect.h);
    int out = *gx < 0 || *gy < 0 || *gx >= g_tex_w || *gy >= g_tex_h;
    if (*gx < 0) *gx = 0;
    if (*gy < 0) *gy = 0;
    if (*gx >= g_tex_w) *gx = g_tex_w - 1;
    if (*gy >= g_tex_h) *gy = g_tex_h - 1;
    return out;
}

int cheatmenu_handle_event(const SDL_Event *e, int w, int h);
void cheatmenu_toggle(void);
void cheatmenu_run(void);
static void handle_event(const SDL_Event *e) {
    int w, h;
    SDL_GetRendererOutputSize(g_renderer, &w, &h);
    if (cheatmenu_handle_event(e, w, h)) return;
    if (e->type == SDL_KEYDOWN && e->key.keysym.scancode == SDL_SCANCODE_F12 && !e->key.repeat) {
        cheatmenu_toggle();
        return;
    }
    if (overlay_handle_event(e, w, h)) return;
    switch (e->type) {
    case SDL_QUIT:
        g_quit = 1;
        user32_post_close();
        break;
    case SDL_APP_WILLENTERBACKGROUND:
    case SDL_APP_DIDENTERBACKGROUND:
        set_active(0);
        break;
    case SDL_APP_DIDENTERFOREGROUND:
        set_active(1);
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) set_active(0);
        else if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) set_active(1);
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (e->key.keysym.scancode == SDL_SCANCODE_AC_BACK) {
            input_set_vk(0x1b, INPUT_SRC_KEYBOARD, e->type == SDL_KEYDOWN);
            break;
        }
        int vk = scancode_to_vk(e->key.keysym.scancode);
        if (!vk) break;
        if (e->key.repeat) { if (e->type == SDL_KEYDOWN) user32_key_event(vk, 1); break; }
        input_set_vk(vk, INPUT_SRC_KEYBOARD, e->type == SDL_KEYDOWN);
        break;
    }
    case SDL_TEXTINPUT:
        dlg_text_input(e->text.text);
        break;
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        if (e->type == SDL_MOUSEMOTION ? e->motion.which == SDL_TOUCH_MOUSEID : e->button.which == SDL_TOUCH_MOUSEID) break;
        int mx = e->type == SDL_MOUSEMOTION ? e->motion.x : e->button.x, my = e->type == SDL_MOUSEMOTION ? e->motion.y : e->button.y;
        int ww, wh, gx, gy;
        SDL_GetWindowSize(g_window, &ww, &wh);
        if (ww > 0 && wh > 0) { mx = (int)((long)mx * w / ww); my = (int)((long)my * h / wh); } /* HiDPI */
        if (window_to_game(mx, my, &gx, &gy) && e->type == SDL_MOUSEBUTTONDOWN) break;
        int btn = e->type == SDL_MOUSEMOTION ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 0;
        if (e->type != SDL_MOUSEMOTION && e->button.button != SDL_BUTTON_LEFT && e->button.button != SDL_BUTTON_RIGHT) break;
        user32_mouse_event(gx, gy, btn, e->type == SDL_MOUSEMOTION ? 0 : e->type == SDL_MOUSEBUTTONDOWN ? 1 : 2);
        break;
    }
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION: {
        /* touches the overlay did not take act as the mouse (dialogs, menus) */
        static SDL_FingerID mouse_finger = -1;
        if (e->type == SDL_FINGERDOWN && mouse_finger == (SDL_FingerID)-1) mouse_finger = e->tfinger.fingerId;
        if (e->tfinger.fingerId != mouse_finger) break;
        int gx, gy;
        window_to_game((int)(e->tfinger.x * w), (int)(e->tfinger.y * h), &gx, &gy);
        user32_mouse_event(gx, gy, 0, e->type == SDL_FINGERMOTION ? 0 : e->type == SDL_FINGERDOWN ? 1 : 2);
        if (e->type == SDL_FINGERUP) mouse_finger = -1;
        break;
    }
    default:
        gamepad_handle_event(e);
        break;
    }
}

void widescreen_tick(void);
void platform_pump_events(void) {
    debug_script();
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle_event(&e);
    cheatmenu_run();
    widescreen_tick();
}

/* width / height of the window (the whole screen on phones) */
float platform_display_aspect(void) {
    int w = 0, h = 0;
    if (g_window) SDL_GetWindowSize(g_window, &w, &h);
    return w > 0 && h > 0 ? (float)w / (float)h : 4.0f / 3.0f;
}

void platform_debug_tick(void) { debug_script(); }
int platform_route_event(const SDL_Event *e) {
    handle_event(e);
    return 1;
}

/* the last game frame again, with something drawn over it (the cheat menu) */
void platform_redraw_last_frame(void (*over)(SDL_Renderer *, int, int)) {
    int ow, oh;
    SDL_GetRendererOutputSize(g_renderer, &ow, &oh);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    if (g_texture) SDL_RenderCopy(g_renderer, g_texture, NULL, &g_game_rect);
    over(g_renderer, ow, oh);
    static const char *sdump = (const char *)-1;
    static int n;
    if (sdump == (const char *)-1) sdump = getenv("PORT_DUMP_SCREEN");
    if (sdump && ++n % 30 == 0) {
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_RenderReadPixels(g_renderer, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
        char path[1200];
        snprintf(path, sizeof path, "%s/menu_%05d.bmp", sdump, n);
        SDL_SaveBMP(s, path);
        SDL_FreeSurface(s);
    }
    SDL_RenderPresent(g_renderer);
}

void platform_wait_events(int timeout_ms) {
    SDL_Event e;
    if (SDL_WaitEventTimeout(&e, timeout_ms)) handle_event(&e);
    platform_pump_events();
}

int platform_quit_requested(void) { return g_quit; }
int platform_app_active(void) { return g_active; }

/* ---- presentation ------------------------------------------------------------------------ */

int platform_frame_width(void) { return g_tex_w; }

void platform_get_game_rect(int *x, int *y, int *w, int *h) {
    *x = g_game_rect.x; *y = g_game_rect.y; *w = g_game_rect.w; *h = g_game_rect.h;
}

/* 16-bit -> ARGB8888 lookup tables (one table read per pixel). */
static uint32_t *g_lut565, *g_lut555;

static void build_luts(void) {
    g_lut565 = malloc(65536 * 4);
    g_lut555 = malloc(65536 * 4);
    for (uint32_t p = 0; p < 65536; p++) {
        uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
        g_lut565[p] = 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        r = (p >> 10) & 31; g = (p >> 5) & 31;
        g_lut555[p] = 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
    }
}

static void convert_frame(const void *pixels, int w, int h, int pitch, int format) {
    if (!g_lut565) build_luts();
    const uint32_t *lut = format == PIX_RGB565 ? g_lut565 : g_lut555;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)pixels + (ptrdiff_t)y * pitch;
        uint32_t *out = g_conv + y * w;
        if (format == PIX_XRGB8888) {
            const uint32_t *s = (const uint32_t *)row;
            for (int x = 0; x < w; x++) out[x] = s[x] | 0xff000000u;
        } else {
            const uint16_t *s = (const uint16_t *)row;
            for (int x = 0; x < w; x++) out[x] = lut[s[x]];
        }
    }
}

/* Debug aids: PORT_DUMP_FRAMES=<dir> saves every 30th frame as BMP;
 * PORT_SCRIPT="ms:vk:down,..." injects key events at given times. */
static int g_frame_no;
static void debug_dump_frame(int w, int h) {
    static const char *dir = (const char *)-1;
    if (dir == (const char *)-1) dir = getenv("PORT_DUMP_FRAMES");
    if (!dir || (g_frame_no % 30) != 0) return;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(g_conv, w, h, 32, w * 4, SDL_PIXELFORMAT_ARGB8888);
    char path[1200];
    snprintf(path, sizeof path, "%s/frame_%05d.bmp", dir, g_frame_no);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}

static SDL_Joystick *g_vpad;

static void debug_script(void) {
    static char *script = (char *)-1;
    static char *pos;
    if (script == (char *)-1) {
        const char *e = getenv("PORT_SCRIPT");
        script = e ? strdup(e) : NULL;
        pos = script;
        if (script && strstr(script, "pad:")) {
            /* a virtual SDL game controller, to exercise gamepad.c without hardware */
            SDL_VirtualJoystickDesc d = {SDL_VIRTUAL_JOYSTICK_DESC_VERSION};
            d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
            d.naxes = SDL_CONTROLLER_AXIS_MAX;
            d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
            d.name = "Scripted virtual pad";
            int idx = SDL_JoystickAttachVirtualEx(&d);
            g_vpad = idx >= 0 ? SDL_JoystickOpen(idx) : NULL;
            rt_log("virtual pad %s", g_vpad ? "attached" : SDL_GetError());
        }
    }
    while (pos && *pos) {
        unsigned ms, a, b;
        int n = 0, v;
        int mx, my;
        if (sscanf(pos, "%u:mouse:%d:%d:%u%n", &ms, &mx, &my, &b, &n) == 4) {
            /* game coordinates; b: 0 move, 1 down, 2 up */
            if (platform_ticks_ms() < ms) break;
            rt_log("script: mouse %d,%d %u", mx, my, b);
            user32_mouse_event(mx, my, 0, (int)b);
        } else if (sscanf(pos, "%u:pad:%u:%u%n", &ms, &a, &b, &n) == 3) {
            if (platform_ticks_ms() < ms) break;
            rt_log("script: pad button %u %s", a, b ? "down" : "up");
            if (g_vpad) SDL_JoystickSetVirtualButton(g_vpad, (int)a, (Uint8)b);
        } else if (sscanf(pos, "%u:axis:%u:%d%n", &ms, &a, &v, &n) == 3) {
            if (platform_ticks_ms() < ms) break;
            rt_log("script: pad axis %u = %d", a, v);
            if (g_vpad) SDL_JoystickSetVirtualAxis(g_vpad, (int)a, (Sint16)v);
        } else if (sscanf(pos, "%u:%x:%u%n", &ms, &a, &b, &n) == 3) {
            if (platform_ticks_ms() < ms) break;
            rt_log("script: vk %02x %s", a, b ? "down" : "up");
            int cheatmenu_is_open(void);
            if (cheatmenu_is_open()) { /* the menu reads SDL events: send it a key */
                SDL_Keycode k = a == 0x26 ? SDLK_UP : a == 0x28 ? SDLK_DOWN : a == 0x25 ? SDLK_LEFT : a == 0x27 ? SDLK_RIGHT :
                                a == 0x0d ? SDLK_RETURN : a == 0x1b ? SDLK_ESCAPE : a == 0x09 ? SDLK_TAB :
                                (a >= 'A' && a <= 'Z') ? (SDL_Keycode)(a + 32) : SDLK_UNKNOWN;
                SDL_Event ev = {0};
                ev.type = b ? SDL_KEYDOWN : SDL_KEYUP;
                ev.key.keysym.sym = k;
                SDL_PushEvent(&ev);
                if (b && a >= 'A' && a <= 'Z') {
                    SDL_Event t = {0};
                    t.type = SDL_TEXTINPUT;
                    t.text.text[0] = (char)a;
                    SDL_PushEvent(&t);
                }
            } else
            input_set_vk((int)a, INPUT_SRC_KEYBOARD, (int)b);
        } else {
            pos = NULL;
            break;
        }
        pos += n;
        if (*pos == ',') pos++;
    }
}

static uint64_t g_present_us;
void platform_present(const void *pixels, int w, int h, int pitch, int format) {
    uint64_t t_start = platform_ticks_us();
    g_frame_no++;
    static uint32_t last_stat;
    static int last_frames;
    static uint64_t last_sleep;
    extern uint64_t g_sleep_us;
    if (platform_ticks_ms() - last_stat > 15000) {
        uint32_t now = platform_ticks_ms();
        int frames = g_frame_no - last_frames;
        double busy = (now - last_stat) - (double)(g_sleep_us - last_sleep) / 1000.0;
        if (last_stat)
            rt_log("perf: %.1f fps, %.1f ms busy per frame (%.1f ms present), %dx%d, %s", frames * 1000.0 / (now - last_stat),
                   frames ? busy / frames : 0.0, frames ? g_present_us / 1000.0 / frames : 0.0, w, h,
                   input_in_gameplay() ? "gameplay" : "menu");
        g_present_us = 0;
        last_stat = now;
        last_frames = g_frame_no;
        last_sleep = g_sleep_us;
    }
    if (!g_texture || w != g_tex_w || h != g_tex_h) {
        if (g_texture) SDL_DestroyTexture(g_texture);
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!g_texture) rt_log("SDL_CreateTexture failed: %s", SDL_GetError());
        g_tex_w = w;
        g_tex_h = h;
        free(g_conv);
        g_conv = malloc((size_t)w * h * 4);
        rt_log("present: %dx%d format %d", w, h, format);
    }
    convert_frame(pixels, w, h, pitch, format);
    debug_dump_frame(w, h);
    if (SDL_UpdateTexture(g_texture, NULL, g_conv, w * 4) != 0) {
        static int once;
        if (!once++) rt_log("SDL_UpdateTexture failed: %s", SDL_GetError());
    }

    int ow, oh;
    SDL_GetRendererOutputSize(g_renderer, &ow, &oh);
    /* letterbox to the frame's aspect (pixels are square in every mode but 320x200-type ones) */
    int dw = ow, dh = (int)((long)ow * h / w);
    if (dh > oh) { dh = oh; dw = (int)((long)oh * w / h); }
    g_game_rect = (SDL_Rect){(ow - dw) / 2, (oh - dh) / 2, dw, dh};
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, &g_game_rect);
    overlay_draw(g_renderer, ow, oh);
    static const char *sdump = (const char *)-1;
    if (sdump == (const char *)-1) sdump = getenv("PORT_DUMP_SCREEN");
    static int severy = -1;
    if (severy < 0) severy = getenv("PORT_DUMP_EVERY") ? atoi(getenv("PORT_DUMP_EVERY")) : 30;
    static int safter = -1;
    if (safter < 0) safter = getenv("PORT_DUMP_AFTER_MS") ? atoi(getenv("PORT_DUMP_AFTER_MS")) : 0;
    if (sdump && severy > 0 && g_frame_no % severy == 0 && platform_ticks_ms() >= (uint32_t)safter) {
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_RenderReadPixels(g_renderer, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
        char path[1200];
        snprintf(path, sizeof path, "%s/screen_%05d.bmp", sdump, g_frame_no);
        SDL_SaveBMP(s, path);
        SDL_FreeSurface(s);
    }
    SDL_RenderPresent(g_renderer);
    g_present_us += platform_ticks_us() - t_start;
}

/* ---- audio ----------------------------------------------------------------------------------- */
static void sdl_audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    if (g_mix_fn) g_mix_fn((int16_t *)stream, len / 4);
    else memset(stream, 0, len);
}

/* No usable audio device (or SDL's dummy driver, which never pulls samples): a thread
 * mixes into a scratch buffer at real-time pace, so sound buffers still play to the end
 * (the game waits for some of them, e.g. the TITLE voice before the menu takes keys). */
static SDL_mutex *g_silent_lock;
static int g_silent_rate;
static int silent_audio_thread(void *ud) {
    (void)ud;
    static int16_t buf[1024 * 2];
    uint64_t t = platform_ticks_us();
    for (;;) {
        SDL_LockMutex(g_silent_lock);
        g_mix_fn(buf, 1024);
        SDL_UnlockMutex(g_silent_lock);
        t += 1024ull * 1000000 / (uint64_t)g_silent_rate;
        int64_t wait = (int64_t)(t - platform_ticks_us());
        if (wait > 0) SDL_Delay((Uint32)(wait / 1000));
    }
    return 0;
}
static int silent_audio_start(int rate) {
    g_silent_rate = rate;
    g_silent_lock = SDL_CreateMutex();
    SDL_CreateThread(silent_audio_thread, "silent-audio", NULL);
    rt_log("audio: no output device, mixing silently at %d Hz", rate);
    return rate;
}

int platform_audio_open(int rate, AudioMixFn fn) {
    SDL_AudioSpec want = {0}, have;
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
#ifdef __ANDROID__
    want.samples = 2048;  /* phones underrun (crackle) with smaller buffers */
#else
    want.samples = 1024;
#endif
    want.callback = sdl_audio_cb;
    g_mix_fn = fn;
    const char *drv = SDL_GetCurrentAudioDriver();
    if (drv && !strcmp(drv, "dummy")) return silent_audio_start(rate);
    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_audio_dev) {
        rt_log("SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return silent_audio_start(rate);
    }
    rt_log("audio: %d Hz, %d ch, %d samples, driver %s", have.freq, have.channels, have.samples, SDL_GetCurrentAudioDriver());
    SDL_PauseAudioDevice(g_audio_dev, 0);
    return have.freq;
}
void platform_audio_lock(void) {
    if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
    else if (g_silent_lock) SDL_LockMutex(g_silent_lock);
}
void platform_audio_unlock(void) {
    if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
    else if (g_silent_lock) SDL_UnlockMutex(g_silent_lock);
}

void platform_fatal_message(const char *msg) {
    if (getenv("PORT_HEADLESS")) return; /* automated runs: no blocking dialog */
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "fatal error", msg, g_window);
}

int platform_message_box(const char *caption, const char *text) {
    return SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, caption, text, g_window);
}

/* MessageBox with the Win32 button sets; returns IDOK/IDCANCEL/IDYES/... */
int platform_message_box_ex(const char *caption, const char *text, uint32_t type) {
    static const struct { int n; int ids[3]; const char *names[3]; } sets[6] = {
        {1, {1}, {"OK"}},
        {2, {1, 2}, {"OK", "Cancel"}},
        {3, {3, 4, 5}, {"Abort", "Retry", "Ignore"}},
        {3, {6, 7, 2}, {"Yes", "No", "Cancel"}},
        {2, {6, 7}, {"Yes", "No"}},
        {2, {4, 2}, {"Retry", "Cancel"}}};
    int k = (int)(type & 0xf);
    if (k > 5) k = 0;
    int def = (int)((type >> 8) & 0xf);
    if (getenv("PORT_HEADLESS")) return sets[k].ids[def < sets[k].n ? def : 0];
    SDL_MessageBoxButtonData b[3];
    for (int i = 0; i < sets[k].n; i++) {
        b[i].flags = i == def ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
        if (sets[k].ids[i] == 2 || (sets[k].n == 1)) b[i].flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
        b[i].buttonid = sets[k].ids[i];
        b[i].text = sets[k].names[i];
    }
    uint32_t icon = type & 0xf0;
    SDL_MessageBoxData d = {icon == 0x10 ? SDL_MESSAGEBOX_ERROR : icon == 0x30 ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION,
                            g_window, caption, text, sets[k].n, b, NULL};
    int hit = sets[k].ids[0];
    if (SDL_ShowMessageBox(&d, &hit) != 0 || hit < 0) hit = sets[k].ids[sets[k].n - 1];
    return hit;
}

/* text input while an edit control has the focus: 1 = tapped/clicked, 2 = focus only.
 * On Android text input means the on-screen keyboard, which would cover the dialog, so
 * it comes up only when the field is tapped. */
void platform_text_input(int on) {
#ifdef __ANDROID__
    if (on == 2) { user32_text_input_active(1); return; }
#endif
    if (on) SDL_StartTextInput();
    else SDL_StopTextInput();
    user32_text_input_active(on);
}

/* Windows virtual-key state aggregated from all input sources. */
#include <string.h>
#include <stdint.h>
#include "platform.h"
#include "gamecfg.h"

/* Games sample keys once per frame; a quick tap could fall between two samples,
 * so every press is held for at least [input] min_hold_ms. */
static uint32_t g_min_hold_ms = 80;

static unsigned char g_vk[INPUT_SRC_COUNT][256];
static uint32_t g_pressed_at[INPUT_SRC_COUNT][256];
static unsigned char g_release_pending[INPUT_SRC_COUNT][256];
/* key messages (WM_KEYDOWN/UP) follow the sources directly, without the minimum hold */
static unsigned char g_held[INPUT_SRC_COUNT][256];
static unsigned char g_msg_down[256];
void user32_key_event(int vk, int down);

void input_set_vk(int vk, int source, int down) {
    if (vk <= 0 || vk > 255 || source < 0 || source >= INPUT_SRC_COUNT) return;
    if (vk == PORT_VK_CHEATS) { /* port action, never seen by the game */
        void cheatmenu_toggle(void);
        if (down) cheatmenu_toggle();
        return;
    }
    g_held[source][vk] = (unsigned char)(down != 0);
    int any = 0;
    for (int s = 0; s < INPUT_SRC_COUNT; s++) any |= g_held[s][vk];
    if (any != g_msg_down[vk]) {
        g_msg_down[vk] = (unsigned char)any;
        user32_key_event(vk, any);
    }
    if (down) {
        g_vk[source][vk] = 1;
        g_pressed_at[source][vk] = platform_ticks_ms();
        g_release_pending[source][vk] = 0;
    } else if (g_vk[source][vk]) {
        if (platform_ticks_ms() - g_pressed_at[source][vk] < g_min_hold_ms) g_release_pending[source][vk] = 1;
        else g_vk[source][vk] = 0;
    }
}

int input_vk_down(int vk) {
    if (vk <= 0 || vk > 255) return 0;
    int down = 0;
    for (int s = 0; s < INPUT_SRC_COUNT; s++) {
        if (g_release_pending[s][vk] && platform_ticks_ms() - g_pressed_at[s][vk] >= g_min_hold_ms) {
            g_release_pending[s][vk] = 0;
            g_vk[s][vk] = 0;
        }
        if (g_vk[s][vk]) down = 1;
    }
    return down;
}

void input_clear_source(int source) {
    if (source < 0 || source >= INPUT_SRC_COUNT) return;
    for (int vk = 1; vk < 256; vk++)
        if (g_held[source][vk]) input_set_vk(vk, source, 0);
    memset(g_vk[source], 0, 256);
    memset(g_release_pending[source], 0, 256);
}

/* Menu vs gameplay: many games poll their action keys only while the player
 * has control, so recent polls of [input] gameplay_poll keys mean "gameplay";
 * otherwise we are in a menu.  With no such keys configured: always gameplay. */
static unsigned g_last_game_poll;
static unsigned char g_game_poll_vk[256];
static int g_have_game_poll;
static uint32_t g_game_window_ms = 500;

void input_init(void) {
    g_min_hold_ms = (uint32_t)cfg_int("input", "min_hold_ms", 80);
    g_game_window_ms = (uint32_t)cfg_int("input", "gameplay_window_ms", 500);
    int vks[32];
    int n = cfg_vk_list(cfg_str("input", "gameplay_poll", ""), vks, 32);
    for (int i = 0; i < n; i++) g_game_poll_vk[vks[i]] = 1;
    g_have_game_poll = n > 0;
}

void input_note_poll(int vk) {
    if (vk > 0 && vk < 256 && g_game_poll_vk[vk]) g_last_game_poll = platform_ticks_ms();
}

int input_in_gameplay(void) {
    /* a dialog or other GDI window on screen is always "menu" */
    extern int wm_gdi_overlay_active(void);
    if (wm_gdi_overlay_active()) return 0;
    if (!g_have_game_poll) return 1;
    return g_last_game_poll && platform_ticks_ms() - g_last_game_poll < g_game_window_ms;
}

/* [input] dirs: the keys for up, down, left, right. */
void input_dir_vks(int out[4]) {
    static int dirs[4], loaded;
    if (!loaded) {
        if (cfg_vk_list(cfg_str("input", "dirs", "UP DOWN LEFT RIGHT"), dirs, 4) != 4) {
            dirs[0] = 0x26; dirs[1] = 0x28; dirs[2] = 0x25; dirs[3] = 0x27;
        }
        loaded = 1;
    }
    memcpy(out, dirs, sizeof dirs);
}

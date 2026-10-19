/* Physical game controllers (SDL GameController API: Xbox, PlayStation, generic
 * Android/Bluetooth pads).  Buttons are translated to the game's keyboard keys,
 * with separate maps for gameplay and menus (game.ini [pad.game], [pad.menu]). */
#include <SDL.h>
#include "platform.h"
#include "runtime.h"

#include "gamecfg.h"

void overlay_set_gamepad_active(int active);

/* Button -> VK maps for gameplay and menus, from game.ini [pad.game] / [pad.menu].
 * The triggers are analog axes but configured like buttons. */
enum { TRIG_L = SDL_CONTROLLER_BUTTON_MAX, TRIG_R, NMAP };
static int g_game_map[NMAP], g_menu_map[NMAP];
static int g_dirs[4]; /* up down left right */

static const struct { const char *name; int index; } PAD_NAMES[] = {
    {"a", SDL_CONTROLLER_BUTTON_A}, {"b", SDL_CONTROLLER_BUTTON_B}, {"x", SDL_CONTROLLER_BUTTON_X},
    {"y", SDL_CONTROLLER_BUTTON_Y}, {"back", SDL_CONTROLLER_BUTTON_BACK}, {"guide", SDL_CONTROLLER_BUTTON_GUIDE},
    {"start", SDL_CONTROLLER_BUTTON_START}, {"leftstick", SDL_CONTROLLER_BUTTON_LEFTSTICK},
    {"rightstick", SDL_CONTROLLER_BUTTON_RIGHTSTICK}, {"leftshoulder", SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    {"rightshoulder", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}, {"dpup", SDL_CONTROLLER_BUTTON_DPAD_UP},
    {"dpdown", SDL_CONTROLLER_BUTTON_DPAD_DOWN}, {"dpleft", SDL_CONTROLLER_BUTTON_DPAD_LEFT},
    {"dpright", SDL_CONTROLLER_BUTTON_DPAD_RIGHT}, {"lefttrigger", TRIG_L}, {"righttrigger", TRIG_R},
};

static void load_map(const char *section, int *map) {
    /* the d-pad sends the directions unless configured otherwise */
    map[SDL_CONTROLLER_BUTTON_DPAD_UP] = g_dirs[0];
    map[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = g_dirs[1];
    map[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = g_dirs[2];
    map[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = g_dirs[3];
    for (size_t i = 0; i < sizeof PAD_NAMES / sizeof PAD_NAMES[0]; i++) {
        const char *v = cfg_str(section, PAD_NAMES[i].name, NULL);
        if (v) map[PAD_NAMES[i].index] = cfg_vk(v);
    }
}

/* Each held button/axis remembers the VK it pressed, so a context switch while
 * holding cannot leave a key stuck. Counts allow several sources per VK. */
static int g_held_vk[SDL_CONTROLLER_BUTTON_MAX];
static int g_vk_count[256];
static int g_axis_vk[SDL_CONTROLLER_AXIS_MAX][2];

static void vk_ref(int vk, int down) {
    if (!vk) return;
    if (down) {
        if (g_vk_count[vk]++ == 0) input_set_vk(vk, INPUT_SRC_PAD, 1);
    } else if (g_vk_count[vk] > 0 && --g_vk_count[vk] == 0) {
        input_set_vk(vk, INPUT_SRC_PAD, 0);
    }
}

void gamepad_init(void) {
    input_dir_vks(g_dirs);
    load_map("pad.game", g_game_map);
    load_map("pad.menu", g_menu_map);
    char path[1200];
    snprintf(path, sizeof path, "%s/gamecontrollerdb.txt", platform_save_dir());
    if (SDL_GameControllerAddMappingsFromFile(path) > 0) rt_log("loaded extra controller mappings from %s", path);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
}

static void axis_dir(int axis, int value, int neg_vk, int pos_vk) {
    /* hysteresis: press at 50%, release at 35% */
    int *st = g_axis_vk[axis];
    int want_neg = st[0] ? value < -11500 : value < -16384;
    int want_pos = st[1] ? value > 11500 : value > 16384;
    if (want_neg != (st[0] != 0)) {
        if (want_neg) { st[0] = neg_vk; vk_ref(neg_vk, 1); } else { vk_ref(st[0], 0); st[0] = 0; }
    }
    if (want_pos != (st[1] != 0)) {
        if (want_pos) { st[1] = pos_vk; vk_ref(pos_vk, 1); } else { vk_ref(st[1], 0); st[1] = 0; }
    }
}

static void trigger(int axis, int value, int index) {
    int *st = g_axis_vk[axis];
    int want = st[0] ? value > 8000 : value > 16000;
    if (want != (st[0] != 0)) {
        if (want) {
            int vk = input_in_gameplay() ? g_game_map[index] : g_menu_map[index];
            st[0] = vk ? vk : -1;
            vk_ref(vk, 1);
        } else {
            if (st[0] > 0) vk_ref(st[0], 0);
            st[0] = 0;
        }
    }
}

/* forget held buttons (the cheat menu took the controller) */
void gamepad_release_all(void) {
    input_clear_source(INPUT_SRC_PAD);
    memset(g_held_vk, 0, sizeof g_held_vk);
    memset(g_vk_count, 0, sizeof g_vk_count);
    memset(g_axis_vk, 0, sizeof g_axis_vk);
}

void gamepad_handle_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_CONTROLLERDEVICEADDED:
        if (SDL_GameControllerOpen(e->cdevice.which))
            rt_log("controller connected: %s", SDL_GameControllerNameForIndex(e->cdevice.which));
        break;
    case SDL_CONTROLLERDEVICEREMOVED: {
        SDL_GameController *c = SDL_GameControllerFromInstanceID(e->cdevice.which);
        if (c) SDL_GameControllerClose(c);
        rt_log("controller disconnected");
        input_clear_source(INPUT_SRC_PAD);
        memset(g_held_vk, 0, sizeof g_held_vk);
        memset(g_vk_count, 0, sizeof g_vk_count);
        memset(g_axis_vk, 0, sizeof g_axis_vk);
        overlay_set_gamepad_active(0);
        break;
    }
    case SDL_CONTROLLERBUTTONDOWN: {
        int b = e->cbutton.button;
        if (b < 0 || b >= SDL_CONTROLLER_BUTTON_MAX || g_held_vk[b]) break;
        overlay_set_gamepad_active(1);
        int vk = input_in_gameplay() ? g_game_map[b] : g_menu_map[b];
        g_held_vk[b] = vk ? vk : -1;
        vk_ref(vk, 1);
        break;
    }
    case SDL_CONTROLLERBUTTONUP: {
        int b = e->cbutton.button;
        if (b < 0 || b >= SDL_CONTROLLER_BUTTON_MAX) break;
        if (g_held_vk[b] > 0) vk_ref(g_held_vk[b], 0);
        g_held_vk[b] = 0;
        break;
    }
    case SDL_CONTROLLERAXISMOTION: {
        int a = e->caxis.axis, v = e->caxis.value;
        if (a == SDL_CONTROLLER_AXIS_LEFTX) axis_dir(a, v, g_dirs[2], g_dirs[3]);
        else if (a == SDL_CONTROLLER_AXIS_LEFTY) axis_dir(a, v, g_dirs[0], g_dirs[1]);
        else if (a == SDL_CONTROLLER_AXIS_TRIGGERLEFT) trigger(a, v, TRIG_L);
        else if (a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) trigger(a, v, TRIG_R);
        else break;
        if (abs(v) > 16384) overlay_set_gamepad_active(1);
        break;
    }
    }
}

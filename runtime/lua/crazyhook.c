/* CrazyHook.dll, reimplemented natively (the original is a thin bridge between the
 * game and LuaJIT; its logic is reproduced from REA disassembly of the 2018 DLL):
 *   DllMain        luaL_newstate + luaL_openlibs
 *   CrazyHook(g)   registers the logics CustomLogic/Hit/Attack/Bump and MenuHook with
 *                  the game (thiscall 0x4d6f70 on g), then runs CrazyHook.lua
 *   Chameleon(p)   _lua("_map", p): the universal hook the patched exe calls
 * Every hook calls the Lua function _lua(name, lightuserdata ptr). */
#include <stdlib.h>
#include <string.h>
#include "lua_bridge.h"
#include "../runtime.h"
#include "../win32/win32.h"

#define HMODULE_CRAZYHOOK 0x7f900000u
#define GAME_REGISTER_LOGIC 0x4d6f70u /* thiscall (this, fn, name, 0) */
#define MENU_LOGIC_DEFAULT 0x4614d0u

static lua_State *g_L;
static uint32_t g_vtable; /* the per-object "state" C++ object (destructor calls _destroy) */

lua_State *crazyhook_lua(void) { return g_L; }

static void lua_fail(const char *what) {
    const char *msg = lua_tostring(g_L, -1);
    rt_log("CrazyHook: %s: %s", what, msg ? msg : "?");
    platform_message_box("CrazyHook.lua error", msg ? msg : what);
    lua_pop(g_L, 1);
}

/* _lua(name, ptr) -- 0x10001d90 (fastcall name, ptr) */
static void dispatch(const char *name, uint32_t ptr, int with_ptr) {
    lua_State *L = g_L;
    int top = lua_gettop(L);
    lua_getfield(L, LUA_GLOBALSINDEX, "_lua");
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        platform_message_box(name, "Critical error: function _lua does not exit");
        rt_fatal("CrazyHook: _lua missing");
    }
    lua_pushstring(L, name);
    if (with_ptr) lua_pushlightuserdata(L, ptr ? GPTR(ptr) : NULL);
    if (lua_pcall(L, with_ptr ? 2 : 1, 0, 0)) lua_fail(name);
    lua_settop(L, top);
}

/* object destructor (thiscall, flags; ret 4): calls _destroy(obj), frees on flag 1 */
static void state_dtor(CPU *cpu) {
    uint32_t self = cpu->ecx, flags = ARG(0);
    WR32(self, g_vtable);
    dispatch("_destroy", RD32(self + 4), 1);
    if (flags & 1) heap_free(self);
    cpu->eax = self;
    cpu->ret_addr = RD32(cpu->esp);
    cpu->esp += 8;
}

static void state_unused(CPU *cpu) { rt_fatal("CrazyHook state object: unexpected virtual call"); (void)cpu; }

/* CustomLogic(obj): first call creates the state object and runs _create */
static void custom_logic(CPU *cpu) {
    uint32_t obj = ARG(0);
    uint32_t edi = RD32(obj + 0x7c);
    if (RD32(edi + 0x18) == 0) {
        if (RD32(edi + 0x1c) != 0) platform_message_box("CrazyHook", "state != 0");
        uint32_t st = heap_alloc(8, 1);
        WR32(st, g_vtable);
        WR32(st + 4, obj);
        WR32(edi + 0x18, st);
        dispatch("_create", obj, 1);
    }
    dispatch("_logic", obj, 1);
    RET_CDECL(1);
}
static void custom_hit(CPU *cpu) { dispatch("_hit", ARG(0), 1); RET_CDECL(1); }
static void custom_attack(CPU *cpu) { dispatch("_attack", ARG(0), 1); RET_CDECL(1); }
static void custom_bump(CPU *cpu) { dispatch("_bump", ARG(0), 1); RET_CDECL(1); }
static void menu_hook(CPU *cpu) {
    uint32_t obj = ARG(0);
    dispatch("_menu", 0, 0);
    WR32(RD32(obj + 0x7c) + 0x10, MENU_LOGIC_DEFAULT);
    RET_CDECL(1);
}

/* export Chameleon(ptr) */
static void exp_chameleon(CPU *cpu) { dispatch("_map", ARG(0), 1); RET_CDECL(0); }

static void register_logic(CPU *cpu, uint32_t game, GuestFn fn, const char *name) {
    uint32_t gname = guest_strdup(name); /* the game keeps the pointer */
    uint32_t args[3] = {thunk_register(fn, name), gname, 0};
    uint32_t ecx = cpu->ecx;
    cpu->ecx = game;
    guest_call(cpu, GAME_REGISTER_LOGIC, 3, args);
    cpu->ecx = ecx;
}

/* export CrazyHook(game) */
static void exp_crazyhook(CPU *cpu) {
    uint32_t game = ARG(0);
    register_logic(cpu, game, custom_logic, "CustomLogic");
    register_logic(cpu, game, custom_hit, "CustomHit");
    register_logic(cpu, game, custom_attack, "CustomAttack");
    register_logic(cpu, game, custom_bump, "CustomBump");
    register_logic(cpu, game, menu_hook, "MenuHook");
    rt_log("CrazyHook: running CrazyHook.lua");
    if (luaL_loadfile(g_L, "CrazyHook.lua") || lua_pcall(g_L, 0, LUA_MULTRET, 0)) {
        lua_fail("CrazyHook.lua");
        rt_fatal("CrazyHook.lua failed");
    }
    RET_CDECL(0);
}

static int lua_print(lua_State *L) {
    char line[2048];
    int n = 0, top = lua_gettop(L);
    for (int i = 1; i <= top && n < (int)sizeof line - 2; i++) {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        const char *s = lua_tostring(L, -1);
        n += snprintf(line + n, sizeof line - n, "%s%s", i > 1 ? "\t" : "", s ? s : "");
        lua_pop(L, 1);
    }
    rt_log("lua: %s", line);
    return 0;
}

uint32_t crazyhook_load(void) {
    if (!g_L) {
        g_L = lua_bridge_newstate();
        lua_pushcfunction(g_L, lua_print);
        lua_setglobal(g_L, "print");
        static const GuestFn vt[4] = {state_dtor, state_unused, state_unused, state_unused};
        g_vtable = com_make_vtable(vt, NULL, 4);
        rt_log("CrazyHook: native LuaJIT state ready");
    }
    return HMODULE_CRAZYHOOK;
}

uint32_t crazyhook_proc(uint32_t module, const char *name) {
    if (module != HMODULE_CRAZYHOOK) return 0;
    if (!strcmp(name, "CrazyHook")) return thunk_register(exp_crazyhook, "CrazyHook");
    if (!strcmp(name, "Chameleon")) return thunk_register(exp_chameleon, "Chameleon");
    return 0;
}

/* For the cheat menu: the label and on/off state CrazyHook's scripts keep for a cheat
 * (game_codes.codes_main: CNEW = CrazyHook and custom-level cheats, CORG = original ones
 * the scripts track).  Returns 1 when the scripts know the code. */
int crazyhook_cheat_info(const char *code, char *text, int n, int *toggle) {
    lua_State *L = g_L;
    if (!L) return 0;
    int top = lua_gettop(L), found = 0;
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "loaded");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "game_codes.codes_main");
            if (lua_istable(L, -1)) {
                static const char *lists[] = {"CNEW", "CORG"};
                for (int k = 0; k < 2 && !found; k++) {
                    lua_getfield(L, -1, lists[k]);
                    if (lua_istable(L, -1)) {
                        lua_getfield(L, -1, code);
                        if (lua_istable(L, -1)) {
                            found = 1;
                            lua_getfield(L, -1, "Text");
                            if (lua_isstring(L, -1)) snprintf(text, (size_t)n, "%s", lua_tostring(L, -1));
                            lua_pop(L, 1);
                            lua_getfield(L, -1, "Toggle");
                            if (lua_isnumber(L, -1)) *toggle = lua_tointeger(L, -1) != 0;
                            lua_pop(L, 1);
                        }
                        lua_pop(L, 1);
                    }
                    lua_pop(L, 1);
                }
            }
        }
    }
    lua_settop(L, top);
    return found;
}

/* the WM_COMMAND id CrazyHookConsts gives a cheat name (_message.MPKFA...), or 0 */
int crazyhook_message_id(const char *code) {
    lua_State *L = g_L;
    if (!L) return 0;
    int top = lua_gettop(L), id = 0;
    lua_getglobal(L, "_message");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, code);
        if (lua_isnumber(L, -1)) id = (int)lua_tointeger(L, -1);
    }
    lua_settop(L, top);
    return id;
}

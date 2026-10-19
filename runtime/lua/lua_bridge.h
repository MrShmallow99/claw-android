/* Native LuaJIT (guest-memory FFI) for the game's scripts: see lua_bridge.c. */
#pragma once
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

lua_State *lua_bridge_newstate(void);
void lua_bridge_open_lfs(lua_State *L);

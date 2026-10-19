/* Per-game configuration (games/<name>/game.ini).
 *
 * The ini of the game the port was built for is compiled in (CMake option GAME);
 * a <save dir>/game.ini, if present, is read on top of it, so controls can be
 * tuned on a device without rebuilding.  Format: "[section]" lines and
 * "key = value" lines; '#' or ';' starts a comment; a key may repeat (e.g.
 * "button"), cfg_each() visits every occurrence in order. */
#pragma once

void cfg_init(void);   /* after platform_init (needs the save dir) */
const char *cfg_str(const char *section, const char *key, const char *def);
int cfg_int(const char *section, const char *key, int def);
/* Calls fn(value, ctx) for every `key` in `section`, in file order. */
void cfg_each(const char *section, const char *key, void (*fn)(const char *value, void *ctx), void *ctx);
void cfg_each_line(const char *section, void (*fn)(const char *key, const char *value, void *ctx), void *ctx);

/* Windows virtual-key from a name: "UP", "CTRL", "F9", "A", "0x11", "17"; 0 if unknown. */
int cfg_vk(const char *name);
/* Parses up to `max` whitespace-separated VK names; returns how many were parsed. */
int cfg_vk_list(const char *s, int *out, int max);

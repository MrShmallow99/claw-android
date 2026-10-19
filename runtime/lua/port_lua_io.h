/* Force-included into LuaJIT: its file functions go through the game's VFS. */
#pragma once
#ifndef __ASSEMBLER__
#include <stdio.h>
FILE *port_fopen(const char *path, const char *mode);
int port_remove(const char *path);
int port_rename(const char *a, const char *b);
#define fopen(p, m) port_fopen(p, m)
#define remove(p) port_remove(p)
#define rename(a, b) port_rename(a, b)
#endif

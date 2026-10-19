/* Host runtime shared declarations. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "cpu.h"

typedef struct { uint32_t addr; GuestFn fn; } FuncEntry;
typedef struct { uint32_t iat; const char *dll; const char *name; GuestFn fn; } ImportEntry;
typedef struct { uint32_t addr; int len; const char *bytes; } ImagePatch;
extern const ImagePatch g_image_patches[];
extern const int g_image_patch_count;

extern const FuncEntry g_func_table[];
extern const int g_func_count;
extern const ImportEntry g_import_table[];
extern const int g_import_count;

/* Host thunks: guest-callable addresses that dispatch to host functions. */
#define THUNK_BASE 0xF0000000u
uint32_t thunk_register(GuestFn fn, const char *name);
GuestFn lookup_guest_fn(uint32_t addr);

/* Call a guest function from host code (cdecl or stdcall; args pushed right-to-left). */
uint32_t guest_call(CPU *cpu, uint32_t addr, int nargs, const uint32_t *args);

/* Logging */
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* stdcall helpers for host implementations of guest APIs.
 * On entry [esp] holds the return address, arguments follow. */
#define ARG(n) RD32(cpu->esp + 4 + 4 * (n))
#define RET_STDCALL(nargs, val) do { cpu->eax = (uint32_t)(val); cpu->ret_addr = RD32(cpu->esp); \
    cpu->esp += 4 + 4 * (nargs); return; } while (0)
#define RET_CDECL(val) do { cpu->eax = (uint32_t)(val); cpu->ret_addr = RD32(cpu->esp); \
    cpu->esp += 4; return; } while (0)

/* Guest memory management (runtime/memory.c) */
void mem_init(void);
uint32_t mem_reserve(uint32_t size, uint32_t align);
uint32_t heap_alloc(uint32_t size, int zero);
void heap_free(uint32_t p);
uint32_t heap_realloc(uint32_t p, uint32_t size);
uint32_t heap_size(uint32_t p);
uint32_t guest_strdup(const char *s);
static inline char *gstr(uint32_t a) { return a ? (char *)GPTR(a) : NULL; }

/* Per-thread CPU */
CPU *cpu_current(void);
CPU *cpu_new_thread(uint32_t stack_size);

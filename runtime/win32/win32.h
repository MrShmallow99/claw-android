/* Shared declarations for the Win32 emulation layer. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "../runtime.h"
#include "../platform.h"

#define WIN_TRUE 1
#define WIN_FALSE 0

/* Window handles / module handles handed to the guest */
#define HWND_MAIN       0x00010010u
#define HDC_SCREEN      0x00020010u
#define HDC_MAIN        0x00020020u
#define HMODULE_EXE     0x00400000u
#define HMODULE_KERNEL  0x7ff00000u
#define HMODULE_USER32  0x7fe00000u
#define HMODULE_DSOUND  0x7fd00000u
#define HMODULE_DDRAW   0x7fc00000u
#define HMODULE_GDI32   0x7fb00000u

/* ---- handles ---------------------------------------------------------------- */
enum HandleType { H_FREE = 0, H_FILE, H_THREAD, H_MAPPING, H_EVENT, H_HEAP, H_GDI, H_FIND };
typedef struct {
    int type;
    struct VFile *vf;
    int append;
    void *ptr;
} Handle;
uint32_t handle_new(int type);
Handle *handle_get(uint32_t h, int type);
void handle_close(uint32_t h);

/* ---- per-thread Win32 state ------------------------------------------------------ */
void win32_set_last_error(uint32_t e);
uint32_t win32_get_last_error(void);

/* ---- paths ---------------------------------------------------------------------- */
/* Guest (DOS-style) path -> '/'-separated path relative to the game root. */
void guest_rel_path(const char *guest, char *out, size_t outsz);
/* Resolve for writing (save dir overlay); creates parent dirs. */
int path_resolve_write(const char *guest, char *out, size_t outsz);
extern const char *g_exe_guest_path; /* [game] guest_dir + exe, e.g. "C:\\CLAW\\CLAW.EXE" */
void win32_paths_init(void);

/* ---- messages --------------------------------------------------------------------- */
typedef struct { uint32_t hwnd, message, wparam, lparam, time, x, y; } WinMsg;
void msg_post(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);
uint32_t window_send(CPU *cpu, uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);
extern uint32_t g_wndproc;
extern int g_window_created;

/* ---- loader / module emulation ------------------------------------------------------ */
GuestFn win32_find_export(const char *dll, const char *name);
uint32_t win32_load_library(const char *name);
uint32_t win32_get_proc_address(uint32_t module, const char *name);

/* Allocate zeroed guest memory for host-side objects (COM objects, vtables...). */
uint32_t guest_alloc(uint32_t size);

/* COM helpers */
uint32_t com_make_vtable(const GuestFn *methods, const char *const *names, int count);
#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_FAIL 0x80004005u
#define E_NOTIMPL 0x80004001u
#define E_INVALIDARG 0x80070057u

/* Video */
void gdi_present_dib(uint32_t bits, int w, int h, int bpp, int topdown);

extern int g_trace_api;
#define TRACE(...) do { if (g_trace_api) rt_log(__VA_ARGS__); } while (0)

/* ---- resources of the exe (resource.c) ---------------------------------------------------- */
/* type/name: an integer id (< 0x10000) or a guest string ("#123" allowed).  Returns the
 * guest address of the IMAGE_RESOURCE_DATA_ENTRY, 0 if missing. */
uint32_t res_find(uint32_t type, uint32_t name);
uint32_t res_find_host(int type, const char *name); /* name: string or "#id" */
uint32_t res_data(uint32_t entry, uint32_t *size);  /* guest address of the bytes */
/* Loads string table entry id into out (UTF-16 -> Latin-1); returns its length or -1. */
int res_load_string(uint32_t id, char *out, int outsz);

/* ---- atoms (kernel32_ext.c) ---------------------------------------------------------------- */
uint32_t win32_atom_add(const char *s);
const char *win32_atom_name(uint32_t a);

/* Window manager internals shared by user32.c (windows, messages, painting) and
 * user32_dlg.c (dialogs and the standard controls). */
#pragma once
#include "win32.h"
#include "gdi.h"

#define MAX_WND 1024
#define WND_BASE 0x00010010u

/* messages */
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000a
#define WM_SETREDRAW 0x000b
#define WM_SETTEXT 0x000c
#define WM_GETTEXT 0x000d
#define WM_GETTEXTLENGTH 0x000e
#define WM_PAINT 0x000f
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001c
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_GETMINMAXINFO 0x0024
#define WM_DRAWITEM 0x002b
#define WM_MEASUREITEM 0x002c
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCPAINT 0x0085
#define WM_NCACTIVATE 0x0086
#define WM_GETDLGCODE 0x0087
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_HSCROLL 0x0114
#define WM_VSCROLL 0x0115
#define WM_ENTERIDLE 0x0121
#define WM_CTLCOLORMSGBOX 0x0132
#define WM_CTLCOLOREDIT 0x0133
#define WM_CTLCOLORLISTBOX 0x0134
#define WM_CTLCOLORBTN 0x0135
#define WM_CTLCOLORDLG 0x0136
#define WM_CTLCOLORSCROLLBAR 0x0137
#define WM_CTLCOLORSTATIC 0x0138
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_MOUSEWHEEL 0x020a
#define WM_PARENTNOTIFY 0x0210
#define WM_CAPTURECHANGED 0x0215
#define WM_USER 0x0400

/* styles */
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_CLIPSIBLINGS 0x04000000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_CAPTION 0x00c00000u
#define WS_BORDER 0x00800000u
#define WS_DLGFRAME 0x00400000u
#define WS_VSCROLL 0x00200000u
#define WS_HSCROLL 0x00100000u
#define WS_THICKFRAME 0x00040000u
#define WS_GROUP 0x00020000u
#define WS_TABSTOP 0x00010000u
#define WS_EX_DLGMODALFRAME 0x0001u
#define WS_EX_TOPMOST 0x0008u
#define WS_EX_CLIENTEDGE 0x0200u
#define WS_EX_STATICEDGE 0x20000u
#define DS_MODALFRAME 0x80u
#define DS_CENTER 0x800u

/* WM_GETDLGCODE */
#define DLGC_WANTARROWS 0x1
#define DLGC_WANTTAB 0x2
#define DLGC_WANTALLKEYS 0x4
#define DLGC_HASSETSEL 0x8
#define DLGC_DEFPUSHBUTTON 0x10
#define DLGC_UNDEFPUSHBUTTON 0x20
#define DLGC_RADIOBUTTON 0x40
#define DLGC_WANTCHARS 0x80
#define DLGC_STATIC 0x100
#define DLGC_BUTTON 0x2000

typedef struct WndClass {
    char name[64];
    uint32_t atom, style, wndproc, cls_extra, wnd_extra, hinst, hicon, hcursor, hbrush, menuname;
    uint32_t extra[16];
} WndClass;

typedef struct Prop { uint32_t atom; char *name; uint32_t value; } Prop;

typedef struct Wnd {
    int used;
    uint32_t hwnd;
    WndClass *cls;
    uint32_t wndproc;
    uint32_t style, exstyle;
    uint32_t parent, owner;
    uint32_t id, hinst, userdata, menu;
    int x, y, w, h;               /* window rect: screen coords (top level) or parent client */
    char *text;
    uint8_t extra[64];            /* cbWndExtra (DLGWINDOWEXTRA = 30) */
    Prop props[16];
    int nprops;
    int invalid, erase;
    Rect inv;                     /* client coordinates */
    int destroying;
    void *ctl;                    /* control state (user32_dlg.c) */
    void (*ctl_free)(void *);
    uint32_t font;                /* WM_SETFONT */
    /* dialogs */
    int is_dialog, dlg_ended;
    uint32_t dlg_result, dlg_focus;
    uint32_t create_param;
} Wnd;

Wnd *wnd_get(uint32_t hwnd);
Wnd *wnd_by_index(int i);
uint32_t wm_send(CPU *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void wm_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t wm_create(CPU *cpu, uint32_t exstyle, const char *cls, uint32_t clsatom, const char *name, uint32_t style,
                   int x, int y, int w, int h, uint32_t parent, uint32_t menu, uint32_t hinst, uint32_t param);
void wm_destroy(CPU *cpu, uint32_t hwnd);
void wm_show(CPU *cpu, uint32_t hwnd, int show);
void wm_invalidate(uint32_t hwnd, const Rect *r, int erase);
void wm_set_text(uint32_t hwnd, const char *s);
const char *wm_text(uint32_t hwnd);
uint32_t wm_set_focus(CPU *cpu, uint32_t hwnd);
uint32_t wm_focus(void);
void wm_set_capture(uint32_t hwnd);
uint32_t wm_capture(void);
void wm_client_rect(Wnd *w, Rect *r);              /* client rect in client coords (0,0,cw,ch) */
void wm_client_origin(Wnd *w, int *x, int *y);     /* client origin in screen coords */
void wm_enable(CPU *cpu, uint32_t hwnd, int enable);
int wm_is_child_of(uint32_t hwnd, uint32_t ancestor);
uint32_t wm_first_child(uint32_t parent);
uint32_t wm_next_sibling(uint32_t hwnd);
uint32_t wm_prev_sibling(uint32_t hwnd);
uint32_t wm_def_proc(CPU *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t wm_notify_parent(CPU *cpu, Wnd *w, uint32_t code);
void wm_move(CPU *cpu, uint32_t hwnd, int x, int y, int w, int h, int repaint);
WndClass *wm_register_class(const char *name, uint32_t style, uint32_t wndproc, uint32_t wnd_extra, uint32_t hbrush,
                           uint32_t hcursor);
WndClass *wm_find_class(const char *name, uint32_t atom);
void wm_message_loop_idle(CPU *cpu);  /* pump host events, present if needed */
int wm_get_message(CPU *cpu, WinMsg *m, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove, int wait);
uint32_t wm_dispatch(CPU *cpu, const WinMsg *m);
void wm_translate(const WinMsg *m);
uint32_t wm_dialog_default_proc_thunk(void);

/* user32_dlg.c */
void ctl_register_classes(void);
uint32_t dlg_create_indirect(CPU *cpu, uint32_t hinst, uint32_t tmpl, uint32_t owner, uint32_t dlgproc, uint32_t param);
int dlg_is_dialog_message(CPU *cpu, uint32_t hdlg, const WinMsg *m);
uint32_t dlg_next_tab(uint32_t hdlg, uint32_t from, int prev);
void dlg_text_input(const char *utf8); /* soft keyboard text for the focused edit control */

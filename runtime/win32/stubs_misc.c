/* Subsystems the game links against but doesn't need on a phone: networking
 * (DirectPlay, Winsock, WinInet: multiplayer racing and update checks report
 * "no network"), printing, version resources, common controls/dialogs and the
 * Smacker player (the pack ships no .smk files). */
#include <string.h>
#include "win32.h"

/* ---- DPLAYX (by ordinal: 1 DirectPlayCreate, 2 DirectPlayEnumerateA, 4 DirectPlayLobbyCreateA) -- */
#define DPERR_UNAVAILABLE 0x88770122u
void imp_DPLAYX_ord1(CPU *cpu) { if (ARG(1)) WR32(ARG(1), 0); RET_STDCALL(3, DPERR_UNAVAILABLE); }
void imp_DPLAYX_ord2(CPU *cpu) { RET_STDCALL(2, 0); }
void imp_DPLAYX_ord4(CPU *cpu) { if (ARG(1)) WR32(ARG(1), 0); RET_STDCALL(5, DPERR_UNAVAILABLE); }

/* ---- WSOCK32: no network ---------------------------------------------------------------- */
#define WSAENETDOWN 10050
static uint32_t g_wsa_error = WSAENETDOWN;
void imp_WSOCK32_WSAGetLastError(CPU *cpu) { RET_STDCALL(0, g_wsa_error); }
void imp_WSOCK32_WSASetLastError(CPU *cpu) { g_wsa_error = ARG(0); RET_STDCALL(1, 0); }
void imp_WSOCK32_WSAStartup(CPU *cpu) {
    uint32_t d = ARG(1);
    if (d) { memset(GPTR(d), 0, 400); WR16(d, 0x0101); WR16(d + 2, 0x0101); }
    RET_STDCALL(2, 0);
}
void imp_WSOCK32_WSACleanup(CPU *cpu) { RET_STDCALL(0, 0); }
void imp_WSOCK32_WSAAsyncSelect(CPU *cpu) { RET_STDCALL(4, 0xffffffffu); }
void imp_WSOCK32_socket(CPU *cpu) { g_wsa_error = WSAENETDOWN; RET_STDCALL(3, 0xffffffffu); }
void imp_WSOCK32_closesocket(CPU *cpu) { RET_STDCALL(1, 0); }
void imp_WSOCK32_connect(CPU *cpu) { RET_STDCALL(3, 0xffffffffu); }
void imp_WSOCK32_bind(CPU *cpu) { RET_STDCALL(3, 0xffffffffu); }
void imp_WSOCK32_listen(CPU *cpu) { RET_STDCALL(2, 0xffffffffu); }
void imp_WSOCK32_accept(CPU *cpu) { RET_STDCALL(3, 0xffffffffu); }
void imp_WSOCK32_send(CPU *cpu) { RET_STDCALL(4, 0xffffffffu); }
void imp_WSOCK32_recv(CPU *cpu) { RET_STDCALL(4, 0xffffffffu); }
void imp_WSOCK32_sendto(CPU *cpu) { RET_STDCALL(6, 0xffffffffu); }
void imp_WSOCK32_recvfrom(CPU *cpu) { RET_STDCALL(6, 0xffffffffu); }
void imp_WSOCK32_select(CPU *cpu) { RET_STDCALL(5, 0xffffffffu); }
void imp_WSOCK32_ioctlsocket(CPU *cpu) { RET_STDCALL(3, 0xffffffffu); }
void imp_WSOCK32_setsockopt(CPU *cpu) { RET_STDCALL(5, 0xffffffffu); }
void imp_WSOCK32_getsockname(CPU *cpu) { RET_STDCALL(3, 0xffffffffu); }
void imp_WSOCK32_shutdown(CPU *cpu) { RET_STDCALL(2, 0xffffffffu); }
void imp_WSOCK32_gethostbyname(CPU *cpu) { RET_STDCALL(1, 0); }
void imp_WSOCK32_gethostname(CPU *cpu) {
    if (ARG(1) > 9) strcpy(gstr(ARG(0)), "localhost");
    RET_STDCALL(2, 0);
}
void imp_WSOCK32_htons(CPU *cpu) { uint32_t v = ARG(0) & 0xffff; RET_STDCALL(1, (v >> 8) | ((v & 0xff) << 8)); }
void imp_WSOCK32_ntohs(CPU *cpu) { uint32_t v = ARG(0) & 0xffff; RET_STDCALL(1, (v >> 8) | ((v & 0xff) << 8)); }
void imp_WSOCK32_htonl(CPU *cpu) { RET_STDCALL(1, __builtin_bswap32(ARG(0))); }
void imp_WSOCK32_ntohl(CPU *cpu) { RET_STDCALL(1, __builtin_bswap32(ARG(0))); }
void imp_WSOCK32_inet_addr(CPU *cpu) {
    unsigned a, b, c, d;
    if (sscanf(gstr(ARG(0)), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) RET_STDCALL(1, a | b << 8 | c << 16 | d << 24);
    RET_STDCALL(1, 0xffffffffu);
}
void imp_WSOCK32_inet_ntoa(CPU *cpu) {
    static uint32_t buf;
    if (!buf) buf = heap_alloc(16, 1);
    uint32_t v = ARG(0);
    snprintf(gstr(buf), 16, "%u.%u.%u.%u", v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24);
    RET_STDCALL(1, buf);
}

/* ---- WININET: offline --------------------------------------------------------------------- */
void imp_WININET_InternetOpenA(CPU *cpu) { RET_STDCALL(5, 0); }
void imp_WININET_InternetOpenUrlA(CPU *cpu) { RET_STDCALL(6, 0); }
void imp_WININET_InternetCloseHandle(CPU *cpu) { RET_STDCALL(1, 1); }
void imp_WININET_InternetReadFile(CPU *cpu) { if (ARG(3)) WR32(ARG(3), 0); RET_STDCALL(4, 0); }
void imp_WININET_InternetWriteFile(CPU *cpu) { if (ARG(3)) WR32(ARG(3), 0); RET_STDCALL(4, 0); }
void imp_WININET_InternetQueryDataAvailable(CPU *cpu) { if (ARG(1)) WR32(ARG(1), 0); RET_STDCALL(4, 0); }
void imp_WININET_InternetQueryOptionA(CPU *cpu) { RET_STDCALL(4, 0); }
void imp_WININET_InternetSetFilePointer(CPU *cpu) { RET_STDCALL(5, 0xffffffffu); }
void imp_WININET_InternetSetStatusCallback(CPU *cpu) { RET_STDCALL(2, 0); }
void imp_WININET_InternetCrackUrlA(CPU *cpu) { RET_STDCALL(4, 0); }
void imp_WININET_InternetCanonicalizeUrlA(CPU *cpu) { RET_STDCALL(4, 0); }
void imp_WININET_InternetGetLastResponseInfoA(CPU *cpu) { if (ARG(0)) WR32(ARG(0), 0); RET_STDCALL(3, 0); }

/* ---- WINSPOOL ------------------------------------------------------------------------------ */
void imp_WINSPOOL_OpenPrinterA(CPU *cpu) { RET_STDCALL(3, 0); }
void imp_WINSPOOL_ClosePrinter(CPU *cpu) { RET_STDCALL(1, 1); }
void imp_WINSPOOL_DocumentPropertiesA(CPU *cpu) { RET_STDCALL(6, 0xffffffffu); }

/* ---- VERSION: report "no version resource" ------------------------------------------------- */
void imp_VERSION_GetFileVersionInfoSizeA(CPU *cpu) { if (ARG(1)) WR32(ARG(1), 0); RET_STDCALL(2, 0); }
void imp_VERSION_GetFileVersionInfoA(CPU *cpu) { RET_STDCALL(4, 0); }
void imp_VERSION_VerQueryValueA(CPU *cpu) { RET_STDCALL(4, 0); }

/* ---- COMCTL32 / COMDLG32 ------------------------------------------------------------------- */
void imp_COMCTL32_ord17(CPU *cpu) { RET_STDCALL(0, 0); } /* InitCommonControls */
void imp_COMCTL32_ImageList_Create(CPU *cpu) { RET_STDCALL(5, 0x7a00); }
void imp_COMCTL32_ImageList_Destroy(CPU *cpu) { RET_STDCALL(1, 1); }
void imp_COMCTL32_ImageList_ReplaceIcon(CPU *cpu) { RET_STDCALL(3, 0); }
void imp_COMDLG32_GetFileTitleA(CPU *cpu) {
    const char *p = gstr(ARG(0)), *s = p;
    for (const char *c = p; *c; c++) if (*c == '\\' || *c == '/' || *c == ':') s = c + 1;
    uint32_t n = (uint32_t)strlen(s);
    if (ARG(2) <= n) RET_STDCALL(3, n + 1);
    memcpy(GPTR(ARG(1)), s, n + 1);
    RET_STDCALL(3, 0);
}

/* ---- WINMM extras -------------------------------------------------------------------------- */
void imp_WINMM_joyGetNumDevs(CPU *cpu) { RET_STDCALL(0, 0); }

/* ---- DSOUND as a static import --------------------------------------------------------------- */
void dsound_DirectSoundCreate(CPU *cpu);
void imp_DSOUND_DirectSoundCreate(CPU *cpu) { dsound_DirectSoundCreate(cpu); }

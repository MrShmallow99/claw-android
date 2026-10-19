/* WINMM emulation: timers, joystick, mixer/aux (absent), MCI CD audio. */
#include <string.h>
#include "win32.h"

#define MMSYSERR_NODRIVER 6
#define JOYERR_UNPLUGGED 167

void imp_WINMM_timeGetTime(CPU *cpu) { RET_STDCALL(0, platform_ticks_ms()); }
void imp_WINMM_timeBeginPeriod(CPU *cpu) { RET_STDCALL(1, 0); }
void imp_WINMM_timeEndPeriod(CPU *cpu) { RET_STDCALL(1, 0); }

/* Physical controllers are delivered as keyboard input (see input.c), so the
 * Windows joystick API reports no devices. */
void imp_WINMM_joyGetPos(CPU *cpu) { RET_STDCALL(2, JOYERR_UNPLUGGED); }
void imp_WINMM_joyGetPosEx(CPU *cpu) { RET_STDCALL(2, JOYERR_UNPLUGGED); }
void imp_WINMM_joyGetDevCapsA(CPU *cpu) { RET_STDCALL(3, JOYERR_UNPLUGGED); }

void imp_WINMM_auxGetNumDevs(CPU *cpu) { RET_STDCALL(0, 0); }
void imp_WINMM_auxGetDevCapsA(CPU *cpu) { RET_STDCALL(3, MMSYSERR_NODRIVER); }
void imp_WINMM_auxGetVolume(CPU *cpu) { RET_STDCALL(2, MMSYSERR_NODRIVER); }
void imp_WINMM_auxSetVolume(CPU *cpu) { RET_STDCALL(2, MMSYSERR_NODRIVER); }
void imp_WINMM_mixerGetNumDevs(CPU *cpu) { RET_STDCALL(0, 0); }
void imp_WINMM_mixerGetLineInfoA(CPU *cpu) { RET_STDCALL(3, MMSYSERR_NODRIVER); }
void imp_WINMM_mixerGetLineControlsA(CPU *cpu) { RET_STDCALL(3, MMSYSERR_NODRIVER); }
void imp_WINMM_mixerGetControlDetailsA(CPU *cpu) { RET_STDCALL(3, MMSYSERR_NODRIVER); }
void imp_WINMM_mixerSetControlDetails(CPU *cpu) { RET_STDCALL(3, MMSYSERR_NODRIVER); }

int mci_command(uint32_t dev, uint32_t msg, uint32_t flags, uint32_t params);

void imp_WINMM_mciSendCommandA(CPU *cpu) {
    RET_STDCALL(4, mci_command(ARG(0), ARG(1), ARG(2), ARG(3)));
}

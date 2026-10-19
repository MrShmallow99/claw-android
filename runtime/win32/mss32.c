/* Miles Sound System (MSS32) XMIDI sequence API -> runtime/xmidi.c synthesizer. */
#include <string.h>
#include "win32.h"
#include "../xmidi.h"

/* AIL_startup() */
void imp_MSS32_AIL_startup(CPU *cpu) { xmidi_init(); RET_STDCALL(0, 1); }
void imp_MSS32_AIL_shutdown(CPU *cpu) { RET_STDCALL(0, 0); }

/* AIL_midiOutOpen(HMDIDRIVER *drvr, LPHMIDIOUT *lphMidiOut, int dwDeviceId) -> 0 on success */
void imp_MSS32_AIL_midiOutOpen(CPU *cpu) {
    xmidi_init();
    if (ARG(0)) WR32(ARG(0), 0x4d494449u); /* "MIDI": the driver handle */
    if (ARG(1)) WR32(ARG(1), 0);
    RET_STDCALL(3, 0);
}

void imp_MSS32_AIL_allocate_sequence_handle(CPU *cpu) { RET_STDCALL(1, (uint32_t)xmidi_seq_alloc()); }
void imp_MSS32_AIL_release_sequence_handle(CPU *cpu) { xmidi_seq_release((int)ARG(0)); RET_STDCALL(1, 0); }

/* AIL_init_sequence(seq, start, sequence_num) -> 1 ok, 0 fail, -1 needs timbres */
void imp_MSS32_AIL_init_sequence(CPU *cpu) {
    int r = xmidi_seq_init((int)ARG(0), GPTR(ARG(1)), (int)ARG(2));
    RET_STDCALL(3, (uint32_t)r);
}
void imp_MSS32_AIL_start_sequence(CPU *cpu) { xmidi_seq_start((int)ARG(0)); RET_STDCALL(1, 0); }
void imp_MSS32_AIL_stop_sequence(CPU *cpu) { xmidi_seq_stop((int)ARG(0)); RET_STDCALL(1, 0); }
void imp_MSS32_AIL_resume_sequence(CPU *cpu) { xmidi_seq_resume((int)ARG(0)); RET_STDCALL(1, 0); }
void imp_MSS32_AIL_end_sequence(CPU *cpu) { xmidi_seq_end((int)ARG(0)); RET_STDCALL(1, 0); }
/* SEQ_FREE 1, SEQ_DONE 2, SEQ_PLAYING 4, SEQ_STOPPED 8 */
void imp_MSS32_AIL_sequence_status(CPU *cpu) { RET_STDCALL(1, (uint32_t)xmidi_seq_status((int)ARG(0))); }
void imp_MSS32_AIL_set_sequence_loop_count(CPU *cpu) { xmidi_seq_loops((int)ARG(0), (int)ARG(1)); RET_STDCALL(2, 0); }
/* AIL_set_sequence_tempo(seq, tempo percent, milliseconds) */
void imp_MSS32_AIL_set_sequence_tempo(CPU *cpu) { xmidi_seq_tempo((int)ARG(0), (int)ARG(1)); RET_STDCALL(3, 0); }
/* AIL_set_XMIDI_master_volume(driver, 0..127) */
void imp_MSS32_AIL_set_XMIDI_master_volume(CPU *cpu) { xmidi_master_volume((int)ARG(1)); RET_STDCALL(2, 0); }
void imp_MSS32_AIL_XMIDI_master_volume(CPU *cpu) { RET_STDCALL(1, (uint32_t)xmidi_get_master_volume()); }

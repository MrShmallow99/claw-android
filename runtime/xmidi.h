/* XMIDI (Miles Sound System .XMI) sequencer + General MIDI synthesizer (TinySoundFont). */
#pragma once

void xmidi_init(void);
int xmidi_seq_alloc(void);                         /* handle > 0, 0 if none free */
void xmidi_seq_release(int h);
int xmidi_seq_init(int h, const void *xmi, int seqnum); /* 1 ok, 0 fail */
void xmidi_seq_start(int h);
void xmidi_seq_stop(int h);
void xmidi_seq_resume(int h);
void xmidi_seq_end(int h);
int xmidi_seq_status(int h);                       /* SEQ_DONE 2 / SEQ_PLAYING 4 / SEQ_STOPPED 8 */
void xmidi_seq_loops(int h, int n);                /* 0 = forever */
void xmidi_seq_tempo(int h, int percent);
void xmidi_master_volume(int v);                   /* 0..127 */
int xmidi_get_master_volume(void);
/* adds the synth output to acc (interleaved stereo float) -- called by the mixer */
void xmidi_mix(float *acc, int frames, int rate);

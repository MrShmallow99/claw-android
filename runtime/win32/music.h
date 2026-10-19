/* CD music tracks played from audio files (see mci.c). */
#pragma once

int music_track_count(void);                 /* highest track number available, 0 if none */
int music_track_length(int track);           /* seconds, -1 if the track does not exist */
int music_play(int track, double start, double end_secs);
void music_stop(void);
int music_playing(void);
void music_set_volume(float v);              /* 0..1, from the game's CD volume setting */
/* Called from the audio mixer thread: adds music into an interleaved stereo float buffer. */
void music_mix(float *acc, int frames, int out_rate);

/* FFmpeg-based decoding of the game's movies (Smacker, AVI) from memory. */
#pragma once
#include <stdint.h>

typedef struct Movie Movie;

/* takes ownership of data (malloc'd) */
Movie *movie_open(uint8_t *data, int64_t size, const char *name);
/* streams from a VFile (takes ownership) */
struct VFile;
Movie *movie_open_vfile(struct VFile *vf, const char *name);
void movie_close(Movie *m);
/* presentation time of the current frame in seconds (-1 if unknown) */
double movie_frame_time(Movie *m);
int movie_width(Movie *m);
int movie_height(Movie *m);
double movie_fps(Movie *m);
int movie_frames(Movie *m);
int movie_has_audio(Movie *m);
/* decodes the next video frame; audio met on the way goes to movie_audio_push().
 * Returns 0 at the end.  For paletted video *pal gets 256 0x00RRGGBB entries and
 * *pal_changed is set when they differ from the previous frame. */
int movie_next_frame(Movie *m);
const uint8_t *movie_frame_pal8(Movie *m, int *pitch, const uint32_t **pal, int *pal_changed);
/* the current frame as 0x00RRGGBB, w*h pixels */
const uint32_t *movie_frame_rgb(Movie *m);
int movie_rewind(Movie *m);
/* reads ahead until ~1 s of audio is queued (call while waiting between frames) */
void movie_pump_audio(Movie *m);

void movie_audio_start(int rate);
void movie_audio_stop(void);
void movie_audio_push(const float *stereo, int frames);
int movie_audio_queued(void);
double movie_audio_clock(void);
void movie_audio_set_volume(float v);

/* Audio of the movie being played (Smacker or AVI): the decoder pushes stereo
 * float samples, the DirectSound mixer pulls them (resampled to the output rate). */
#include <string.h>
#include <pthread.h>
#include "platform.h"

#define RING (1 << 19) /* frames, ~11 s at 44.1 kHz */
static float g_ring[RING * 2];
static uint64_t g_wr, g_rd;     /* frames written / read */
static int g_rate = 44100, g_active;
static double g_frac;
static float g_volume = 1.0f;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void movie_audio_start(int rate) {
    pthread_mutex_lock(&g_lock);
    g_wr = g_rd = 0;
    g_frac = 0;
    g_rate = rate > 0 ? rate : 44100;
    g_active = 1;
    pthread_mutex_unlock(&g_lock);
}

void movie_audio_stop(void) {
    pthread_mutex_lock(&g_lock);
    g_active = 0;
    g_wr = g_rd = 0;
    pthread_mutex_unlock(&g_lock);
}

void movie_audio_set_volume(float v) { g_volume = v; }

/* frames queued but not yet played (the decoder keeps about a second ahead) */
int movie_audio_queued(void) {
    pthread_mutex_lock(&g_lock);
    int n = (int)(g_wr - g_rd);
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* seconds of audio played so far (the movie clock when the movie has sound) */
double movie_audio_clock(void) {
    pthread_mutex_lock(&g_lock);
    double t = (double)g_rd / g_rate;
    pthread_mutex_unlock(&g_lock);
    return t;
}

void movie_audio_push(const float *st, int frames) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < frames && g_wr - g_rd < RING; i++) {
        uint64_t k = g_wr++ % RING;
        g_ring[k * 2] = st[i * 2];
        g_ring[k * 2 + 1] = st[i * 2 + 1];
    }
    pthread_mutex_unlock(&g_lock);
}

void avi_audio_mix(float *acc, int frames, int rate) {
    if (!g_active) return;
    pthread_mutex_lock(&g_lock);
    double step = (double)g_rate / rate;
    for (int f = 0; f < frames; f++) {
        if (g_rd + 1 >= g_wr) break;
        uint64_t a = g_rd % RING, b = (g_rd + 1) % RING;
        float t = (float)g_frac;
        acc[f * 2] += (g_ring[a * 2] * (1 - t) + g_ring[b * 2] * t) * g_volume;
        acc[f * 2 + 1] += (g_ring[a * 2 + 1] * (1 - t) + g_ring[b * 2 + 1] * t) * g_volume;
        g_frac += step;
        while (g_frac >= 1.0) { g_frac -= 1.0; g_rd++; }
    }
    pthread_mutex_unlock(&g_lock);
}

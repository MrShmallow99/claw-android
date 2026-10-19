/* CD music: tracks are music/trackNN.{ogg,flac,wav} files (from the pack,
 * the save folder or the game folder). */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "win32.h"
#include "music.h"
#include "../pack/vfs.h"
#include "../pack/drflac.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "../../third_party/stb/stb_vorbis.c"

#define MAX_TRACKS 100

enum { SRC_NONE, SRC_FLAC, SRC_OGG, SRC_WAV };

typedef struct {
    int kind;
    char rel[64];        /* path in the game file system */
    char path[1200];     /* real path (wav) */
    int length;          /* seconds, -1 = missing */
} TrackInfo;

static TrackInfo g_tracks[MAX_TRACKS];
static int g_scanned, g_max_track;
static int g_fake_cd;    /* debug: PORT_FAKE_CD=n pretends n silent tracks */

/* ---- FLAC over VFile ---------------------------------------------------------------- */
static size_t vf_read_cb(void *u, void *buf, size_t n) { return vfs_read(u, buf, n); }
static drflac_bool32 vf_seek_cb(void *u, int off, drflac_seek_origin o) {
    return vfs_seek(u, off, o == drflac_seek_origin_start ? SEEK_SET : SEEK_CUR) >= 0;
}

typedef struct {
    VFile *vf;
    drflac *fl;
} Flac;

static int flac_open(Flac *f, const char *rel) {
    f->vf = vfs_open(rel);
    if (!f->vf) return -1;
    f->fl = drflac_open(vf_read_cb, vf_seek_cb, f->vf, NULL);
    if (!f->fl) {
        vfs_close(f->vf);
        f->vf = NULL;
        return -1;
    }
    return 0;
}

static void flac_close(Flac *f) {
    if (f->fl) drflac_close(f->fl);
    if (f->vf) vfs_close(f->vf);
    f->fl = NULL;
    f->vf = NULL;
}

static int wav_open(const char *path, FILE **out, int *rate, int *ch, int *bits, long *start, long *end) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(f); return -1; }
    int got_fmt = 0;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) break;
        uint32_t sz = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[16];
            if (sz < 16 || fread(fm, 1, 16, f) != 16) break;
            *ch = fm[2] | fm[3] << 8;
            *rate = (int)(fm[4] | fm[5] << 8 | fm[6] << 16 | (uint32_t)fm[7] << 24);
            *bits = fm[14] | fm[15] << 8;
            fseek(f, (long)(sz - 16 + (sz & 1)), SEEK_CUR);
            got_fmt = 1;
        } else if (!memcmp(c, "data", 4)) {
            if (!got_fmt) break;
            *start = ftell(f);
            *end = *start + (long)sz;
            *out = f;
            return 0;
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
    fclose(f);
    return -1;
}

/* ---- playback sources -------------------------------------------------------------- */
typedef struct {
    int kind;
    int rate, channels;
    stb_vorbis *ogg;
    uint8_t *ogg_data;  /* whole .ogg file (stb_vorbis decodes from memory) */
    FILE *wav;
    int wav_bits;
    long wav_end;
    Flac flac;
} Source;

static void src_close(Source *s) {
    if (s->ogg) stb_vorbis_close(s->ogg);
    if (s->wav) fclose(s->wav);
    free(s->ogg_data);
    flac_close(&s->flac);
    memset(s, 0, sizeof *s);
}

static int src_open(Source *s, const TrackInfo *ti, double start) {
    memset(s, 0, sizeof *s);
    s->kind = ti->kind;
    switch (ti->kind) {
    case SRC_FLAC:
        if (flac_open(&s->flac, ti->rel)) return -1;
        s->rate = (int)s->flac.fl->sampleRate;
        s->channels = s->flac.fl->channels;
        if (start > 0) drflac_seek_to_pcm_frame(s->flac.fl, (drflac_uint64)(start * s->rate));
        return 0;
    case SRC_OGG: {
        int err;
        VFile *f = vfs_open(ti->rel);
        if (!f) return -1;
        int64_t n = vfs_size(f);
        s->ogg_data = n > 0 ? malloc((size_t)n) : NULL;
        int ok = s->ogg_data && vfs_read(f, s->ogg_data, (size_t)n) == (size_t)n;
        vfs_close(f);
        if (ok) s->ogg = stb_vorbis_open_memory(s->ogg_data, (int)n, &err, NULL);
        if (!s->ogg) {
            src_close(s);
            return -1;
        }
        stb_vorbis_info inf = stb_vorbis_get_info(s->ogg);
        s->rate = (int)inf.sample_rate;
        s->channels = 2;
        if (start > 0) stb_vorbis_seek(s->ogg, (unsigned)(start * s->rate));
        return 0;
    }
    case SRC_WAV: {
        long st;
        if (wav_open(ti->path, &s->wav, &s->rate, &s->channels, &s->wav_bits, &st, &s->wav_end)) return -1;
        long off = (long)(start * s->rate) * s->channels * (s->wav_bits / 8);
        fseek(s->wav, st + off < s->wav_end ? st + off : s->wav_end, SEEK_SET);
        return 0;
    }
    }
    return -1;
}

/* Reads up to n frames as interleaved stereo float; returns frames read (0 = end). */
static int src_read(Source *s, float *out, int n) {
    switch (s->kind) {
    case SRC_OGG:
        return stb_vorbis_get_samples_float_interleaved(s->ogg, 2, out, n * 2);
    case SRC_WAV: {
        int bps = s->wav_bits / 8, ch = s->channels;
        uint8_t raw[4096 * 4];
        long remain = s->wav_end - ftell(s->wav);
        long want = (long)n * bps * ch;
        if (want > (long)sizeof raw) want = (long)sizeof raw / (bps * ch) * (bps * ch);
        if (want > remain) want = remain;
        size_t got = want > 0 ? fread(raw, 1, (size_t)want, s->wav) : 0;
        int frames = (int)(got / (size_t)(bps * ch));
        for (int i = 0; i < frames; i++)
            for (int c = 0; c < 2; c++) {
                int sc = c < ch ? c : 0;
                out[i * 2 + c] = bps == 2 ? ((int16_t)(raw[(i * ch + sc) * 2] | raw[(i * ch + sc) * 2 + 1] << 8)) / 32768.0f
                                          : (raw[i * ch + sc] - 128) / 128.0f;
            }
        return frames;
    }
    case SRC_FLAC: {
        int16_t tmp[4096 * 2];
        int ch = s->channels;
        if (n > 4096 * 2 / ch) n = 4096 * 2 / ch;
        int got = (int)drflac_read_pcm_frames_s16(s->flac.fl, (drflac_uint64)n, tmp);
        for (int i = 0; i < got; i++) {
            out[i * 2] = tmp[i * ch] / 32768.0f;
            out[i * 2 + 1] = tmp[i * ch + (ch > 1)] / 32768.0f;
        }
        return got;
    }
    }
    return 0;
}

/* ---- track table ---------------------------------------------------------------- */
static int track_seconds(TrackInfo *ti) {
    Source s;
    if (src_open(&s, ti, 0)) return -1;
    double secs = -1;
    if (s.kind == SRC_FLAC) secs = (double)s.flac.fl->totalPCMFrameCount / s.rate;
    else if (s.kind == SRC_OGG) secs = stb_vorbis_stream_length_in_seconds(s.ogg);
    else if (s.kind == SRC_WAV) secs = (double)(s.wav_end - ftell(s.wav)) / (s.rate * s.channels * (s.wav_bits / 8));
    src_close(&s);
    return secs < 0 ? -1 : (int)ceil(secs);
}

static void scan(void) {
    if (g_scanned) return;
    g_scanned = 1;
    const char *fake = getenv("PORT_FAKE_CD");
    if (fake) {
        g_fake_cd = atoi(fake);
        g_max_track = g_fake_cd;
        return;
    }
    static const struct { const char *ext; int kind; } kinds[] = {{"ogg", SRC_OGG}, {"flac", SRC_FLAC}, {"wav", SRC_WAV}};
    for (int t = 1; t < MAX_TRACKS; t++) {
        TrackInfo *ti = &g_tracks[t];
        ti->length = -1;
        for (size_t k = 0; k < sizeof kinds / sizeof kinds[0] && ti->length < 0; k++) {
            snprintf(ti->rel, sizeof ti->rel, "music/track%02d.%s", t, kinds[k].ext);
            if (vfs_stat(ti->rel, NULL, NULL)) continue;
            if (kinds[k].kind == SRC_WAV && vfs_loose_path(ti->rel, ti->path, sizeof ti->path)) continue;
            ti->kind = kinds[k].kind;
            ti->length = track_seconds(ti);
        }
        if (ti->length >= 0) {
            g_max_track = t;
            rt_log("music: track %d = %s (%d s)", t, ti->rel, ti->length);
        }
    }
}

int music_track_count(void) {
    scan();
    return g_max_track;
}

int music_track_length(int track) {
    scan();
    if (g_fake_cd) return track >= 2 && track <= g_fake_cd ? 180 : -1;
    if (track <= 0 || track >= MAX_TRACKS) return -1;
    return g_tracks[track].length;
}

/* ---- playback (guarded by the audio lock) ---------------------------------------- */
static struct {
    int active;
    int track;
    Source src;
    double pos_frames, end_frames;
    float buf[4096 * 2];
    int buf_len, buf_pos;  /* in frames */
    double frac;           /* resampler phase */
    float prev[2], cur[2];
} g_play;

static float g_volume = 1.0f;

int dsound_ensure_audio(void);

static void close_current(void) {
    src_close(&g_play.src);
    g_play.active = 0;
}

int music_play(int track, double start, double end_secs) {
    scan();
    if (g_fake_cd) {
        rt_log("music (fake CD): play track %d from %.1f to %.1f", track, start, end_secs);
        platform_audio_lock();
        g_play.active = 1;
        g_play.track = track;
        platform_audio_unlock();
        return 1;
    }
    if (music_track_length(track) < 0) return 0;
    dsound_ensure_audio();
    Source s;
    int ok = src_open(&s, &g_tracks[track], start) == 0;
    platform_audio_lock();
    close_current();
    if (ok) {
        g_play.src = s;
        g_play.active = 1;
        g_play.track = track;
        g_play.pos_frames = start * s.rate;
        g_play.end_frames = end_secs > 0 ? end_secs * s.rate : 1e18;
        g_play.buf_len = g_play.buf_pos = 0;
        g_play.frac = 0;
        g_play.prev[0] = g_play.prev[1] = g_play.cur[0] = g_play.cur[1] = 0;
    }
    platform_audio_unlock();
    return ok;
}

void music_stop(void) {
    platform_audio_lock();
    close_current();
    platform_audio_unlock();
}

int music_playing(void) {
    return g_play.active;
}

void music_set_volume(float v) {
    g_volume = v < 0 ? 0 : v > 1 ? 1 : v;
}

/* Refill the decode buffer; returns frames available (0 at end of track). */
static int refill(void) {
    if (g_play.buf_pos < g_play.buf_len) return g_play.buf_len - g_play.buf_pos;
    g_play.buf_pos = 0;
    g_play.buf_len = 0;
    if (g_play.pos_frames >= g_play.end_frames) return 0;
    g_play.buf_len = src_read(&g_play.src, g_play.buf, 4096);
    return g_play.buf_len;
}

void music_mix(float *acc, int frames, int out_rate) {
    if (!g_play.active || g_fake_cd || !g_play.src.kind) return;
    double step = (double)g_play.src.rate / out_rate;
    float vol = g_volume * 0.9f;
    for (int f = 0; f < frames; f++) {
        g_play.frac += step;
        while (g_play.frac >= 1.0) {
            g_play.frac -= 1.0;
            if (!refill()) {
                close_current();
                return;
            }
            g_play.prev[0] = g_play.cur[0];
            g_play.prev[1] = g_play.cur[1];
            g_play.cur[0] = g_play.buf[g_play.buf_pos * 2];
            g_play.cur[1] = g_play.buf[g_play.buf_pos * 2 + 1];
            g_play.buf_pos++;
            g_play.pos_frames += 1;
        }
        float t = (float)g_play.frac;
        acc[f * 2] += (g_play.prev[0] + (g_play.cur[0] - g_play.prev[0]) * t) * vol;
        acc[f * 2 + 1] += (g_play.prev[1] + (g_play.cur[1] - g_play.prev[1]) * t) * vol;
    }
}

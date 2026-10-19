/* DirectSound emulation: IDirectSound, IDirectSoundBuffer, IDirectSound3DBuffer,
 * IDirectSound3DListener, mixed in software into the platform audio stream.
 *
 * Sound data lives in guest memory (Lock hands out guest pointers); the mixer
 * reads it from the audio thread.  Buffer state is guarded by the audio lock. */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "win32.h"
#include "../xmidi.h"
void avi_audio_mix(float *acc, int frames, int rate);
#include "music.h"

#define DSERR_NODRIVER 0x88780078u
#define DSERR_INVALIDPARAM E_INVALIDARG
#define DSERR_BUFFERLOST 0x88780096u

#define DSBCAPS_PRIMARYBUFFER 0x1
#define DSBCAPS_CTRL3D 0x10
#define DSBCAPS_CTRLFREQUENCY 0x20
#define DSBPLAY_LOOPING 0x1
#define DSBSTATUS_PLAYING 0x1
#define DSBSTATUS_LOOPING 0x4

#define MIX_RATE 44100
#define MAX_BUFFERS 512
#define CURSOR_HIST 8

typedef struct { float x, y, z; } Vec3;

typedef struct {
    Vec3 pos, vel;
    float min_dist, max_dist;
    uint32_t mode;
} Params3D;

typedef struct DSBuf {
    int used;
    int primary;
    int refs;
    uint32_t obj;        /* guest IDirectSoundBuffer object */
    uint32_t obj3d;      /* guest IDirectSound3DBuffer object (lazily created) */
    uint32_t data;       /* guest address of sample data */
    uint32_t bytes;
    int shared_owner;    /* index of buffer owning `data` for duplicates, or -1 */
    uint32_t flags;
    int channels, bits, rate, block;
    /* playback */
    int playing, looping;
    double pos;          /* in frames: how far the mixer has read */
    double upos;         /* frames read in total (unwrapped) */
    /* (time, upos) at the start of the last CURSOR_HIST mixer callbacks */
    uint64_t hist_us[CURSOR_HIST];
    double hist_pos[CURSOR_HIST];
    int hist_n, hist_i;
    double mix_block;    /* frames one mixer callback reads */
    double start_upos;   /* upos when playback (re)started */
    int flip;
    int freq;            /* current frequency */
    int32_t volume, pan; /* hundredths of dB */
    Params3D p3d, p3d_deferred;
} DSBuf;

static DSBuf g_bufs[MAX_BUFFERS];
static int g_dsound_ready;
static int g_out_rate = MIX_RATE;
static uint32_t g_vt_ds, g_vt_buf, g_vt_3dbuf, g_vt_listener;
static uint32_t g_ds_obj, g_listener_obj;

/* listener */
static struct {
    Vec3 pos, vel, front, top;
    float distance_factor, rolloff, doppler;
} g_lis = {{0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 1.0f, 1.0f, 1.0f}, g_lis_deferred;

/* ---- helpers ------------------------------------------------------------------------- */
static float argf(CPU *cpu, int n) {
    uint32_t v = ARG(n);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static Vec3 rdvec(uint32_t a) {
    Vec3 v;
    memcpy(&v, GPTR(a), 12);
    return v;
}
static void wrvec(uint32_t a, Vec3 v) { memcpy(GPTR(a), &v, 12); }

static DSBuf *buf_from_obj(uint32_t obj) {
    uint32_t i = RD32(obj + 4);
    if (i >= MAX_BUFFERS || !g_bufs[i].used) rt_fatal("dsound: bad buffer object %08x", obj);
    return &g_bufs[i];
}

static float db_to_gain(int32_t v) {
    if (v <= -10000) return 0.0f;
    if (v >= 0) return 1.0f;
    return powf(10.0f, (float)v / 2000.0f);
}

/* ---- mixer ---------------------------------------------------------------------------- */
static void calc_3d(const DSBuf *b, float *gain, float *pan) {
    *gain = 1.0f;
    *pan = 0.0f;
    if (!(b->flags & DSBCAPS_CTRL3D) || b->p3d.mode == 2) return;
    Vec3 rel = b->p3d.pos;
    if (b->p3d.mode == 0) {
        rel.x -= g_lis.pos.x; rel.y -= g_lis.pos.y; rel.z -= g_lis.pos.z;
    }
    float d = sqrtf(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z) * g_lis.distance_factor;
    float mn = b->p3d.min_dist > 0 ? b->p3d.min_dist : 1.0f;
    float mx = b->p3d.max_dist > mn ? b->p3d.max_dist : 1e9f;
    if (d > mx) d = mx;
    if (d > mn) *gain = mn / (mn + g_lis.rolloff * (d - mn));
    if (d > 1e-4f && b->p3d.mode == 0) {
        /* right = top x front (left-handed) */
        Vec3 f = g_lis.front, t = g_lis.top;
        Vec3 r = {t.y * f.z - t.z * f.y, t.z * f.x - t.x * f.z, t.x * f.y - t.y * f.x};
        float rl = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z);
        if (rl > 1e-6f) {
            float dl = sqrtf(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
            *pan = (rel.x * r.x + rel.y * r.y + rel.z * r.z) / (rl * dl);
        }
    } else if (d > 1e-4f) {
        float dl = sqrtf(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
        *pan = rel.x / dl;
    }
    *pan *= 0.8f; /* keep some signal in both ears */
}

static inline float sample_at(const DSBuf *b, const uint8_t *data, uint32_t frame, int ch) {
    if (b->bits == 16) return ((const int16_t *)data)[frame * b->channels + ch] * (1.0f / 32768.0f);
    return (data[frame * b->channels + ch] - 128) * (1.0f / 128.0f);
}

static void mix(int16_t *out, int frames) {
    static float acc[8192 * 2];
    if (frames > 8192) frames = 8192;
    memset(acc, 0, sizeof(float) * frames * 2);
    uint64_t now = platform_ticks_us();
    for (int i = 0; i < MAX_BUFFERS; i++) {
        DSBuf *b = &g_bufs[i];
        if (!b->used || b->primary || !b->playing || !b->bytes) continue;
        b->hist_us[b->hist_i] = now;
        b->hist_pos[b->hist_i] = b->upos;
        b->hist_i = (b->hist_i + 1) % CURSOR_HIST;
        if (b->hist_n < CURSOR_HIST) b->hist_n++;
        const DSBuf *owner = b->shared_owner >= 0 ? &g_bufs[b->shared_owner] : b;
        const uint8_t *data = GPTR(owner->data);
        uint32_t nframes = b->bytes / b->block;
        if (!nframes) continue;
        float g = db_to_gain(b->volume), g3, pan3;
        calc_3d(b, &g3, &pan3);
        g *= g3;
        float pan = 0;
        if (b->pan) {
            float pv = db_to_gain(-abs(b->pan));
            pan = b->pan > 0 ? 1.0f - pv : pv - 1.0f;
        }
        pan += pan3;
        if (pan > 1) pan = 1;
        if (pan < -1) pan = -1;
        float gl = g * (pan > 0 ? 1.0f - pan : 1.0f), gr = g * (pan < 0 ? 1.0f + pan : 1.0f);
        double step = (double)b->freq / g_out_rate;
        double pos = b->pos;
        int done = frames;
        for (int f = 0; f < frames; f++) {
            uint32_t i0 = (uint32_t)pos;
            if (i0 >= nframes) {
                if (!b->looping) { b->playing = 0; pos = 0; done = f; break; }
                pos -= nframes;
                i0 = (uint32_t)pos;
                if (i0 >= nframes) { pos = 0; i0 = 0; }
            }
            uint32_t i1 = i0 + 1;
            if (i1 >= nframes) i1 = b->looping ? 0 : i0;
            float t = (float)(pos - i0);
            float l = sample_at(b, data, i0, 0) * (1 - t) + sample_at(b, data, i1, 0) * t;
            float r = l;
            if (b->channels == 2) r = sample_at(b, data, i0, 1) * (1 - t) + sample_at(b, data, i1, 1) * t;
            acc[f * 2] += l * gl;
            acc[f * 2 + 1] += r * gr;
            pos += step;
        }
        if (b->playing && pos >= nframes) pos = b->looping ? fmod(pos, nframes) : nframes;
        b->pos = pos;
        b->upos += done * step;
        b->mix_block = frames * step;
    }
    music_mix(acc, frames, g_out_rate);
    xmidi_mix(acc, frames, g_out_rate);
    avi_audio_mix(acc, frames, g_out_rate);
    for (int k = 0; k < frames * 2; k++) {
        float v = acc[k] * 32767.0f;
        out[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    static FILE *dump = (FILE *)-1;
    if (dump == (FILE *)-1) {
        const char *p = getenv("PORT_DUMP_AUDIO");
        dump = p ? fopen(p, "wb") : NULL;
    }
    if (dump) fwrite(out, 4, (size_t)frames, dump);
}

/* ---- buffer creation --------------------------------------------------------------------- */
static int buf_alloc(void) {
    for (int i = 0; i < MAX_BUFFERS; i++)
        if (!g_bufs[i].used) {
            memset(&g_bufs[i], 0, sizeof(DSBuf));
            g_bufs[i].used = 1;
            g_bufs[i].shared_owner = -1;
            g_bufs[i].refs = 1;
            g_bufs[i].p3d.min_dist = 1.0f;
            g_bufs[i].p3d.max_dist = 1e9f;
            return i;
        }
    return -1;
}

static uint32_t make_obj(uint32_t vt, uint32_t idx) {
    uint32_t o = guest_alloc(16);
    WR32(o, vt);
    WR32(o + 4, idx);
    return o;
}

static void set_format(DSBuf *b, uint32_t wfx) {
    b->channels = RD16(wfx + 2);
    b->rate = (int)RD32(wfx + 4);
    b->bits = RD16(wfx + 14);
    b->block = RD16(wfx + 12);
    if (b->block <= 0) b->block = b->channels * b->bits / 8;
    if (b->block <= 0) b->block = 1;
    b->freq = b->rate;
}

/* ---- IDirectSound ------------------------------------------------------------------------- */
static void DS_QueryInterface(CPU *cpu) {
    if (ARG(2)) WR32(ARG(2), 0);
    RET_STDCALL(3, E_NOINTERFACE);
}
static void DS_AddRef(CPU *cpu) { RET_STDCALL(1, 2); }
static void DS_Release(CPU *cpu) { RET_STDCALL(1, 1); }

static void DS_CreateSoundBuffer(CPU *cpu) {
    uint32_t desc = ARG(1), pp = ARG(2);
    uint32_t flags = RD32(desc + 4), bytes = RD32(desc + 8), wfx = RD32(desc + 16);
    platform_audio_lock();
    int i = buf_alloc();
    if (i < 0) {
        platform_audio_unlock();
        rt_log("dsound: out of buffers");
        RET_STDCALL(4, E_FAIL);
    }
    DSBuf *b = &g_bufs[i];
    b->flags = flags;
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        b->primary = 1;
        b->channels = 2; b->bits = 16; b->rate = 22050; b->block = 4; b->freq = 22050;
        b->bytes = 0x10000;
    } else {
        if (!wfx) { b->used = 0; platform_audio_unlock(); RET_STDCALL(4, DSERR_INVALIDPARAM); }
        set_format(b, wfx);
        b->bytes = bytes;
        b->data = guest_alloc(bytes + 16);
        if (b->bits == 8) memset(GPTR(b->data), 0x80, bytes);
    }
    b->obj = make_obj(g_vt_buf, (uint32_t)i);
    platform_audio_unlock();
    TRACE("CreateSoundBuffer(flags=%x, bytes=%u, %dHz %dbit %dch) -> #%d", flags, bytes, b->rate, b->bits, b->channels, i);
    WR32(pp, b->obj);
    RET_STDCALL(4, S_OK);
}

static void DS_GetCaps(CPU *cpu) {
    uint32_t c = ARG(1);
    uint32_t sz = RD32(c);
    memset(GPTR(c + 4), 0, sz > 4 ? sz - 4 : 0);
    WR32(c + 4, 0x0f00 | 0x400); /* DSCAPS_*: secondary 8/16 bit mono/stereo, emulated */
    WR32(c + 8, 100);
    WR32(c + 12, 100000);
    RET_STDCALL(2, S_OK);
}

static void DS_DuplicateSoundBuffer(CPU *cpu) {
    uint32_t orig = ARG(1), pp = ARG(2);
    platform_audio_lock();
    DSBuf *o = buf_from_obj(orig);
    int i = buf_alloc();
    if (i < 0) { platform_audio_unlock(); RET_STDCALL(3, E_FAIL); }
    DSBuf *b = &g_bufs[i];
    int owner = o->shared_owner >= 0 ? o->shared_owner : (int)(o - g_bufs);
    b->flags = o->flags;
    b->channels = o->channels; b->bits = o->bits; b->rate = o->rate; b->block = o->block;
    b->freq = o->rate;
    b->bytes = o->bytes;
    b->data = o->data;
    b->shared_owner = owner;
    g_bufs[owner].refs++;
    b->obj = make_obj(g_vt_buf, (uint32_t)i);
    platform_audio_unlock();
    WR32(pp, b->obj);
    RET_STDCALL(3, S_OK);
}

static void DS_SetCooperativeLevel(CPU *cpu) { RET_STDCALL(3, S_OK); }
static void DS_Compact(CPU *cpu) { RET_STDCALL(1, S_OK); }
static void DS_GetSpeakerConfig(CPU *cpu) { WR32(ARG(1), 4); RET_STDCALL(2, S_OK); }
static void DS_SetSpeakerConfig(CPU *cpu) { RET_STDCALL(2, S_OK); }
static void DS_Initialize(CPU *cpu) { RET_STDCALL(2, S_OK); }

/* ---- IDirectSoundBuffer -------------------------------------------------------------------- */
static const uint8_t IID_3DBUFFER[16] = {0x86, 0xfa, 0x9a, 0x27, 0x81, 0x49, 0xce, 0x11, 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60};
static const uint8_t IID_3DLISTENER[16] = {0x84, 0xfa, 0x9a, 0x27, 0x81, 0x49, 0xce, 0x11, 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60};

static void free_buf(DSBuf *b) {
    if (b->shared_owner >= 0) {
        DSBuf *o = &g_bufs[b->shared_owner];
        if (--o->refs <= 0) {
            heap_free(o->data);
            o->used = 0;
        }
    } else if (b->refs <= 0) {
        if (b->data) heap_free(b->data);
    } else {
        /* still referenced by duplicates: keep the data alive */
        b->playing = 0;
        b->obj = 0;
        return;
    }
    heap_free(b->obj);
    if (b->obj3d) heap_free(b->obj3d);
    b->used = 0;
}

static void DSB_QueryInterface(CPU *cpu) {
    uint32_t self = ARG(0), iid = ARG(1), pp = ARG(2);
    DSBuf *b = buf_from_obj(self);
    if (!memcmp(GPTR(iid), IID_3DBUFFER, 16) && !b->primary) {
        if (!b->obj3d) b->obj3d = make_obj(g_vt_3dbuf, RD32(self + 4));
        b->refs++;
        WR32(pp, b->obj3d);
        RET_STDCALL(3, S_OK);
    }
    if (!memcmp(GPTR(iid), IID_3DLISTENER, 16) && b->primary) {
        if (!g_listener_obj) g_listener_obj = make_obj(g_vt_listener, 0);
        WR32(pp, g_listener_obj);
        RET_STDCALL(3, S_OK);
    }
    WR32(pp, 0);
    RET_STDCALL(3, E_NOINTERFACE);
}

static void DSB_AddRef(CPU *cpu) {
    DSBuf *b = buf_from_obj(ARG(0));
    RET_STDCALL(1, ++b->refs);
}

static void DSB_Release(CPU *cpu) {
    platform_audio_lock();
    DSBuf *b = buf_from_obj(ARG(0));
    int r = --b->refs;
    if (r <= 0 && !b->primary) free_buf(b);
    platform_audio_unlock();
    RET_STDCALL(1, r < 0 ? 0 : r);
}

static void DSB_GetCaps(CPU *cpu) {
    DSBuf *b = buf_from_obj(ARG(0));
    uint32_t c = ARG(1);
    WR32(c + 4, b->flags | 0x8); /* LOCSOFTWARE */
    WR32(c + 8, b->bytes);
    WR32(c + 12, 0);
    WR32(c + 16, 0);
    RET_STDCALL(2, S_OK);
}

/* The play cursor.  Streaming code (the movie sound) refills the part of the
 * buffer the cursor has left, and refills at once when two readings are equal
 * (it takes that as a stalled buffer).  So the cursor must never pass what the
 * mixer has read, yet keep moving between mixer callbacks, which come late and
 * unevenly (on phones, gaps of one to two callback blocks).  It runs with the
 * clock averaged over the last CURSOR_HIST callbacks, two blocks behind, so it
 * has room to move through a late callback; if it still reaches what has been
 * read (or is held at the start), it wobbles by a frame instead of standing still. */
static uint32_t current_play_frame(DSBuf *b) {
    double pos = b->pos;
    if (b->playing && b->hist_n) {
        int old = b->hist_n < CURSOR_HIST ? 0 : b->hist_i;
        double est = b->hist_pos[old] + (double)(platform_ticks_us() - b->hist_us[old]) / 1e6 * b->freq
                     - 2 * b->mix_block;
        b->flip ^= 1;
        if (est >= b->upos) est = b->upos - b->flip;
        if (est <= b->start_upos) est = b->start_upos + b->flip;
        pos -= b->upos - est;
    }
    uint32_t nframes = b->bytes / b->block;
    if (nframes) {
        pos = fmod(pos, nframes);
        if (pos < 0) pos += nframes;
    }
    return (uint32_t)pos;
}

/* The write cursor: the first byte the mixer has not read yet. */
static uint32_t current_write_frame(DSBuf *b) {
    uint32_t nframes = b->bytes / b->block;
    return nframes ? (uint32_t)fmod(b->pos, nframes) : 0;
}

static void DSB_GetCurrentPosition(CPU *cpu) {
    platform_audio_lock();
    DSBuf *b = buf_from_obj(ARG(0));
    uint32_t play = current_play_frame(b) * b->block;
    uint32_t write = current_write_frame(b) * b->block;
    platform_audio_unlock();
    if (ARG(1)) WR32(ARG(1), play);
    if (ARG(2)) WR32(ARG(2), write);
    RET_STDCALL(3, S_OK);
}

static void DSB_GetFormat(CPU *cpu) {
    DSBuf *b = buf_from_obj(ARG(0));
    uint32_t w = ARG(1), size = ARG(2), pw = ARG(3);
    if (w && size >= 16) {
        WR16(w, 1);
        WR16(w + 2, b->channels);
        WR32(w + 4, b->rate);
        WR32(w + 8, b->rate * b->block);
        WR16(w + 12, b->block);
        WR16(w + 14, b->bits);
        if (size >= 18) WR16(w + 16, 0);
    }
    if (pw) WR32(pw, 18);
    RET_STDCALL(4, S_OK);
}

static void DSB_GetVolume(CPU *cpu) { WR32(ARG(1), (uint32_t)buf_from_obj(ARG(0))->volume); RET_STDCALL(2, S_OK); }
static void DSB_GetPan(CPU *cpu) { WR32(ARG(1), (uint32_t)buf_from_obj(ARG(0))->pan); RET_STDCALL(2, S_OK); }
static void DSB_GetFrequency(CPU *cpu) { WR32(ARG(1), (uint32_t)buf_from_obj(ARG(0))->freq); RET_STDCALL(2, S_OK); }

static void DSB_GetStatus(CPU *cpu) {
    platform_audio_lock();
    DSBuf *b = buf_from_obj(ARG(0));
    uint32_t s = 0;
    if (b->playing || b->primary) s |= DSBSTATUS_PLAYING;
    if (b->looping) s |= DSBSTATUS_LOOPING;
    platform_audio_unlock();
    WR32(ARG(1), s);
    RET_STDCALL(2, S_OK);
}

static void DSB_Initialize(CPU *cpu) { RET_STDCALL(3, S_OK); }

static void DSB_Lock(CPU *cpu) {
    DSBuf *b = buf_from_obj(ARG(0));
    uint32_t off = ARG(1), n = ARG(2), pp1 = ARG(3), pb1 = ARG(4), pp2 = ARG(5), pb2 = ARG(6), flags = ARG(7);
    if (b->primary || !b->bytes) RET_STDCALL(8, E_FAIL);
    if (flags & 2) { off = 0; n = b->bytes; }
    if (flags & 1) {
        platform_audio_lock();
        off = current_write_frame(b) * b->block;
        platform_audio_unlock();
    }
    if (n > b->bytes) n = b->bytes;
    off %= b->bytes;
    const DSBuf *owner = b->shared_owner >= 0 ? &g_bufs[b->shared_owner] : b;
    uint32_t first = n;
    if (off + n > b->bytes) first = b->bytes - off;
    WR32(pp1, owner->data + off);
    WR32(pb1, first);
    if (pp2) WR32(pp2, first < n ? owner->data : 0);
    if (pb2) WR32(pb2, n - first);
    RET_STDCALL(8, S_OK);
}

static void DSB_Play(CPU *cpu) {
    platform_audio_lock();
    DSBuf *b = buf_from_obj(ARG(0));
    if (!b->primary) {
        b->playing = 1;
        b->looping = (ARG(3) & DSBPLAY_LOOPING) != 0;
        b->hist_n = b->hist_i = 0;
        b->start_upos = b->upos;
    }
    platform_audio_unlock();
    RET_STDCALL(4, S_OK);
}

static void DSB_SetCurrentPosition(CPU *cpu) {
    platform_audio_lock();
    DSBuf *b = buf_from_obj(ARG(0));
    if (b->block) b->pos = (double)(ARG(1) / (uint32_t)b->block);
    b->hist_n = b->hist_i = 0;
    b->start_upos = b->upos;
    platform_audio_unlock();
    RET_STDCALL(2, S_OK);
}

static void DSB_SetFormat(CPU *cpu) {
    DSBuf *b = buf_from_obj(ARG(0));
    if (!b->primary) set_format(b, ARG(1));
    RET_STDCALL(2, S_OK);
}

static void DSB_SetVolume(CPU *cpu) { buf_from_obj(ARG(0))->volume = (int32_t)ARG(1); RET_STDCALL(2, S_OK); }
static void DSB_SetPan(CPU *cpu) { buf_from_obj(ARG(0))->pan = (int32_t)ARG(1); RET_STDCALL(2, S_OK); }
static void DSB_SetFrequency(CPU *cpu) {
    DSBuf *b = buf_from_obj(ARG(0));
    uint32_t f = ARG(1);
    b->freq = f ? (int)f : b->rate;
    RET_STDCALL(2, S_OK);
}

static void DSB_Stop(CPU *cpu) {
    platform_audio_lock();
    DSBuf *b = buf_from_obj(ARG(0));
    b->playing = 0;
    platform_audio_unlock();
    RET_STDCALL(1, S_OK);
}

static void DSB_Unlock(CPU *cpu) { RET_STDCALL(5, S_OK); }
static void DSB_Restore(CPU *cpu) { RET_STDCALL(1, S_OK); }

/* ---- IDirectSound3DBuffer ------------------------------------------------------------------------ */
#define B3D (buf_from_obj(ARG(0)))
static void apply3d(DSBuf *b, uint32_t apply) {
    if (apply == 0) b->p3d = b->p3d_deferred; /* DS3D_IMMEDIATE == 0 */
}
static Params3D *p3d_target(DSBuf *b) {
    b->p3d_deferred = b->p3d;
    return &b->p3d_deferred;
}

static void D3B_QueryInterface(CPU *cpu) { WR32(ARG(2), 0); RET_STDCALL(3, E_NOINTERFACE); }
static void D3B_AddRef(CPU *cpu) { RET_STDCALL(1, ++B3D->refs); }
static void D3B_Release(CPU *cpu) {
    platform_audio_lock();
    DSBuf *b = B3D;
    int r = --b->refs;
    if (r <= 0) free_buf(b);
    platform_audio_unlock();
    RET_STDCALL(1, r < 0 ? 0 : r);
}
static void D3B_GetAllParameters(CPU *cpu) {
    DSBuf *b = B3D;
    uint32_t p = ARG(1);
    wrvec(p + 4, b->p3d.pos);
    wrvec(p + 16, b->p3d.vel);
    WR32(p + 28, 360); WR32(p + 32, 360);
    wrvec(p + 36, (Vec3){0, 0, 1});
    WR32(p + 48, 0);
    WRF32(p + 52, b->p3d.min_dist);
    WRF32(p + 56, b->p3d.max_dist);
    WR32(p + 60, b->p3d.mode);
    RET_STDCALL(2, S_OK);
}
static void D3B_GetConeAngles(CPU *cpu) { WR32(ARG(1), 360); WR32(ARG(2), 360); RET_STDCALL(3, S_OK); }
static void D3B_GetConeOrientation(CPU *cpu) { wrvec(ARG(1), (Vec3){0, 0, 1}); RET_STDCALL(2, S_OK); }
static void D3B_GetConeOutsideVolume(CPU *cpu) { WR32(ARG(1), 0); RET_STDCALL(2, S_OK); }
static void D3B_GetMaxDistance(CPU *cpu) { WRF32(ARG(1), B3D->p3d.max_dist); RET_STDCALL(2, S_OK); }
static void D3B_GetMinDistance(CPU *cpu) { WRF32(ARG(1), B3D->p3d.min_dist); RET_STDCALL(2, S_OK); }
static void D3B_GetMode(CPU *cpu) { WR32(ARG(1), B3D->p3d.mode); RET_STDCALL(2, S_OK); }
static void D3B_GetPosition(CPU *cpu) { wrvec(ARG(1), B3D->p3d.pos); RET_STDCALL(2, S_OK); }
static void D3B_GetVelocity(CPU *cpu) { wrvec(ARG(1), B3D->p3d.vel); RET_STDCALL(2, S_OK); }
static void D3B_SetAllParameters(CPU *cpu) {
    DSBuf *b = B3D;
    uint32_t p = ARG(1);
    Params3D *t = p3d_target(b);
    t->pos = rdvec(p + 4);
    t->vel = rdvec(p + 16);
    t->min_dist = (float)RDF32(p + 52);
    t->max_dist = (float)RDF32(p + 56);
    t->mode = RD32(p + 60);
    apply3d(b, ARG(2));
    RET_STDCALL(3, S_OK);
}
static void D3B_SetConeAngles(CPU *cpu) { RET_STDCALL(4, S_OK); }
static void D3B_SetConeOrientation(CPU *cpu) { RET_STDCALL(5, S_OK); }
static void D3B_SetConeOutsideVolume(CPU *cpu) { RET_STDCALL(3, S_OK); }
static void D3B_SetMaxDistance(CPU *cpu) { DSBuf *b = B3D; p3d_target(b)->max_dist = argf(cpu, 1); apply3d(b, ARG(2)); RET_STDCALL(3, S_OK); }
static void D3B_SetMinDistance(CPU *cpu) { DSBuf *b = B3D; p3d_target(b)->min_dist = argf(cpu, 1); apply3d(b, ARG(2)); RET_STDCALL(3, S_OK); }
static void D3B_SetMode(CPU *cpu) { DSBuf *b = B3D; p3d_target(b)->mode = ARG(1); apply3d(b, ARG(2)); RET_STDCALL(3, S_OK); }
static void D3B_SetPosition(CPU *cpu) {
    DSBuf *b = B3D;
    p3d_target(b)->pos = (Vec3){argf(cpu, 1), argf(cpu, 2), argf(cpu, 3)};
    apply3d(b, ARG(4));
    RET_STDCALL(5, S_OK);
}
static void D3B_SetVelocity(CPU *cpu) {
    DSBuf *b = B3D;
    p3d_target(b)->vel = (Vec3){argf(cpu, 1), argf(cpu, 2), argf(cpu, 3)};
    apply3d(b, ARG(4));
    RET_STDCALL(5, S_OK);
}

/* ---- IDirectSound3DListener ------------------------------------------------------------------------- */
static void lis_apply(uint32_t apply) {
    if (apply == 0) g_lis = g_lis_deferred;
}
static void lis_begin(void) { g_lis_deferred = g_lis; }

static void D3L_QueryInterface(CPU *cpu) { WR32(ARG(2), 0); RET_STDCALL(3, E_NOINTERFACE); }
static void D3L_AddRef(CPU *cpu) { RET_STDCALL(1, 2); }
static void D3L_Release(CPU *cpu) { RET_STDCALL(1, 1); }
static void D3L_GetAllParameters(CPU *cpu) {
    uint32_t p = ARG(1);
    wrvec(p + 4, g_lis.pos);
    wrvec(p + 16, g_lis.vel);
    wrvec(p + 28, g_lis.front);
    wrvec(p + 40, g_lis.top);
    WRF32(p + 52, g_lis.distance_factor);
    WRF32(p + 56, g_lis.rolloff);
    WRF32(p + 60, g_lis.doppler);
    RET_STDCALL(2, S_OK);
}
static void D3L_GetDistanceFactor(CPU *cpu) { WRF32(ARG(1), g_lis.distance_factor); RET_STDCALL(2, S_OK); }
static void D3L_GetDopplerFactor(CPU *cpu) { WRF32(ARG(1), g_lis.doppler); RET_STDCALL(2, S_OK); }
static void D3L_GetOrientation(CPU *cpu) { wrvec(ARG(1), g_lis.front); wrvec(ARG(2), g_lis.top); RET_STDCALL(3, S_OK); }
static void D3L_GetPosition(CPU *cpu) { wrvec(ARG(1), g_lis.pos); RET_STDCALL(2, S_OK); }
static void D3L_GetRolloffFactor(CPU *cpu) { WRF32(ARG(1), g_lis.rolloff); RET_STDCALL(2, S_OK); }
static void D3L_GetVelocity(CPU *cpu) { wrvec(ARG(1), g_lis.vel); RET_STDCALL(2, S_OK); }
static void D3L_SetAllParameters(CPU *cpu) {
    uint32_t p = ARG(1);
    lis_begin();
    g_lis_deferred.pos = rdvec(p + 4);
    g_lis_deferred.vel = rdvec(p + 16);
    g_lis_deferred.front = rdvec(p + 28);
    g_lis_deferred.top = rdvec(p + 40);
    g_lis_deferred.distance_factor = (float)RDF32(p + 52);
    g_lis_deferred.rolloff = (float)RDF32(p + 56);
    g_lis_deferred.doppler = (float)RDF32(p + 60);
    lis_apply(ARG(2));
    RET_STDCALL(3, S_OK);
}
static void D3L_SetDistanceFactor(CPU *cpu) { lis_begin(); g_lis_deferred.distance_factor = argf(cpu, 1); lis_apply(ARG(2)); RET_STDCALL(3, S_OK); }
static void D3L_SetDopplerFactor(CPU *cpu) { lis_begin(); g_lis_deferred.doppler = argf(cpu, 1); lis_apply(ARG(2)); RET_STDCALL(3, S_OK); }
static void D3L_SetOrientation(CPU *cpu) {
    lis_begin();
    g_lis_deferred.front = (Vec3){argf(cpu, 1), argf(cpu, 2), argf(cpu, 3)};
    g_lis_deferred.top = (Vec3){argf(cpu, 4), argf(cpu, 5), argf(cpu, 6)};
    lis_apply(ARG(7));
    RET_STDCALL(8, S_OK);
}
static void D3L_SetPosition(CPU *cpu) {
    lis_begin();
    g_lis_deferred.pos = (Vec3){argf(cpu, 1), argf(cpu, 2), argf(cpu, 3)};
    lis_apply(ARG(4));
    RET_STDCALL(5, S_OK);
}
static void D3L_SetRolloffFactor(CPU *cpu) { lis_begin(); g_lis_deferred.rolloff = argf(cpu, 1); lis_apply(ARG(2)); RET_STDCALL(3, S_OK); }
static void D3L_SetVelocity(CPU *cpu) {
    lis_begin();
    g_lis_deferred.vel = (Vec3){argf(cpu, 1), argf(cpu, 2), argf(cpu, 3)};
    lis_apply(ARG(4));
    RET_STDCALL(5, S_OK);
}
static void D3L_CommitDeferredSettings(CPU *cpu) {
    platform_audio_lock();
    g_lis = g_lis_deferred;
    for (int i = 0; i < MAX_BUFFERS; i++)
        if (g_bufs[i].used) g_bufs[i].p3d = g_bufs[i].p3d_deferred;
    platform_audio_unlock();
    RET_STDCALL(1, S_OK);
}

/* ---- creation ---------------------------------------------------------------------------------------- */
#define VT(name, ...) do { \
    static const GuestFn fns[] = {__VA_ARGS__}; \
    name = com_make_vtable(fns, NULL, (int)(sizeof fns / sizeof fns[0])); } while (0)

static void dsound_init_vtables(void) {
    VT(g_vt_ds, DS_QueryInterface, DS_AddRef, DS_Release, DS_CreateSoundBuffer, DS_GetCaps,
       DS_DuplicateSoundBuffer, DS_SetCooperativeLevel, DS_Compact, DS_GetSpeakerConfig,
       DS_SetSpeakerConfig, DS_Initialize);
    VT(g_vt_buf, DSB_QueryInterface, DSB_AddRef, DSB_Release, DSB_GetCaps, DSB_GetCurrentPosition,
       DSB_GetFormat, DSB_GetVolume, DSB_GetPan, DSB_GetFrequency, DSB_GetStatus, DSB_Initialize,
       DSB_Lock, DSB_Play, DSB_SetCurrentPosition, DSB_SetFormat, DSB_SetVolume, DSB_SetPan,
       DSB_SetFrequency, DSB_Stop, DSB_Unlock, DSB_Restore);
    VT(g_vt_3dbuf, D3B_QueryInterface, D3B_AddRef, D3B_Release, D3B_GetAllParameters, D3B_GetConeAngles,
       D3B_GetConeOrientation, D3B_GetConeOutsideVolume, D3B_GetMaxDistance, D3B_GetMinDistance,
       D3B_GetMode, D3B_GetPosition, D3B_GetVelocity, D3B_SetAllParameters, D3B_SetConeAngles,
       D3B_SetConeOrientation, D3B_SetConeOutsideVolume, D3B_SetMaxDistance, D3B_SetMinDistance,
       D3B_SetMode, D3B_SetPosition, D3B_SetVelocity);
    VT(g_vt_listener, D3L_QueryInterface, D3L_AddRef, D3L_Release, D3L_GetAllParameters,
       D3L_GetDistanceFactor, D3L_GetDopplerFactor, D3L_GetOrientation, D3L_GetPosition,
       D3L_GetRolloffFactor, D3L_GetVelocity, D3L_SetAllParameters, D3L_SetDistanceFactor,
       D3L_SetDopplerFactor, D3L_SetOrientation, D3L_SetPosition, D3L_SetRolloffFactor,
       D3L_SetVelocity, D3L_CommitDeferredSettings);
}

static int g_audio_open;

/* Opens the output device (also used by CD music, which may start before DirectSound). */
int dsound_ensure_audio(void) {
    if (!g_audio_open) {
        int rate = platform_audio_open(MIX_RATE, mix);
        if (rate <= 0) return 0;
        g_out_rate = rate;
        g_audio_open = 1;
    }
    return 1;
}

/* HRESULT DirectSoundCreate(LPGUID, LPDIRECTSOUND *, LPUNKNOWN) */
void dsound_DirectSoundCreate(CPU *cpu) {
    uint32_t pp = ARG(1);
    if (!g_dsound_ready) {
        if (!dsound_ensure_audio()) {
            WR32(pp, 0);
            RET_STDCALL(3, DSERR_NODRIVER);
        }
        dsound_init_vtables();
        g_ds_obj = make_obj(g_vt_ds, 0);
        g_dsound_ready = 1;
    }
    WR32(pp, g_ds_obj);
    RET_STDCALL(3, S_OK);
}

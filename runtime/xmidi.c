/* XMIDI sequencer and General MIDI synthesizer.
 *
 * XMI (Miles Sound System) is MIDI in an IFF container: FORM XDIR (sequence count),
 * CAT XMID with one FORM XMID per sequence holding TIMB (timbres, unused for GM)
 * and EVNT.  EVNT has no running status and no note-offs: a note-on carries its
 * duration as a variable-length number, delays are runs of bytes < 0x80, and the
 * clock is a fixed 120 ticks per second.  Controllers 116/117 are Miles' FOR/NEXT
 * loop markers.  The notes are rendered with TinySoundFont from a GM SoundFont. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "runtime.h"
#include "platform.h"
#include "xmidi.h"
#include "pack/vfs.h"

#define TSF_IMPLEMENTATION
#include "../third_party/tsf/tsf.h"

typedef struct { uint32_t tick, dur; uint8_t st, d1, d2, pad; } Ev;
typedef struct { uint32_t tick; uint8_t ch, note; } NoteOff;

#define MAX_SEQ 8
#define MAX_OFF 256
#define MAX_FOR 4

typedef struct {
    int used, status;          /* 2 done, 4 playing, 8 stopped */
    Ev *ev;
    int nev, pos;
    double tick;               /* current position (ticks) */
    int tempo;                 /* percent */
    int loops;                 /* remaining whole-sequence plays (0 = forever) */
    NoteOff off[MAX_OFF];
    int noff;
    struct { int pos; uint32_t tick; int count; } forst[MAX_FOR];
    int nfor;
    tsf *synth;
} Seq;

static Seq g_seq[MAX_SEQ];
static tsf *g_sf;
static int g_inited, g_master = 127;
static int g_rate;

static void load_soundfont(void) {
    const char *names[] = {getenv("PORT_SOUNDFONT"), "port/soundfont.sf2", NULL};
    for (int i = 0; i < 2 && !g_sf; i++) {
        if (!names[i]) continue;
        if (i == 0) { g_sf = tsf_load_filename(names[i]); continue; }
        VFile *f = vfs_open(names[i]);
        if (!f) continue;
        int64_t n = vfs_size(f);
        void *buf = malloc((size_t)n);
        if (buf && vfs_read(f, buf, (size_t)n) == (size_t)n) g_sf = tsf_load_memory(buf, (int)n);
        free(buf);
        vfs_close(f);
    }
    if (!g_sf) {
        int64_t n = 0;
        void *buf = platform_read_port_file("soundfont.sf2", &n);
        if (buf) g_sf = tsf_load_memory(buf, (int)n);
        free(buf);
    }
    rt_log("xmidi: soundfont %s", g_sf ? "loaded" : "missing (music will be silent)");
}

void xmidi_init(void) {
    if (g_inited) return;
    g_inited = 1;
    load_soundfont();
}

int xmidi_seq_alloc(void) {
    xmidi_init();
    platform_audio_lock();
    for (int i = 1; i < MAX_SEQ; i++)
        if (!g_seq[i].used) {
            memset(&g_seq[i], 0, sizeof g_seq[i]);
            g_seq[i].used = 1;
            g_seq[i].status = 2;
            g_seq[i].tempo = 100;
            g_seq[i].loops = 1;
            platform_audio_unlock();
            return i;
        }
    platform_audio_unlock();
    return 0;
}

static Seq *seq_get(int h) { return h > 0 && h < MAX_SEQ && g_seq[h].used ? &g_seq[h] : NULL; }

static void notes_off(Seq *s) {
    if (s->synth) tsf_note_off_all(s->synth);
    s->noff = 0;
}

void xmidi_seq_release(int h) {
    platform_audio_lock();
    Seq *s = seq_get(h);
    if (s) {
        free(s->ev);
        if (s->synth) tsf_close(s->synth);
        memset(s, 0, sizeof *s);
    }
    platform_audio_unlock();
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* finds the EVNT chunk of sequence `want` */
static const uint8_t *find_evnt(const uint8_t *p, const uint8_t *end, int want, int *idx, uint32_t *len) {
    while (p + 8 <= end) {
        uint32_t n = be32(p + 4);
        const uint8_t *body = p + 8, *next = body + n + (n & 1);
        if (next > end) next = end;
        if (!memcmp(p, "FORM", 4) || !memcmp(p, "CAT ", 4)) {
            if (body + 4 <= end && !memcmp(body, "XMID", 4) && !memcmp(p, "FORM", 4)) {
                /* one sequence */
                if (*idx == want) {
                    const uint8_t *r = find_evnt(body + 4, next, want, idx, len);
                    if (r) return r;
                }
                (*idx)++;
            } else {
                const uint8_t *r = find_evnt(body + 4, next, want, idx, len);
                if (r) return r;
            }
        } else if (!memcmp(p, "EVNT", 4)) {
            *len = n;
            return body;
        }
        p = next;
    }
    return NULL;
}

static uint32_t vlq(const uint8_t **pp, const uint8_t *end) {
    uint32_t v = 0;
    const uint8_t *p = *pp;
    while (p < end) {
        uint8_t b = *p++;
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    *pp = p;
    return v;
}

int xmidi_seq_init(int h, const void *data, int seqnum) {
    Seq *s = seq_get(h);
    if (!s || !data) return 0;
    const uint8_t *p = data;
    /* the header tells the container size: FORM XDIR <len> ..., then CAT */
    uint32_t total = 8 + be32(p + 4);
    const uint8_t *end = p + total;
    if (!memcmp(p, "FORM", 4) && !memcmp(p + 8, "XDIR", 4)) {
        const uint8_t *cat = p + total + (total & 1);
        if (!memcmp(cat, "CAT ", 4)) end = cat + 8 + be32(cat + 4);
    }
    int idx = 0;
    uint32_t len = 0;
    const uint8_t *ev = find_evnt(p, end, seqnum, &idx, &len);
    if (!ev) { rt_log("xmidi: sequence %d not found", seqnum); return 0; }
    const uint8_t *q = ev, *qe = ev + len;
    int cap = 1024, n = 0;
    Ev *out = malloc(sizeof(Ev) * (size_t)cap);
    uint32_t tick = 0;
    while (q < qe) {
        uint8_t b = *q;
        if (b < 0x80) { /* delay run */
            while (q < qe && *q < 0x80) tick += *q++;
            continue;
        }
        q++;
        Ev e = {tick, 0, b, 0, 0, 0};
        int type = b & 0xf0;
        if (b == 0xff) {
            uint8_t mt = q < qe ? *q++ : 0;
            uint32_t ml = vlq(&q, qe);
            q += ml;
            if (mt == 0x2f) { e.st = 0xff; e.d1 = 0x2f; }
            else continue;
        } else if (b == 0xf0 || b == 0xf7) {
            uint32_t ml = vlq(&q, qe);
            q += ml;
            continue;
        } else if (type == 0x90) {
            e.d1 = q < qe ? *q++ : 0;
            e.d2 = q < qe ? *q++ : 0;
            e.dur = vlq(&q, qe);
        } else if (type == 0xc0 || type == 0xd0) {
            e.d1 = q < qe ? *q++ : 0;
        } else {
            e.d1 = q < qe ? *q++ : 0;
            e.d2 = q < qe ? *q++ : 0;
        }
        if (n == cap) { cap *= 2; out = realloc(out, sizeof(Ev) * (size_t)cap); }
        out[n++] = e;
        if (e.st == 0xff) break;
    }
    platform_audio_lock();
    free(s->ev);
    s->ev = out;
    s->nev = n;
    s->pos = 0;
    s->tick = 0;
    s->nfor = 0;
    s->status = 8;
    if (g_sf && !s->synth) s->synth = tsf_copy(g_sf);
    if (s->synth) {
        tsf_reset(s->synth);
        for (int c = 0; c < 16; c++) tsf_channel_set_presetnumber(s->synth, c, 0, c == 9);
    }
    notes_off(s);
    platform_audio_unlock();
    return 1;
}

int dsound_ensure_audio(void);
void xmidi_seq_start(int h) {
    dsound_ensure_audio();
    platform_audio_lock();
    Seq *s = seq_get(h);
    if (s && s->ev) {
        notes_off(s);
        s->pos = 0;
        s->tick = 0;
        s->nfor = 0;
        if (s->synth) for (int c = 0; c < 16; c++) {
            tsf_channel_set_presetnumber(s->synth, c, 0, c == 9);
            tsf_channel_midi_control(s->synth, c, 121, 0); /* reset controllers */
        }
        s->status = 4;
    }
    platform_audio_unlock();
}

void xmidi_seq_stop(int h) {
    platform_audio_lock();
    Seq *s = seq_get(h);
    if (s && s->status == 4) { s->status = 8; notes_off(s); }
    platform_audio_unlock();
}

void xmidi_seq_resume(int h) {
    platform_audio_lock();
    Seq *s = seq_get(h);
    if (s && s->status == 8 && s->ev) s->status = 4;
    platform_audio_unlock();
}

void xmidi_seq_end(int h) {
    platform_audio_lock();
    Seq *s = seq_get(h);
    if (s) { s->status = 2; notes_off(s); }
    platform_audio_unlock();
}

int xmidi_seq_status(int h) {
    Seq *s = seq_get(h);
    return s ? s->status : 1;
}

void xmidi_seq_loops(int h, int n) {
    Seq *s = seq_get(h);
    if (s) s->loops = n;
}

void xmidi_seq_tempo(int h, int percent) {
    Seq *s = seq_get(h);
    if (s) s->tempo = percent > 0 ? percent : 100;
}

void xmidi_master_volume(int v) { g_master = v < 0 ? 0 : v > 127 ? 127 : v; }
int xmidi_get_master_volume(void) { return g_master; }

/* ---- playback (mixer thread, audio lock held by the caller's device callback) --------- */
static void do_event(Seq *s, const Ev *e) {
    tsf *f = s->synth;
    int ch = e->st & 15;
    switch (e->st & 0xf0) {
    case 0x90:
        if (e->d2 == 0) { if (f) tsf_channel_note_off(f, ch, e->d1); break; }
        if (f) tsf_channel_note_on(f, ch, e->d1, e->d2 / 127.0f);
        if (s->noff < MAX_OFF) s->off[s->noff++] = (NoteOff){e->tick + e->dur, (uint8_t)ch, e->d1};
        break;
    case 0x80:
        if (f) tsf_channel_note_off(f, ch, e->d1);
        break;
    case 0xb0:
        if (e->d1 == 116) { /* FOR loop start: value = count (0 forever) */
            if (s->nfor < MAX_FOR) {
                s->forst[s->nfor].pos = s->pos;
                s->forst[s->nfor].tick = e->tick;
                s->forst[s->nfor].count = e->d2;
                s->nfor++;
            }
        } else if (e->d1 == 117) { /* NEXT / BREAK */
            if (s->nfor > 0 && e->d2 >= 64) {
                int k = s->nfor - 1;
                if (s->forst[k].count == 0 || --s->forst[k].count > 0) {
                    /* jump back: positions are relative to the loop start time */
                    double back = (double)e->tick - s->forst[k].tick;
                    s->pos = s->forst[k].pos;
                    s->tick -= back;
                    for (int i = 0; i < s->noff; i++) s->off[i].tick -= (uint32_t)back;
                } else {
                    s->nfor--;
                }
            }
        } else if (e->d1 >= 110 && e->d1 <= 120) {
            /* other Miles controllers (channel lock, callbacks, branches): ignored */
        } else if (f) {
            tsf_channel_midi_control(f, ch, e->d1, e->d2);
        }
        break;
    case 0xc0:
        if (f) tsf_channel_set_presetnumber(f, ch, e->d1, ch == 9);
        break;
    case 0xe0:
        if (f) tsf_channel_set_pitchwheel(f, ch, e->d1 | e->d2 << 7);
        break;
    }
}

static float g_tmp[4096 * 2];

void xmidi_mix(float *acc, int frames, int rate) {
    if (rate != g_rate && g_sf) {
        g_rate = rate;
        tsf_set_output(g_sf, TSF_STEREO_INTERLEAVED, rate, -6.0f);
        for (int i = 1; i < MAX_SEQ; i++)
            if (g_seq[i].synth) tsf_set_output(g_seq[i].synth, TSF_STEREO_INTERLEAVED, rate, -6.0f);
    }
    float vol = g_master / 127.0f;
    for (int i = 1; i < MAX_SEQ; i++) {
        Seq *s = &g_seq[i];
        if (!s->used || s->status != 4 || !s->ev) continue;
        double tps = 120.0 * s->tempo / 100.0;
        int done = 0;
        while (done < frames) {
            /* due events and note-offs */
            for (;;) {
                int progressed = 0;
                for (int k = 0; k < s->noff; k++)
                    if (s->off[k].tick <= s->tick) {
                        if (s->synth) tsf_channel_note_off(s->synth, s->off[k].ch, s->off[k].note);
                        s->off[k--] = s->off[--s->noff];
                        progressed = 1;
                    }
                while (s->pos < s->nev && s->ev[s->pos].tick <= s->tick) {
                    const Ev *e = &s->ev[s->pos++];
                    if (e->st == 0xff) { s->pos = s->nev; break; }
                    do_event(s, e);
                    progressed = 1;
                }
                if (!progressed) break;
            }
            if (s->pos >= s->nev && !s->noff) {
                /* end of sequence: loop or finish */
                if (s->loops == 0 || s->loops > 1) {
                    if (s->loops > 1) s->loops--;
                    s->pos = 0;
                    s->tick = 0;
                    s->nfor = 0;
                    continue;
                }
                s->status = 2;
                break;
            }
            double next = 1e18;
            if (s->pos < s->nev) next = s->ev[s->pos].tick;
            for (int k = 0; k < s->noff; k++) if (s->off[k].tick < next) next = s->off[k].tick;
            int chunk = (int)ceil((next - s->tick) * rate / tps);
            if (chunk < 1) chunk = 1;
            if (chunk > frames - done) chunk = frames - done;
            if (chunk > 4096) chunk = 4096;
            if (s->synth) {
                tsf_render_float(s->synth, g_tmp, chunk, 0);
                for (int k = 0; k < chunk * 2; k++) acc[done * 2 + k] += g_tmp[k] * vol;
            }
            done += chunk;
            s->tick += chunk * tps / rate;
        }
    }
}

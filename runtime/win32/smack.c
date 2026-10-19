/* SMACKW32 (RAD Smacker) on FFmpeg's smacker decoder.  The exe imports by ordinal:
 *   14 SmackOpen(name|HANDLE, flags, extrabuf)   18 SmackClose(smk)
 *   19 SmackDoFrame(smk)                         21 SmackNextFrame(smk)
 *   23 SmackToBuffer(smk, left, top, pitch, height, buf, flags)
 *   27 SmackGoto(smk, frame)                     28 SmackToBufferRect(smk, surface)
 *   32 SmackWait(smk)                            17 SmackSoundOnOff(smk, on)
 *   38 SmackSoundUseDirectSound(ds)
 * (mapping from REA decompilation of the exe's Smacker wrapper at 0x4f4000-0x4f4680).
 * The Smack struct the game reads lives in guest memory, in RAD's layout. */
#include <stdlib.h>
#include <string.h>
#include "win32.h"
#include "../pack/vfs.h"
#include "../movie.h"

#define SMACKFILEHANDLE 0x1000u

/* offsets in RAD's Smack struct */
enum { S_VERSION = 0, S_WIDTH = 4, S_HEIGHT = 8, S_FRAMES = 12, S_MSPERFRAME = 16, S_TYPE = 20,
       S_NEWPAL = 104, S_PALETTE = 108, S_FRAMENUM = 884, S_RECTX = 896, S_RECTY = 900, S_RECTW = 904,
       S_RECTH = 908, S_OPENFLAGS = 912, S_SIZE = 1024 };

#define MAX_SMK 8
static struct {
    uint32_t g;           /* guest Smack* (0 = free) */
    Movie *mv;
    int decoded;          /* frames decoded so far */
    uint32_t buf, pitch, left, top, height, flags;
    int rect_pending;
    uint64_t start_us;
    double mspf;
    int sound;
} g_smk[MAX_SMK];

static int find(uint32_t g) {
    for (int i = 0; i < MAX_SMK; i++)
        if (g && g_smk[i].g == g) return i;
    return -1;
}

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* SmackOpen(name, flags, extrabuf) -> Smack* */
void imp_SMACKW32_ord14(CPU *cpu) {
    uint32_t a = ARG(0), flags = ARG(1);
    VFile *vf = NULL;
    int own = 0;
    char nm[300] = "handle";
    if (flags & SMACKFILEHANDLE) {
        Handle *hd = handle_get(a, H_FILE);
        if (hd) vf = hd->vf;
    } else if (a) {
        char rel[1024];
        guest_rel_path(gstr(a), rel, sizeof rel);
        snprintf(nm, sizeof nm, "%s", rel);
        vf = vfs_open(rel);
        own = 1;
    }
    if (!vf) RET_STDCALL(3, 0);
    int64_t start = vfs_tell(vf);
    uint8_t hdr[104];
    if (vfs_read(vf, hdr, sizeof hdr) != sizeof hdr || memcmp(hdr, "SMK", 3)) {
        rt_log("SmackOpen(%s): not a Smacker file", nm);
        if (own) vfs_close(vf); else vfs_seek(vf, start, SEEK_SET);
        RET_STDCALL(3, 0);
    }
    uint32_t w = rd32le(hdr + 4), h = rd32le(hdr + 8), frames = rd32le(hdr + 12);
    int32_t rate = (int32_t)rd32le(hdr + 16);
    uint32_t sflags = rd32le(hdr + 20), trees = rd32le(hdr + 52);
    uint32_t n = frames + (sflags & 1); /* ring frame */
    uint8_t *sizes = malloc(n * 4);
    vfs_read(vf, sizes, n * 4);
    uint64_t total = 104 + 5ull * n + trees;
    for (uint32_t i = 0; i < n; i++) total += rd32le(sizes + 4 * i) & ~3u;
    free(sizes);
    uint8_t *data = malloc(total);
    vfs_seek(vf, start, SEEK_SET);
    size_t got = vfs_read(vf, data, total);
    if (own) vfs_close(vf);
    if (getenv("PORT_DUMP_SMK")) {
        char p[1200];
        snprintf(p, sizeof p, "%s/smk_%lld.smk", getenv("PORT_DUMP_SMK"), (long long)start);
        FILE *df = fopen(p, "wb");
        if (df) { fwrite(data, 1, got, df); fclose(df); }
        rt_log("SmackOpen: start %lld total %llu got %zu", (long long)start, (unsigned long long)total, got);
    }
    /* the FEC container stores its Smacker files as "SMK0": version 2 data */
    if (got >= 4 && data[3] == '0') data[3] = '2';
    Movie *mv = got == total ? movie_open(data, (int64_t)total, nm) : NULL;
    if (!mv) { if (got != total) free(data); RET_STDCALL(3, 0); }
    int i = find(0);
    for (i = 0; i < MAX_SMK && g_smk[i].g; i++) {}
    if (i == MAX_SMK) { movie_close(mv); RET_STDCALL(3, 0); }
    uint32_t g = heap_alloc(S_SIZE, 1);
    memset(&g_smk[i], 0, sizeof g_smk[i]);
    g_smk[i].g = g;
    g_smk[i].mv = mv;
    g_smk[i].sound = 1;
    /* frame rate field: > 0 ms per frame, < 0 hundredths of ms, 0 = 10 fps */
    g_smk[i].mspf = rate > 0 ? rate : rate < 0 ? -rate / 100.0 : 100.0;
    g_smk[i].start_us = platform_ticks_us();
    WR32(g + S_VERSION, rd32le(hdr));
    WR32(g + S_WIDTH, w);
    WR32(g + S_HEIGHT, h);
    WR32(g + S_FRAMES, frames);
    WR32(g + S_MSPERFRAME, (uint32_t)(g_smk[i].mspf * 1000));
    WR32(g + S_TYPE, sflags);
    WR32(g + S_OPENFLAGS, flags);
    rt_log("SmackOpen(%s) %ux%u, %u frames, %.1f ms/frame -> %08x", nm, w, h, frames, g_smk[i].mspf, g);
    RET_STDCALL(3, g);
}

/* SmackClose(smk) */
void imp_SMACKW32_ord18(CPU *cpu) {
    int i = find(ARG(0));
    if (i >= 0) {
        movie_close(g_smk[i].mv);
        movie_audio_stop();
        heap_free(g_smk[i].g);
        g_smk[i].g = 0;
    }
    RET_STDCALL(1, 0);
}

/* SmackToBuffer(smk, left, top, pitch, height, buf, flags) */
void imp_SMACKW32_ord23(CPU *cpu) {
    int i = find(ARG(0));
    if (i >= 0) {
        g_smk[i].left = ARG(1);
        g_smk[i].top = ARG(2);
        g_smk[i].pitch = ARG(3);
        g_smk[i].height = ARG(4);
        g_smk[i].buf = ARG(5);
        g_smk[i].flags = ARG(6);
    }
    RET_STDCALL(7, 0);
}

/* SmackDoFrame(smk): decodes the current frame into the buffer */
void imp_SMACKW32_ord19(CPU *cpu) {
    int i = find(ARG(0));
    if (i < 0) RET_STDCALL(1, 0);
    uint32_t g = g_smk[i].g;
    int want = (int)RD32(g + S_FRAMENUM);
    while (g_smk[i].decoded <= want) {
        if (!movie_next_frame(g_smk[i].mv)) break;
        g_smk[i].decoded++;
    }
    int pitch, changed;
    const uint32_t *pal;
    const uint8_t *px = movie_frame_pal8(g_smk[i].mv, &pitch, &pal, &changed);
    uint32_t w = RD32(g + S_WIDTH), h = RD32(g + S_HEIGHT);
    if (px && g_smk[i].buf) {
        int rev = (g_smk[i].flags & 0x80000000u) != 0; /* SMACKBUFFERREVERSED */
        for (uint32_t y = 0; y < h && y < g_smk[i].height; y++) {
            uint32_t dy = rev ? g_smk[i].height - 1 - y - g_smk[i].top : y + g_smk[i].top;
            memcpy(GPTR(g_smk[i].buf + dy * g_smk[i].pitch + g_smk[i].left), px + (size_t)y * pitch, w);
        }
    }
    if (px && changed) {
        for (int k = 0; k < 256; k++) {
            WR8(g + S_PALETTE + 3 * k, (pal[k] >> 16) & 255);
            WR8(g + S_PALETTE + 3 * k + 1, (pal[k] >> 8) & 255);
            WR8(g + S_PALETTE + 3 * k + 2, pal[k] & 255);
        }
    }
    WR32(g + S_NEWPAL, px && changed);
    WR32(g + S_RECTX, 0);
    WR32(g + S_RECTY, 0);
    WR32(g + S_RECTW, w);
    WR32(g + S_RECTH, h);
    g_smk[i].rect_pending = 1;
    movie_pump_audio(g_smk[i].mv);
    RET_STDCALL(1, 0);
}

/* SmackNextFrame(smk) */
void imp_SMACKW32_ord21(CPU *cpu) {
    int i = find(ARG(0));
    if (i >= 0) WR32(g_smk[i].g + S_FRAMENUM, RD32(g_smk[i].g + S_FRAMENUM) + 1);
    RET_STDCALL(1, 0);
}

/* SmackGoto(smk, frame) */
void imp_SMACKW32_ord27(CPU *cpu) {
    int i = find(ARG(0));
    if (i >= 0) {
        int f = (int)ARG(1);
        if (f < g_smk[i].decoded) {
            movie_rewind(g_smk[i].mv);
            g_smk[i].decoded = 0;
        }
        WR32(g_smk[i].g + S_FRAMENUM, (uint32_t)f);
        g_smk[i].start_us = platform_ticks_us() - (uint64_t)(f * g_smk[i].mspf * 1000);
    }
    RET_STDCALL(2, 0);
}

/* SmackToBufferRect(smk, surface): the changed rectangle, once per frame */
void imp_SMACKW32_ord28(CPU *cpu) {
    int i = find(ARG(0));
    if (i < 0 || !g_smk[i].rect_pending) RET_STDCALL(2, 0);
    g_smk[i].rect_pending = 0;
    RET_STDCALL(2, 1);
}

/* SmackWait(smk): nonzero while it is too early for the next frame */
void imp_SMACKW32_ord32(CPU *cpu) {
    int i = find(ARG(0));
    if (i < 0) RET_STDCALL(1, 0);
    movie_pump_audio(g_smk[i].mv);
    uint64_t due = g_smk[i].start_us + (uint64_t)(RD32(g_smk[i].g + S_FRAMENUM) * g_smk[i].mspf * 1000);
    uint64_t now = platform_ticks_us();
    if (now < due) {
        platform_sleep_ms(1);
        RET_STDCALL(1, 1);
    }
    RET_STDCALL(1, 0);
}

/* SmackSoundOnOff(smk, on) */
void imp_SMACKW32_ord17(CPU *cpu) {
    int i = find(ARG(0));
    if (i >= 0) g_smk[i].sound = ARG(1) != 0;
    movie_audio_set_volume(ARG(1) ? 1.0f : 0.0f);
    RET_STDCALL(2, 0);
}

void imp_SMACKW32_ord38(CPU *cpu) { RET_STDCALL(1, 1); } /* SmackSoundUseDirectSound(ds) */

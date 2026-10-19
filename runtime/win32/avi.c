/* MCI "avivideo": the game's DVD-edition movies on FFmpeg.
 * The exe opens "<cwd>\ClawN.vob" (OPEN with an element, a child window style and the
 * main window as parent), sizes it to the screen (PUT), PLAYs with MCI_NOTIFY and then
 * spins in PeekMessage until its window gets MM_MCINOTIFY (Esc skips: GetAsyncKeyState).
 * Frames are decoded and presented from the message-loop idle hook, timed by the audio.
 * Port option [movies] dvd_avi (default on): a missing ClawN.vob falls back to the
 * CLAWN.avi files the community pack ships (otherwise the game uses its Smacker cutscenes). */
#include <string.h>
#include <strings.h>
#include "win32.h"
#include "wnd.h"
#include "../pack/vfs.h"
#include "../movie.h"
#include "../gamecfg.h"

#define MCIERR_INVALID_DEVICE_NAME 263
#define MCIERR_FILE_NOT_FOUND 275
#define AVI_DEVICE_ID 0x20u
#define MM_MCINOTIFY 0x3b9
#define MCI_NOTIFY_SUCCESSFUL 1
#define MCI_NOTIFY_ABORTED 4

static struct {
    int open, playing;
    Movie *mv;
    uint32_t callback;     /* hwnd for MM_MCINOTIFY */
    int notify;
    uint64_t start_us;     /* wall clock start (movies without audio) */
    int have_frame, eof;
    double next_t;         /* time of the decoded frame waiting to be shown */
    int shown, dropped;    /* stats */
    uint64_t decode_us, convert_us;
} g_avi;

int avi_is_device(uint32_t dev) { return g_avi.open && dev == AVI_DEVICE_ID; }

/* "...\ClawN.vob" -> a VFS path that exists (the .vob itself or the pack's .avi) */
static int resolve(const char *guest, char *rel, size_t n) {
    guest_rel_path(guest, rel, n);
    int is_dir;
    if (vfs_stat(rel, &is_dir, NULL) == 0 && !is_dir) return 1;
    size_t l = strlen(rel);
    if (l > 4 && !strcasecmp(rel + l - 4, ".vob") && cfg_int("movies", "dvd_avi", 1)) {
        memcpy(rel + l - 4, ".avi", 4);
        if (vfs_stat(rel, &is_dir, NULL) == 0 && !is_dir) return 1;
    }
    return 0;
}

int avi_file_exists(const char *guest) {
    char rel[1024];
    return resolve(guest, rel, sizeof rel);
}

static void stop(int notify_code) {
    if (g_avi.playing) {
        int n = g_avi.shown + g_avi.dropped;
        rt_log("AVI: %d frames shown, %d dropped; %.1f ms decode, %.1f ms convert+present per frame", g_avi.shown,
               g_avi.dropped, n ? g_avi.decode_us / 1000.0 / n : 0.0, g_avi.shown ? g_avi.convert_us / 1000.0 / g_avi.shown : 0.0);
        g_avi.playing = 0;
        movie_audio_stop();
        if (g_avi.notify && g_avi.callback) wm_post(g_avi.callback, MM_MCINOTIFY, (uint32_t)notify_code, AVI_DEVICE_ID);
        g_avi.notify = 0;
    }
}

int avi_mci_open(uint32_t flags, uint32_t params) {
    const char *el = (flags & 0x200) && RD32(params + 12) ? gstr(RD32(params + 12)) : "";
    char rel[1024];
    if (!resolve(el, rel, sizeof rel)) {
        rt_log("AVI: %s not found", el);
        return MCIERR_FILE_NOT_FOUND;
    }
    if (g_avi.open) {
        stop(MCI_NOTIFY_ABORTED);
        movie_close(g_avi.mv);
        g_avi.open = 0;
    }
    VFile *vf = vfs_open(rel);
    Movie *mv = vf ? movie_open_vfile(vf, rel) : NULL;
    if (!mv) return MCIERR_INVALID_DEVICE_NAME;
    memset(&g_avi, 0, sizeof g_avi);
    g_avi.open = 1;
    g_avi.mv = mv;
    WR32(params + 4, AVI_DEVICE_ID);
    rt_log("AVI: opened %s", rel);
    return 0;
}

int avi_mci_command(uint32_t dev, uint32_t msg, uint32_t flags, uint32_t params) {
    (void)dev;
    switch (msg) {
    case 0x804: /* MCI_CLOSE */
        stop(MCI_NOTIFY_ABORTED);
        movie_close(g_avi.mv);
        g_avi.mv = NULL;
        g_avi.open = 0;
        return 0;
    case 0x806: /* MCI_PLAY */
        if (g_avi.playing) stop(MCI_NOTIFY_ABORTED);
        g_avi.callback = params ? RD32(params) : 0;
        g_avi.notify = (flags & 1) != 0;
        g_avi.playing = 1;
        g_avi.have_frame = g_avi.eof = 0;
        g_avi.shown = g_avi.dropped = 0;
        g_avi.decode_us = g_avi.convert_us = 0;
        g_avi.start_us = platform_ticks_us();
        if (movie_has_audio(g_avi.mv)) movie_pump_audio(g_avi.mv);
        return 0;
    case 0x808: /* MCI_STOP */
    case 0x809: /* MCI_PAUSE */
        stop(MCI_NOTIFY_ABORTED);
        return 0;
    case 0x814: /* MCI_STATUS */
        if (params) {
            uint32_t item = RD32(params + 8), r = 0;
            if (item == 4) r = g_avi.playing ? 526 : 525; /* MCI_MODE_PLAY / STOP */
            else if (item == 5 || item == 7) r = 1;
            WR32(params + 4, r);
        }
        return 0;
    default: /* PUT, WINDOW, WHERE, REALIZE...: the movie is shown full screen */
        return 0;
    }
}

/* movie clock in seconds: the audio played so far, or wall time without audio */
static double clock_now(void) {
    if (movie_has_audio(g_avi.mv)) return movie_audio_clock();
    return (double)(platform_ticks_us() - g_avi.start_us) / 1e6;
}

/* called from the message loop while the game waits for the movie */
void avi_idle(void) {
    if (!g_avi.playing) return;
    Movie *m = g_avi.mv;
    if (movie_has_audio(m)) movie_pump_audio(m);
    double now = clock_now();
    int shown = 0;
    for (;;) {
        if (!g_avi.have_frame) {
            uint64_t t0 = platform_ticks_us();
            int ok = !g_avi.eof && movie_next_frame(m);
            g_avi.decode_us += platform_ticks_us() - t0;
            if (!ok) {
                g_avi.eof = 1;
                break;
            }
            g_avi.have_frame = 1;
            g_avi.next_t = movie_frame_time(m);
        }
        if (g_avi.next_t > now) break;
        g_avi.have_frame = 0;
        /* frames more than 50 ms late are dropped (decoded, not shown) */
        if (g_avi.next_t + 0.05 < now) { g_avi.dropped++; continue; }
        uint64_t t0 = platform_ticks_us();
        const uint32_t *px = movie_frame_rgb(m);
        int w = movie_width(m), h = movie_height(m);
        platform_present(px, w, h, w * 4, PIX_XRGB8888);
        g_avi.convert_us += platform_ticks_us() - t0;
        g_avi.shown++;
        shown = 1;
        break;
    }
    if (g_avi.eof && !g_avi.have_frame && (!movie_has_audio(m) || movie_audio_queued() < 64)) {
        rt_log("AVI: finished");
        stop(MCI_NOTIFY_SUCCESSFUL);
        return;
    }
    if (!shown) platform_sleep_ms(1);
}

int avi_playing(void) { return g_avi.playing; }

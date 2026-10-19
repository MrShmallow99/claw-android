/* MCI CD audio emulation.
 *
 * The PC game plays its music from the CD's audio tracks.  Those tracks are
 * read from music/trackNN.ogg or music/trackNN.wav (in the save or game
 * folder).  Without any music files the drive reports "not ready" and the game
 * runs without music, as it would without the CD.
 *
 * Protocol used by the game: MCI_OPEN cdaudio, MCI_STATUS (mode, track type,
 * track length in MSF), MCI_SET time format, MCI_PLAY from/to in TMSF,
 * MCI_STOP, MCI_CLOSE.  It loops tracks itself using their length. */
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "win32.h"
#include "music.h"

#define MCIERR_DEVICE_NOT_READY 276
#define MCIERR_OUTOFRANGE 265
#define MCI_OPEN 0x803
#define MCI_CLOSE 0x804
#define MCI_PLAY 0x806
#define MCI_SEEK 0x807
#define MCI_STOP 0x808
#define MCI_PAUSE 0x809
#define MCI_SET 0x80d
#define MCI_STATUS 0x814
#define MCI_RESUME 0x855
#define MCI_FROM 0x4
#define MCI_TO 0x8
#define MCI_STATUS_ITEM 0x100
#define MCI_TRACK 0x10
#define MCI_SET_TIME_FORMAT 0x400
#define MCI_FORMAT_MILLISECONDS 0
#define MCI_FORMAT_MSF 2
#define MCI_FORMAT_TMSF 10
#define MCI_MODE_STOP 525
#define MCI_MODE_PLAY 526
#define MCI_CDA_TRACK_AUDIO 1088
#define MCI_CDA_TRACK_OTHER 1089
#define CD_DEVICE_ID 1

static int g_open;
static uint32_t g_time_format = MCI_FORMAT_MSF;

static uint32_t to_msf(uint32_t secs) { return (secs / 60) | ((secs % 60) << 8); }

int avi_mci_open(uint32_t flags, uint32_t params);
int avi_mci_command(uint32_t dev, uint32_t msg, uint32_t flags, uint32_t params);
int avi_is_device(uint32_t dev);

static int is_avi_open(uint32_t flags, uint32_t params) {
    char ty[64] = "", el[512] = "";
    if (!params) return 0;
    if ((flags & 0x2000) && RD32(params + 8)) { /* MCI_OPEN_TYPE (not by id) */
        if (!(flags & 0x1000)) snprintf(ty, sizeof ty, "%s", gstr(RD32(params + 8)));
    }
    if ((flags & 0x200) && RD32(params + 12)) snprintf(el, sizeof el, "%s", gstr(RD32(params + 12))); /* MCI_OPEN_ELEMENT */
    size_t n = strlen(el);
    return !strcasecmp(ty, "avivideo") || (n > 4 && (!strcasecmp(el + n - 4, ".avi") || !strcasecmp(el + n - 4, ".vob")));
}

int mci_command(uint32_t dev, uint32_t msg, uint32_t flags, uint32_t params) {
    TRACE("mciSendCommandA(dev=%u, msg=%04x, flags=%08x)", dev, msg, flags);
    if (msg == MCI_OPEN && is_avi_open(flags, params)) return avi_mci_open(flags, params);
    if (msg != MCI_OPEN && avi_is_device(dev)) return avi_mci_command(dev, msg, flags, params);
    switch (msg) {
    case MCI_OPEN:
        if (music_track_count() <= 0) {
            rt_log("CD audio: no music tracks found (music/trackNN.ogg|wav)");
            return MCIERR_DEVICE_NOT_READY;
        }
        if (params) WR32(params + 4, CD_DEVICE_ID);
        g_open = 1;
        rt_log("CD audio: opened, %d tracks", music_track_count());
        return 0;
    }
    if (!g_open || dev != CD_DEVICE_ID) return MCIERR_DEVICE_NOT_READY;
    switch (msg) {
    case MCI_CLOSE:
        music_stop();
        g_open = 0;
        return 0;
    case MCI_SET:
        if (flags & MCI_SET_TIME_FORMAT) g_time_format = RD32(params + 4);
        return 0;
    case MCI_STATUS: {
        uint32_t item = RD32(params + 8), track = RD32(params + 12), ret = 0;
        switch (item) {
        case 1: /* MCI_STATUS_LENGTH */
            if (flags & MCI_TRACK) {
                int len = music_track_length(track);
                if (len < 0) return MCIERR_OUTOFRANGE;
                ret = g_time_format == MCI_FORMAT_MILLISECONDS ? (uint32_t)len * 1000 : to_msf((uint32_t)len);
            } else {
                ret = to_msf(60 * 60);
            }
            break;
        case 3: ret = (uint32_t)music_track_count() + 1; break; /* NUMBER_OF_TRACKS (track 1 = data) */
        case 4: ret = music_playing() ? MCI_MODE_PLAY : MCI_MODE_STOP; break;
        case 5: case 7: ret = 1; break; /* MEDIA_PRESENT, READY */
        case 0x4001: /* MCI_CDA_STATUS_TYPE_TRACK */
            ret = music_track_length(track) >= 0 ? MCI_CDA_TRACK_AUDIO : MCI_CDA_TRACK_OTHER;
            break;
        default:
            TRACE("MCI_STATUS item %x not handled", item);
        }
        WR32(params + 4, ret);
        return 0;
    }
    case MCI_PLAY: {
        uint32_t from = (flags & MCI_FROM) ? RD32(params + 4) : 0;
        uint32_t to = (flags & MCI_TO) ? RD32(params + 8) : 0;
        int track = 0;
        double start = 0, end = -1;
        if (g_time_format == MCI_FORMAT_TMSF) {
            track = from & 0xff;
            start = ((from >> 8) & 0xff) * 60 + ((from >> 16) & 0xff) + ((from >> 24) & 0xff) / 75.0;
            if ((flags & MCI_TO) && (int)(to & 0xff) == track)
                end = ((to >> 8) & 0xff) * 60 + ((to >> 16) & 0xff) + ((to >> 24) & 0xff) / 75.0;
        }
        rt_log("CD audio: play track %d from %.1fs", track, start);
        return music_play(track, start, end) ? 0 : MCIERR_OUTOFRANGE;
    }
    case MCI_STOP:
    case MCI_PAUSE:
        music_stop();
        return 0;
    case MCI_SEEK:
    case MCI_RESUME:
        return 0;
    }
    return 0;
}

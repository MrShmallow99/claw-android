/* Movie decoding with a minimal FFmpeg (tools/build_ffmpeg.sh): demux from memory,
 * decode video on demand, push audio to the movie audio ring (movie_audio.c). */
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include "runtime.h"
#include "movie.h"
#include "pack/vfs.h"

#define VQ 64

struct Movie {
    VFile *vf;            /* streamed source (AVI), or */
    uint8_t *data;        /* the whole file in memory (Smacker) */
    int64_t size, pos;
    AVIOContext *io;
    AVFormatContext *fmt;
    AVCodecContext *vdec, *adec;
    int vs, as;
    AVFrame *frame, *aframe;
    SwrContext *swr;
    struct SwsContext *sws;
    AVPacket *vq[VQ];
    int vq_n;
    int eof;
    uint32_t pal[256];
    int pal_valid;
    uint32_t *rgb;
    uint8_t *rgb_planes[4];
    char name[64];
};

static int io_read(void *o, uint8_t *buf, int n) {
    Movie *m = o;
    if (m->vf) {
        size_t got = vfs_read(m->vf, buf, (size_t)n);
        return got ? (int)got : AVERROR_EOF;
    }
    int64_t left = m->size - m->pos;
    if (left <= 0) return AVERROR_EOF;
    if (n > left) n = (int)left;
    memcpy(buf, m->data + m->pos, (size_t)n);
    m->pos += n;
    return n;
}

static int64_t io_seek(void *o, int64_t off, int wh) {
    Movie *m = o;
    if (wh == AVSEEK_SIZE) return m->size;
    if (m->vf) {
        wh &= ~AVSEEK_FORCE;
        return vfs_seek(m->vf, off, wh);
    }
    wh &= ~AVSEEK_FORCE;
    int64_t p = wh == SEEK_SET ? off : wh == SEEK_CUR ? m->pos + off : m->size + off;
    if (p < 0 || p > m->size) return -1;
    m->pos = p;
    return p;
}

static AVCodecContext *open_dec(AVStream *st) {
    const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c) return NULL;
    AVCodecContext *d = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(d, st->codecpar);
    d->thread_count = 1;
    if (avcodec_open2(d, c, NULL) < 0) { avcodec_free_context(&d); return NULL; }
    return d;
}

static int open_streams(Movie *m) {
    m->pos = 0;
    if (m->vf) vfs_seek(m->vf, 0, SEEK_SET);
    m->io = avio_alloc_context(av_malloc(65536), 65536, 0, m, io_read, NULL, io_seek);
    m->fmt = avformat_alloc_context();
    m->fmt->pb = m->io;
    int r = avformat_open_input(&m->fmt, NULL, NULL, NULL);
    if (r < 0) { rt_log("movie %s: avformat_open_input: %s", m->name, av_err2str(r)); return -1; }
    avformat_find_stream_info(m->fmt, NULL);
    m->vs = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    m->as = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (m->vs < 0) { rt_log("movie %s: no video stream", m->name); return -1; }
    m->vdec = open_dec(m->fmt->streams[m->vs]);
    if (!m->vdec) { rt_log("movie %s: no decoder for %s", m->name, avcodec_get_name(m->fmt->streams[m->vs]->codecpar->codec_id)); return -1; }
    if (m->as >= 0) {
        m->adec = open_dec(m->fmt->streams[m->as]);
        if (!m->adec) m->as = -1;
    }
    m->eof = 0;
    return 0;
}

static void close_streams(Movie *m) {
    for (int i = 0; i < m->vq_n; i++) av_packet_free(&m->vq[i]);
    m->vq_n = 0;
    avcodec_free_context(&m->vdec);
    avcodec_free_context(&m->adec);
    if (m->fmt) avformat_close_input(&m->fmt);
    if (m->io) { av_freep(&m->io->buffer); avio_context_free(&m->io); }
    swr_free(&m->swr);
}

Movie *movie_open(uint8_t *data, int64_t size, const char *name) {
    Movie *m = calloc(1, sizeof *m);
    m->data = data;
    m->size = size;
    snprintf(m->name, sizeof m->name, "%s", name);
    m->frame = av_frame_alloc();
    m->aframe = av_frame_alloc();
    if (open_streams(m)) {
        rt_log("movie %s: cannot open", name);
        movie_close(m);
        return NULL;
    }
    rt_log("movie %s: %dx%d %.2f fps, %d frames, video %s, audio %s", name, movie_width(m), movie_height(m),
           movie_fps(m), movie_frames(m), avcodec_get_name(m->vdec->codec_id),
           m->adec ? avcodec_get_name(m->adec->codec_id) : "none");
    return m;
}

Movie *movie_open_vfile(VFile *vf, const char *name) {
    Movie *m = calloc(1, sizeof *m);
    m->vf = vf;
    m->size = vfs_size(vf);
    snprintf(m->name, sizeof m->name, "%s", name);
    m->frame = av_frame_alloc();
    m->aframe = av_frame_alloc();
    if (open_streams(m)) {
        rt_log("movie %s: cannot open", name);
        movie_close(m);
        return NULL;
    }
    rt_log("movie %s: %dx%d %.2f fps, video %s, audio %s", name, movie_width(m), movie_height(m), movie_fps(m),
           avcodec_get_name(m->vdec->codec_id), m->adec ? avcodec_get_name(m->adec->codec_id) : "none");
    return m;
}

double movie_frame_time(Movie *m) {
    int64_t t = m->frame->best_effort_timestamp;
    if (t == AV_NOPTS_VALUE) t = m->frame->pts;
    if (t == AV_NOPTS_VALUE) return -1;
    return t * av_q2d(m->fmt->streams[m->vs]->time_base);
}

void movie_close(Movie *m) {
    if (!m) return;
    close_streams(m);
    if (m->vf) vfs_close(m->vf);
    av_frame_free(&m->frame);
    av_frame_free(&m->aframe);
    sws_freeContext(m->sws);
    free(m->rgb);
    free(m->data);
    free(m);
}

int movie_width(Movie *m) { return m->vdec->width; }
int movie_height(Movie *m) { return m->vdec->height; }
int movie_has_audio(Movie *m) { return m->adec != NULL; }
double movie_fps(Movie *m) {
    AVRational r = m->fmt->streams[m->vs]->avg_frame_rate;
    if (!r.num) r = m->fmt->streams[m->vs]->r_frame_rate;
    return r.num ? av_q2d(r) : 15.0;
}
int movie_frames(Movie *m) { return (int)m->fmt->streams[m->vs]->nb_frames; }

static void decode_audio(Movie *m, AVPacket *pkt) {
    if (avcodec_send_packet(m->adec, pkt) < 0) return;
    while (avcodec_receive_frame(m->adec, m->aframe) == 0) {
        AVFrame *f = m->aframe;
        if (!m->swr) {
            AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
            swr_alloc_set_opts2(&m->swr, &out, AV_SAMPLE_FMT_FLT, f->sample_rate, &f->ch_layout, f->format,
                                f->sample_rate, 0, NULL);
            swr_init(m->swr);
            movie_audio_start(f->sample_rate);
        }
        float buf[8192 * 2];
        uint8_t *outp[1] = {(uint8_t *)buf};
        int n = swr_convert(m->swr, outp, 8192, (const uint8_t **)f->extended_data, f->nb_samples);
        if (n > 0) movie_audio_push(buf, n);
    }
}

/* reads one packet: audio is decoded at once, video is queued */
static int read_one(Movie *m) {
    if (m->eof) return 0;
    AVPacket *p = av_packet_alloc();
    if (av_read_frame(m->fmt, p) < 0) {
        av_packet_free(&p);
        m->eof = 1;
        return 0;
    }
    if (p->stream_index == m->as && m->adec) {
        decode_audio(m, p);
        av_packet_free(&p);
    } else if (p->stream_index == m->vs && m->vq_n < VQ) {
        m->vq[m->vq_n++] = p;
    } else {
        av_packet_free(&p);
    }
    return 1;
}

void movie_pump_audio(Movie *m) {
    if (!m->adec) return;
    int want = m->adec->sample_rate ? m->adec->sample_rate : 22050;
    while (movie_audio_queued() < want && m->vq_n < VQ && read_one(m)) {}
}

int movie_next_frame(Movie *m) {
    for (;;) {
        int r = avcodec_receive_frame(m->vdec, m->frame);
        if (r == 0) {
            if (m->frame->format == AV_PIX_FMT_PAL8 && m->frame->data[1]) {
                const uint32_t *p = (const uint32_t *)m->frame->data[1];
                for (int i = 0; i < 256; i++) m->pal[i] = p[i] & 0xffffff;
            }
            return 1;
        }
        if (r == AVERROR_EOF) return 0;
        while (!m->vq_n && read_one(m)) {}
        if (!m->vq_n) {
            avcodec_send_packet(m->vdec, NULL); /* drain */
            if (avcodec_receive_frame(m->vdec, m->frame) == 0) return 1;
            return 0;
        }
        AVPacket *p = m->vq[0];
        memmove(m->vq, m->vq + 1, sizeof(AVPacket *) * (size_t)(--m->vq_n));
        avcodec_send_packet(m->vdec, p);
        av_packet_free(&p);
    }
}

const uint8_t *movie_frame_pal8(Movie *m, int *pitch, const uint32_t **pal, int *pal_changed) {
    if (m->frame->format != AV_PIX_FMT_PAL8) return NULL;
    *pitch = m->frame->linesize[0];
    static uint32_t prev[256];
    *pal_changed = !m->pal_valid || memcmp(prev, m->pal, sizeof prev) != 0;
    memcpy(prev, m->pal, sizeof prev);
    m->pal_valid = 1;
    *pal = m->pal;
    return m->frame->data[0];
}

const uint32_t *movie_frame_rgb(Movie *m) {
    int w = m->frame->width, h = m->frame->height;
    if (!m->rgb) m->rgb = malloc((size_t)w * h * 4);
    m->sws = sws_getCachedContext(m->sws, w, h, m->frame->format, w, h, AV_PIX_FMT_BGR0, SWS_BILINEAR, NULL, NULL, NULL);
    uint8_t *dst[4] = {(uint8_t *)m->rgb};
    int dl[4] = {w * 4};
    sws_scale(m->sws, (const uint8_t *const *)m->frame->data, m->frame->linesize, 0, h, dst, dl);
    return m->rgb;
}

int movie_rewind(Movie *m) {
    close_streams(m);
    m->pal_valid = 0;
    return open_streams(m);
}

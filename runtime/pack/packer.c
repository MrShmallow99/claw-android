/* On-device packer: builds game.pak from an imported game folder, the same way
 * tools/mkpack.py does on the desktop, so the folder-picker app can compress the
 * files it imported (an option on the setup screen).
 *
 *   - the [pack] rules of the compiled-in game.ini choose the handler of each file
 *     (skip, stored, lzma, solid, wav; other handlers fall back to lzma);
 *   - byte-identical files are stored once (aliases);
 *   - solid: the files of one folder, in name order, up to 8 MB per group, as one
 *     PIECES stream, with 16-bit PCM sounds as FLAC pieces when FLAC beats 60%;
 *   - LZMA: .lzma "alone" streams, level 9 with nice_len 273 (mkpack: preset 9e),
 *     the dictionary sized to the data, in 4 MB residual blocks;
 *   - FLAC: FFmpeg's encoder at compression level 12 (mkpack runs the ffmpeg CLI);
 *   - the same index layout and entry order (runtime/pack/pack_format.h).
 * Then every file is read back through the runtime VFS from the new pack and
 * compared with the original before anything is deleted.
 * Compression runs on several threads; memory is about 100 MB per thread. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "pack_format.h"
#include "vfs.h"
#include "../gamecfg.h"
#include "../../third_party/lzma-enc/LzmaEnc.h"
#ifdef PORT_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#endif

void rt_log(const char *fmt, ...);
void platform_set_dirs(const char *game, const char *save);

#define SOLID_GROUP (8u << 20)
#define RESID_BLOCK (4u << 20)

enum { H_SKIP, H_STORED, H_LZMA, H_SOLID, H_WAV };

typedef struct {
    char *name;      /* '/'-separated, relative to the game folder */
    char *path;
    int64_t size;
    int handler;
    uint64_t hash;
    int dup_of;      /* index of the first file with the same bytes, or -1 */
    int group;       /* solid group, or -1 */
    uint64_t goff;   /* offset in the group's stream */
    /* placement */
    int kind;
    uint64_t off, len;
} PFile;

typedef struct {
    int first, count;   /* range in the group order array */
    uint64_t blob_len;
    uint64_t off, len;
} Group;

typedef struct {
    int file;           /* >= 0: a file entry; else -1 - group */
    uint8_t *body;
    size_t len;
    int kind;
    int done, written;
} Job;

typedef struct {
    PFile *f;
    int nf, capf;
    Group *g;
    int ng;
    int *gorder;        /* file indices of solid members, in group order */
    Job *jobs;
    int njobs, next_job;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int64_t done_bytes, total_bytes;
    int failed;
    char err[256];
} Packer;

/* ---- helpers -------------------------------------------------------------------------- */
static uint8_t *read_all(const char *path, int64_t *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    if (b && n > 0 && fread(b, 1, (size_t)n, fp) != (size_t)n) { free(b); b = NULL; }
    fclose(fp);
    if (b) *len = n;
    return b;
}

static uint64_t fnv64(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

typedef struct { uint8_t *p; size_t n, cap; } Buf;
static void buf_put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 4096;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put32(Buf *b, uint32_t v) { uint8_t t[4] = {v, v >> 8, v >> 16, v >> 24}; buf_put(b, t, 4); }
static void put64(Buf *b, uint64_t v) { put32(b, (uint32_t)v); put32(b, (uint32_t)(v >> 32)); }

/* ---- LZMA ("alone": 5 property bytes, u64 size, stream) ----------------------------- */
static void *lz_alloc(ISzAllocPtr p, size_t size) { (void)p; return malloc(size); }
static void lz_free(ISzAllocPtr p, void *a) { (void)p; free(a); }
static const ISzAlloc g_alloc = {lz_alloc, lz_free};

static uint8_t *lzma_alone(const uint8_t *src, size_t n, size_t *out_len) {
    CLzmaEncProps pr;
    LzmaEncProps_Init(&pr);
    pr.level = 9;
    uint32_t d = n < (1u << 16) ? (1u << 16) : n > (1u << 26) ? (1u << 26) : (uint32_t)n;
    pr.dictSize = d;
    pr.lc = 3; pr.lp = 0; pr.pb = 2;
    pr.algo = 1; pr.fb = 273; pr.btMode = 1; pr.numHashBytes = 4; pr.mc = 512;
    pr.numThreads = 1;
    pr.reduceSize = n;
    size_t cap = n + n / 3 + 4096;
    uint8_t *out = malloc(cap + 13);
    if (!out) return NULL;
    SizeT dlen = cap, plen = 5;
    if (LzmaEncode(out + 13, &dlen, src, n, &pr, out, &plen, 0, NULL, &g_alloc, &g_alloc) != SZ_OK || plen != 5) {
        free(out);
        return NULL;
    }
    for (int i = 0; i < 8; i++) out[5 + i] = (uint8_t)((uint64_t)n >> (8 * i));
    *out_len = dlen + 13;
    return out;
}

/* ---- FLAC: 16-bit PCM -> "fLaC" + STREAMINFO + frames (mkpack's strip_flac output) --- */
static uint8_t *flac_encode_s16(const uint8_t *pcm, size_t nbytes, int ch, int rate, size_t *out_len) {
#ifdef PORT_HAVE_FFMPEG
    static int logged;
    const AVCodec *c = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!c) { if (!logged++) rt_log("packer: no FLAC encoder in this build: 16-bit sounds use LZMA"); return NULL; }
    AVCodecContext *x = avcodec_alloc_context3(c);
    x->sample_fmt = AV_SAMPLE_FMT_S16;
    x->sample_rate = rate;
    av_channel_layout_default(&x->ch_layout, ch);
    x->compression_level = 12;
    int oe = avcodec_open2(x, c, NULL);
    if (oe < 0) {
        if (!logged++) rt_log("packer: FLAC encoder failed to open (%d): 16-bit sounds use LZMA", oe);
        avcodec_free_context(&x);
        return NULL;
    }
    uint8_t sinfo[34] = {0};
    if (x->extradata_size >= 34) memcpy(sinfo, x->extradata, 34);
    Buf frames = {0};
    AVFrame *fr = av_frame_alloc();
    AVPacket *pk = av_packet_alloc();
    size_t total = nbytes / (2 * (size_t)ch), pos = 0;
    int fs = x->frame_size > 0 ? x->frame_size : 4608, ok = 1;
    for (;;) {
        int r;
        if (pos < total) {
            int n = (int)(total - pos < (size_t)fs ? total - pos : (size_t)fs);
            fr->nb_samples = n;
            fr->format = AV_SAMPLE_FMT_S16;
            fr->sample_rate = rate;
            av_channel_layout_copy(&fr->ch_layout, &x->ch_layout);
            if (av_frame_get_buffer(fr, 0) < 0) { ok = 0; break; }
            memcpy(fr->data[0], pcm + pos * 2 * (size_t)ch, (size_t)n * 2 * (size_t)ch);
            r = avcodec_send_frame(x, fr);
            av_frame_unref(fr);
            pos += (size_t)n;
        } else {
            r = avcodec_send_frame(x, NULL);
        }
        if (r < 0 && r != AVERROR_EOF) { ok = 0; break; }
        int eof = 0;
        while ((r = avcodec_receive_packet(x, pk)) >= 0) {
            size_t sl = 0;
            const uint8_t *sd = av_packet_get_side_data(pk, AV_PKT_DATA_NEW_EXTRADATA, &sl);
            if (sd && sl >= 34) memcpy(sinfo, sd, 34);   /* the final STREAMINFO (sample count, MD5) */
            if (pk->size) buf_put(&frames, pk->data, (size_t)pk->size);
            av_packet_unref(pk);
        }
        if (r == AVERROR_EOF) eof = 1;
        else if (r != AVERROR(EAGAIN)) { ok = 0; break; }
        if (eof) break;
    }
    av_packet_free(&pk);
    av_frame_free(&fr);
    avcodec_free_context(&x);
    if (!ok) {
        if (!logged++) rt_log("packer: FLAC encoding failed: 16-bit sounds use LZMA");
        free(frames.p);
        return NULL;
    }
    Buf out = {0};
    buf_put(&out, "fLaC\x80\x00\x00\x22", 8);   /* last metadata block: STREAMINFO, 34 bytes */
    buf_put(&out, sinfo, 34);
    buf_put(&out, frames.p, frames.n);
    free(frames.p);
    *out_len = out.n;
    return out.p;
#else
    static int logged;
    if (!logged++) rt_log("packer: built without FFmpeg: 16-bit sounds use LZMA");
    (void)pcm; (void)nbytes; (void)ch; (void)rate; (void)out_len;
    return NULL;
#endif
}

/* (offset, length, channels, rate) of a 16-bit PCM WAV's samples; min_len as mkpack */
static int wav_pcm16_range(const uint8_t *d, size_t n, size_t min_len, size_t *start, size_t *len, int *ch, int *rate) {
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return 0;
    size_t p = 12;
    int have_fmt = 0, tag = 0, bits = 0;
    size_t ds = 0, dl = 0;
    int have_data = 0;
    while (p + 8 <= n) {
        uint32_t ln = d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 | (uint32_t)d[p + 7] << 24;
        if (!memcmp(d + p, "fmt ", 4) && ln >= 16 && p + 8 + 16 <= n) {
            const uint8_t *f = d + p + 8;
            tag = f[0] | f[1] << 8;
            *ch = f[2] | f[3] << 8;
            *rate = (int)(f[4] | f[5] << 8 | f[6] << 16 | (uint32_t)f[7] << 24);
            bits = f[14] | f[15] << 8;
            have_fmt = 1;
        } else if (!memcmp(d + p, "data", 4)) {
            ds = p + 8;
            dl = ln < n - p - 8 ? ln : n - p - 8;
            have_data = 1;
        }
        p += 8 + (size_t)ln + (ln & 1);
    }
    if (!have_fmt || !have_data || tag != 1 || bits != 16 || (*ch != 1 && *ch != 2)) return 0;
    size_t m = dl - dl % (2 * (size_t)*ch);
    if (m < min_len) return 0;
    *start = ds;
    *len = m;
    return 1;
}

/* ---- PACK_PIECES body (mkpack's build_pieces) -------------------------------------- */
typedef struct { size_t start, len; int ch; uint8_t *fl; size_t fl_len; } FlacRange;

static uint8_t *build_pieces(const uint8_t *data, size_t n, FlacRange *fr, int nfr, size_t *out_len) {
    typedef struct { uint64_t out_off; uint32_t out_len; int type, ch; uint64_t arg; uint32_t flen; } Piece;
    Piece *pc = malloc(sizeof(Piece) * (size_t)(2 * nfr + 2));
    int np = 0;
    Buf resid = {0};
    size_t pos = 0;
    for (int i = 0; i < nfr; i++) {
        if (fr[i].start > pos) {
            pc[np++] = (Piece){pos, (uint32_t)(fr[i].start - pos), PIECE_RESID, 0, resid.n, 0};
            buf_put(&resid, data + pos, fr[i].start - pos);
        }
        pc[np++] = (Piece){fr[i].start, (uint32_t)fr[i].len, PIECE_FLAC, fr[i].ch, (uint64_t)i, (uint32_t)fr[i].fl_len};
        pos = fr[i].start + fr[i].len;
    }
    if (pos < n) {
        pc[np++] = (Piece){pos, (uint32_t)(n - pos), PIECE_RESID, 0, resid.n, 0};
        buf_put(&resid, data + pos, n - pos);
    }
    int nb = (int)((resid.n + RESID_BLOCK - 1) / RESID_BLOCK);
    uint8_t **bz = calloc((size_t)nb + 1, sizeof *bz);
    size_t *bzl = calloc((size_t)nb + 1, sizeof *bzl), *braw = calloc((size_t)nb + 1, sizeof *braw);
    int *bcodec = calloc((size_t)nb + 1, sizeof *bcodec);
    for (int b = 0; b < nb; b++) {
        size_t o = (size_t)b * RESID_BLOCK, l = resid.n - o < RESID_BLOCK ? resid.n - o : RESID_BLOCK;
        size_t zl = 0;
        uint8_t *z = lzma_alone(resid.p + o, l, &zl);
        braw[b] = l;
        if (z && zl < l) { bz[b] = z; bzl[b] = zl; bcodec[b] = BLOCK_LZMA; }
        else { free(z); bz[b] = malloc(l ? l : 1); memcpy(bz[b], resid.p + o, l); bzl[b] = l; bcodec[b] = BLOCK_RAW; }
    }
    size_t head = 4 + (size_t)nb * PACK_BLOCK_SIZE + 4 + (size_t)np * PACK_PIECE_SIZE;
    Buf out = {0}, pay = {0};
    put32(&out, (uint32_t)nb);
    for (int b = 0; b < nb; b++) {
        put64(&out, head + pay.n);
        put32(&out, (uint32_t)bzl[b]);
        put32(&out, (uint32_t)braw[b]);
        uint8_t c[4] = {(uint8_t)bcodec[b], 0, 0, 0};
        buf_put(&out, c, 4);
        buf_put(&pay, bz[b], bzl[b]);
        free(bz[b]);
    }
    uint64_t *flac_off = malloc(sizeof(uint64_t) * (size_t)(nfr + 1));
    for (int i = 0; i < nfr; i++) {
        flac_off[i] = head + pay.n;
        buf_put(&pay, fr[i].fl, fr[i].fl_len);
    }
    put32(&out, (uint32_t)np);
    for (int i = 0; i < np; i++) {
        uint64_t arg = pc[i].type == PIECE_FLAC ? flac_off[pc[i].arg] : pc[i].arg;
        put64(&out, pc[i].out_off);
        put32(&out, pc[i].out_len);
        uint8_t t[4] = {(uint8_t)pc[i].type, 0, (uint8_t)pc[i].ch, 0};
        buf_put(&out, t, 4);
        put64(&out, arg);
        put32(&out, pc[i].flen);
    }
    buf_put(&out, pay.p, pay.n);
    free(pay.p); free(resid.p); free(pc); free(bz); free(bzl); free(braw); free(bcodec); free(flac_off);
    *out_len = out.n;
    return out.p;
}

/* small files: LZMA if it helps, else stored (mkpack's plain_entry) */
static uint8_t *plain_entry(const uint8_t *d, size_t n, int *kind, size_t *out_len) {
    if (n >= 64) {
        size_t bl = 0;
        uint8_t *b = build_pieces(d, n, NULL, 0, &bl);
        if (b && bl < n) { *kind = PACK_PIECES; *out_len = bl; return b; }
        free(b);
    }
    uint8_t *c = malloc(n ? n : 1);
    memcpy(c, d, n);
    *kind = PACK_STORED;
    *out_len = n;
    return c;
}

/* 16-bit PCM WAV: the samples as FLAC when it beats LZMA by 5% (mkpack's wav_entry) */
static uint8_t *wav_entry(const uint8_t *d, size_t n, int *kind, size_t *out_len) {
    size_t s, l;
    int ch, rate;
    if (wav_pcm16_range(d, n, 4096, &s, &l, &ch, &rate)) {
        size_t fl_len = 0, zl = 0;
        uint8_t *fl = flac_encode_s16(d + s, l, ch, rate, &fl_len);
        uint8_t *z = fl ? lzma_alone(d + s, l, &zl) : NULL;
        free(z);
        if (fl && fl_len < zl * 95 / 100) {
            FlacRange r = {s, l, ch, fl, fl_len};
            uint8_t *b = build_pieces(d, n, &r, 1, out_len);
            free(fl);
            *kind = PACK_PIECES;
            return b;
        }
        free(fl);
    }
    return plain_entry(d, n, kind, out_len);
}

/* ---- jobs ---------------------------------------------------------------------------- */
static void run_job(Packer *P, Job *j) {
    if (j->file >= 0) {
        PFile *f = &P->f[j->file];
        int64_t n = 0;
        uint8_t *d = read_all(f->path, &n);
        if (!d) { snprintf(P->err, sizeof P->err, "cannot read %s", f->name); P->failed = 1; return; }
        j->body = f->handler == H_WAV ? wav_entry(d, (size_t)n, &j->kind, &j->len) : plain_entry(d, (size_t)n, &j->kind, &j->len);
        free(d);
        __atomic_add_fetch(&P->done_bytes, n, __ATOMIC_RELAXED);
        return;
    }
    Group *g = &P->g[-1 - j->file];
    Buf blob = {0};
    FlacRange *fr = malloc(sizeof(FlacRange) * (size_t)(g->count + 1));
    int nfr = 0;
    for (int k = 0; k < g->count; k++) {
        PFile *f = &P->f[P->gorder[g->first + k]];
        int64_t n = 0;
        uint8_t *d = read_all(f->path, &n);
        if (!d) { snprintf(P->err, sizeof P->err, "cannot read %s", f->name); P->failed = 1; free(blob.p); free(fr); return; }
        size_t off = blob.n, s, l;
        int ch, rate;
        if (wav_pcm16_range(d, (size_t)n, 8192, &s, &l, &ch, &rate)) {
            size_t fl_len = 0;
            uint8_t *fl = flac_encode_s16(d + s, l, ch, rate, &fl_len);
            if (fl && fl_len < l * 6 / 10) fr[nfr++] = (FlacRange){off + s, l, ch, fl, fl_len};
            else free(fl);
        }
        buf_put(&blob, d, (size_t)n);
        free(d);
    }
    j->body = build_pieces(blob.p, blob.n, fr, nfr, &j->len);
    j->kind = PACK_PIECES;
    for (int i = 0; i < nfr; i++) free(fr[i].fl);
    free(fr);
    __atomic_add_fetch(&P->done_bytes, (int64_t)blob.n, __ATOMIC_RELAXED);
    free(blob.p);
}

static void *worker(void *arg) {
    Packer *P = arg;
    for (;;) {
        pthread_mutex_lock(&P->mu);
        int i = P->failed ? P->njobs : P->next_job++;
        pthread_mutex_unlock(&P->mu);
        if (i >= P->njobs) break;
        run_job(P, &P->jobs[i]);
        pthread_mutex_lock(&P->mu);
        P->jobs[i].done = 1;
        pthread_cond_broadcast(&P->cv);
        pthread_mutex_unlock(&P->mu);
    }
    return NULL;
}

/* ---- scanning, rules ------------------------------------------------------------------ */
typedef struct { char glob[256]; int handler; } Rule;
static Rule g_rules[256];
static int g_nrules;

static void rule_cb(const char *v, void *ctx) {
    (void)ctx;
    char glob[256], h[32];
    if (g_nrules >= 256 || sscanf(v, "%255s %31s", glob, h) != 2) return;
    for (char *c = glob; *c; c++) *c = (char)tolower((unsigned char)*c);
    int hd = !strcasecmp(h, "skip") ? H_SKIP : !strcasecmp(h, "stored") ? H_STORED : !strcasecmp(h, "solid") ? H_SOLID
           : !strcasecmp(h, "wav") ? H_WAV : H_LZMA;   /* lzma, and desktop-only handlers */
    snprintf(g_rules[g_nrules].glob, sizeof g_rules[0].glob, "%s", glob);
    g_rules[g_nrules++].handler = hd;
}

static int handler_for(const char *name) {
    char low[1024];
    snprintf(low, sizeof low, "%s", name);
    for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
    const char *base = strrchr(low, '/');
    base = base ? base + 1 : low;
    for (int i = 0; i < g_nrules; i++)
        if (!fnmatch(g_rules[i].glob, low, 0) || !fnmatch(g_rules[i].glob, base, 0)) return g_rules[i].handler;
    return g_nrules ? H_LZMA : (fnmatch("*.wav", base, 0) ? H_LZMA : H_WAV);
}

static void scan(Packer *P, const char *root, const char *rel) {
    char dir[2048];
    snprintf(dir, sizeof dir, "%s%s%s", root, *rel ? "/" : "", rel);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!*rel && !strncmp(e->d_name, "game.pak", 8)) continue;   /* our own output */
        char r[2048], p[4096];
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        snprintf(p, sizeof p, "%s/%s", root, r);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { scan(P, root, r); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        if (P->nf == P->capf) { P->capf = P->capf ? P->capf * 2 : 1024; P->f = realloc(P->f, sizeof(PFile) * (size_t)P->capf); }
        PFile *f = &P->f[P->nf++];
        memset(f, 0, sizeof *f);
        f->name = strdup(r);
        f->path = strdup(p);
        f->size = st.st_size;
        f->dup_of = f->group = -1;
    }
    closedir(d);
}

static int cmp_name(const void *a, const void *b) { return strcmp(((const PFile *)a)->name, ((const PFile *)b)->name); }

static Packer *g_sort_p;
static void lower_dir(const char *name, char *out, size_t n) {
    snprintf(out, n, "%s", name);
    char *s = strrchr(out, '/');
    if (s) *s = 0; else out[0] = 0;
    for (char *c = out; *c; c++) *c = (char)tolower((unsigned char)*c);
}
static int cmp_group_order(const void *a, const void *b) {
    const PFile *x = &g_sort_p->f[*(const int *)a], *y = &g_sort_p->f[*(const int *)b];
    char dx[1024], dy[1024];
    lower_dir(x->name, dx, sizeof dx);
    lower_dir(y->name, dy, sizeof dy);
    int c = strcmp(dx, dy);
    return c ? c : strcasecmp(x->name, y->name);
}

static void delete_tree(const char *path, const char *keep) {
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[4096];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        if (keep && !strcmp(p, keep)) continue;
        struct stat st;
        if (lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { delete_tree(p, keep); rmdir(p); }
        else unlink(p);
    }
    closedir(d);
}

/* ---- the whole job ------------------------------------------------------------------- */
typedef void (*PackProgress)(void *ctx, int phase, int64_t done, int64_t total);

static int write_all(FILE *o, const void *p, size_t n) { return fwrite(p, 1, n, o) == n ? 0 : -1; }

/* game_dir: the imported folder; writes <game_dir>/game.pak.  threads <= 0: automatic.
 * delete_sources: after verification, remove everything in game_dir except game.pak.
 * Phases: 0 scanning, 1 compressing, 2 verifying, 3 cleaning up.  Returns 0 on success. */
int packer_run(const char *game_dir, int threads, int delete_sources, PackProgress cb, void *ctx, char *err, size_t errn) {
    Packer P;
    memset(&P, 0, sizeof P);
    pthread_mutex_init(&P.mu, NULL);
    pthread_cond_init(&P.cv, NULL);
    g_nrules = 0;
    cfg_each("pack", "rule", rule_cb, NULL);
    char tmp[2048], final[2048];
    snprintf(tmp, sizeof tmp, "%s/game.pak.tmp", game_dir);
    snprintf(final, sizeof final, "%s/game.pak", game_dir);
    unlink(tmp);

    /* scan, handlers, duplicates */
    if (cb) cb(ctx, 0, 0, 0);
    scan(&P, game_dir, "");
    qsort(P.f, (size_t)P.nf, sizeof(PFile), cmp_name);
    int64_t scan_total = 0;
    for (int i = 0; i < P.nf; i++) {
        P.f[i].handler = handler_for(P.f[i].name);
        if (P.f[i].handler != H_SKIP) scan_total += P.f[i].size;
    }
    int64_t scanned = 0;
    for (int i = 0; i < P.nf; i++) {
        PFile *f = &P.f[i];
        if (f->handler == H_SKIP) continue;
        int64_t n = 0;
        uint8_t *d = read_all(f->path, &n);
        if (!d) { snprintf(err, errn, "cannot read %s", f->name); return -1; }
        f->hash = fnv64(d, (size_t)n);
        for (int k = 0; k < i && f->dup_of < 0; k++) {
            PFile *g = &P.f[k];
            if (g->handler == H_SKIP || g->dup_of >= 0 || g->size != f->size || g->hash != f->hash) continue;
            int64_t m = 0;
            uint8_t *e = read_all(g->path, &m);
            if (e && m == n && !memcmp(d, e, (size_t)n)) f->dup_of = k;
            free(e);
        }
        free(d);
        scanned += n;
        if (cb && (i & 255) == 0) cb(ctx, 0, scanned, scan_total);
    }

    /* solid groups: unique files of one folder, in name order, up to SOLID_GROUP bytes */
    P.gorder = malloc(sizeof(int) * (size_t)(P.nf + 1));
    int ns = 0;
    for (int i = 0; i < P.nf; i++)
        if (P.f[i].handler == H_SOLID && P.f[i].dup_of < 0) P.gorder[ns++] = i;
    g_sort_p = &P;
    qsort(P.gorder, (size_t)ns, sizeof(int), cmp_group_order);
    P.g = calloc((size_t)ns + 1, sizeof(Group));
    char cur_dir[1024] = "";
    uint64_t cur_size = 0;
    for (int k = 0; k < ns; k++) {
        PFile *f = &P.f[P.gorder[k]];
        char d[1024];
        lower_dir(f->name, d, sizeof d);
        if (P.ng && P.g[P.ng - 1].count && (strcmp(d, cur_dir) || cur_size + (uint64_t)f->size > SOLID_GROUP)) {
            P.ng++;
            cur_size = 0;
        }
        if (!P.ng) P.ng = 1;
        Group *g = &P.g[P.ng - 1];
        if (!g->count) { memset(g, 0, sizeof *g); g->first = k; }
        f->group = P.ng - 1;
        f->goff = g->blob_len;
        g->count++;
        g->blob_len += (uint64_t)f->size;
        snprintf(cur_dir, sizeof cur_dir, "%s", d);
        cur_size += (uint64_t)f->size;
    }
    if (P.ng && !P.g[P.ng - 1].count) P.ng--;

    /* jobs: solid groups, then lzma/wav files; stored files are copied by this thread */
    P.jobs = calloc((size_t)(P.ng + P.nf + 1), sizeof(Job));
    for (int gi = 0; gi < P.ng; gi++) P.jobs[P.njobs++] = (Job){-1 - gi};
    for (int i = 0; i < P.nf; i++) {
        PFile *f = &P.f[i];
        if (f->handler == H_SKIP || f->dup_of >= 0 || f->group >= 0) continue;
        P.total_bytes += f->size;
        if (f->handler != H_STORED) P.jobs[P.njobs++] = (Job){i};
    }
    for (int gi = 0; gi < P.ng; gi++) P.total_bytes += (int64_t)P.g[gi].blob_len;

    FILE *o = fopen(tmp, "wb");
    if (!o) { snprintf(err, errn, "cannot create %s: %s", tmp, strerror(errno)); return -1; }
    uint8_t zero[24] = {0};
    write_all(o, zero, 24);
    uint64_t pos = 24;
    if (threads <= 0) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        threads = cpus > 1 ? (int)cpus - 1 : 1;
        if (threads > 4) threads = 4;
        /* ~100 MB of encoder memory per thread: leave room on small phones */
        FILE *mi = fopen("/proc/meminfo", "r");
        char line[256];
        long avail_kb = 0;
        while (mi && fgets(line, sizeof line, mi))
            if (sscanf(line, "MemAvailable: %ld kB", &avail_kb) == 1) break;
        if (mi) fclose(mi);
        if (avail_kb > 0) {
            long by_mem = avail_kb / (250 * 1024);
            if (by_mem < threads) threads = by_mem > 1 ? (int)by_mem : 1;
        }
        rt_log("packer: %d threads (%ld MB available)", threads, avail_kb / 1024);
    }
    pthread_t th[16];
    if (threads > 16) threads = 16;
    for (int t = 0; t < threads; t++) pthread_create(&th[t], NULL, worker, &P);
    if (cb) cb(ctx, 1, 0, P.total_bytes);

    /* stored files, streamed */
    int io_err = 0;
    for (int i = 0; i < P.nf && !io_err; i++) {
        PFile *f = &P.f[i];
        if (f->handler != H_STORED || f->dup_of >= 0) continue;
        FILE *in = fopen(f->path, "rb");
        if (!in) { snprintf(P.err, sizeof P.err, "cannot read %s", f->name); io_err = 1; break; }
        f->kind = PACK_STORED;
        f->off = pos;
        uint8_t buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
            if (write_all(o, buf, n)) { io_err = 1; break; }
            pos += n;
        }
        fclose(in);
        f->len = pos - f->off;
        __atomic_add_fetch(&P.done_bytes, f->size, __ATOMIC_RELAXED);
        if (cb) cb(ctx, 1, P.done_bytes, P.total_bytes);
    }
    /* compressed bodies, written as they finish */
    int written = 0;
    while (written < P.njobs && !io_err) {
        pthread_mutex_lock(&P.mu);
        int any = 0;
        for (;;) {
            for (int j = 0; j < P.njobs; j++)
                if (P.jobs[j].done && !P.jobs[j].written) { any = 1; break; }
            if (any || P.failed) break;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 250000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&P.cv, &P.mu, &ts);
            if (cb) { pthread_mutex_unlock(&P.mu); cb(ctx, 1, P.done_bytes, P.total_bytes); pthread_mutex_lock(&P.mu); }
        }
        pthread_mutex_unlock(&P.mu);
        if (P.failed) break;
        for (int j = 0; j < P.njobs; j++) {
            Job *jb = &P.jobs[j];
            pthread_mutex_lock(&P.mu);
            int ready = jb->done && !jb->written;
            pthread_mutex_unlock(&P.mu);
            if (!ready) continue;
            if (!jb->body || write_all(o, jb->body, jb->len)) { io_err = 1; break; }
            if (jb->file >= 0) {
                PFile *f = &P.f[jb->file];
                f->kind = jb->kind; f->off = pos; f->len = jb->len;
            } else {
                Group *g = &P.g[-1 - jb->file];
                g->off = pos; g->len = jb->len;
            }
            pos += jb->len;
            free(jb->body);
            jb->body = NULL;
            jb->written = 1;
            written++;
        }
        if (cb) cb(ctx, 1, P.done_bytes, P.total_bytes);
    }
    pthread_mutex_lock(&P.mu);
    if (io_err) P.failed = 1;
    pthread_mutex_unlock(&P.mu);
    for (int t = 0; t < threads; t++) pthread_join(th[t], NULL);
    if (P.failed || io_err) {
        fclose(o);
        unlink(tmp);
        snprintf(err, errn, "%s", P.err[0] ? P.err : "write failed (is the storage full?)");
        return -1;
    }

    /* index, in mkpack's order: groups, plain files, solid members, aliases */
    Buf idx = {0};
    uint32_t nent = 0;
    for (int gi = 0; gi < P.ng; gi++) {
        char nm[32];
        int nl = snprintf(nm, sizeof nm, "\x01solid/%05d", gi);
        uint8_t h[2] = {(uint8_t)nl, 0};
        buf_put(&idx, h, 2); buf_put(&idx, nm, (size_t)nl);
        uint8_t k = PACK_PIECES;
        buf_put(&idx, &k, 1); put64(&idx, P.g[gi].blob_len); put64(&idx, P.g[gi].off); put64(&idx, P.g[gi].len);
        nent++;
    }
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < P.nf; i++) {
            PFile *f = &P.f[i];
            if (f->handler == H_SKIP) continue;
            int is_alias = f->dup_of >= 0, is_solid = !is_alias && f->group >= 0;
            if ((pass == 0 && (is_alias || is_solid)) || (pass == 1 && !is_solid) || (pass == 2 && !is_alias)) continue;
            int kind; uint64_t off, len;
            PFile *src = is_alias ? &P.f[f->dup_of] : f;
            if (src->group >= 0) { kind = PACK_SOLID; off = src->goff; len = (uint64_t)src->group; }
            else { kind = src->kind; off = src->off; len = src->len; }
            size_t nl = strlen(f->name);
            uint8_t h[2] = {(uint8_t)nl, (uint8_t)(nl >> 8)};
            buf_put(&idx, h, 2); buf_put(&idx, f->name, nl);
            uint8_t k = (uint8_t)kind;
            buf_put(&idx, &k, 1); put64(&idx, (uint64_t)src->size); put64(&idx, off); put64(&idx, len);
            nent++;
        }
    uint64_t ioff = pos;
    int werr = write_all(o, idx.p, idx.n);
    Buf hd = {0};
    buf_put(&hd, PACK_MAGIC, 4); put32(&hd, PACK_VERSION); put32(&hd, nent); put32(&hd, (uint32_t)idx.n); put64(&hd, ioff);
    fseek(o, 0, SEEK_SET);
    werr |= write_all(o, hd.p, hd.n);
    werr |= fflush(o);
    fsync(fileno(o));
    werr |= fclose(o);
    free(idx.p); free(hd.p);
    if (werr) { unlink(tmp); snprintf(err, errn, "write failed (is the storage full?)"); return -1; }
    rt_log("packer: %u entries, %llu bytes", nent, (unsigned long long)(ioff + idx.n));

    /* verify: every packed file, read back through the VFS (pack only), equals the original */
    platform_set_dirs("/nonexistent-game-dir", "/nonexistent-save-dir");
    if (vfs_pack_open(tmp, 0)) { unlink(tmp); snprintf(err, errn, "the new pack does not open"); return -1; }
    int64_t vdone = 0;
    int nverified = 0;
    for (int i = 0; i < P.nf; i++) {
        PFile *f = &P.f[i];
        if (f->handler == H_SKIP) continue;
        int64_t n = 0;
        uint8_t *a = read_all(f->path, &n);
        VFile *v = vfs_open(f->name);
        int ok = a && v && vfs_size(v) == n;
        if (ok) {
            uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
            int64_t p = 0;
            while (p < n) {
                size_t got = vfs_read(v, b + p, (size_t)(n - p));
                if (!got) break;
                p += (int64_t)got;
            }
            ok = p == n && !memcmp(a, b, (size_t)n);
            free(b);
        }
        if (v) vfs_close(v);
        free(a);
        if (!ok) {
            unlink(tmp);
            snprintf(err, errn, "verification failed for %s; nothing was changed", f->name);
            return -1;
        }
        vdone += n;
        nverified++;
        if (cb && (i & 63) == 0) cb(ctx, 2, vdone, scan_total);
    }
    if (rename(tmp, final)) { snprintf(err, errn, "rename failed: %s", strerror(errno)); return -1; }
    if (delete_sources) {
        if (cb) cb(ctx, 3, 0, 0);
        delete_tree(game_dir, final);
    }
    rt_log("packer: done, %d files verified identical", nverified);
    return 0;
}

#ifdef __ANDROID__
/* ---- JNI: PackActivity.nativePack(gameDir, saveDir) -> null or an error message ---- */
#include <jni.h>
typedef struct { JNIEnv *env; jobject obj; jmethodID m; } JCtx;
static void jprogress(void *c, int phase, int64_t done, int64_t total) {
    JCtx *j = c;
    (*j->env)->CallVoidMethod(j->env, j->obj, j->m, (jint)phase, (jlong)done, (jlong)total);
}
JNIEXPORT jstring JNICALL Java_org_gameport_app_PackActivity_nativePack(JNIEnv *env, jobject obj, jstring jgame, jstring jsave) {
    const char *game = (*env)->GetStringUTFChars(env, jgame, NULL);
    const char *save = (*env)->GetStringUTFChars(env, jsave, NULL);
    platform_set_dirs(game, save);
    cfg_init();   /* [pack] rules: the compiled-in game.ini, plus <save>/game.ini */
    jclass cls = (*env)->GetObjectClass(env, obj);
    JCtx j = {env, obj, (*env)->GetMethodID(env, cls, "onProgress", "(IJJ)V")};
    char err[512] = "";
    char gdir[2048];
    snprintf(gdir, sizeof gdir, "%s", game);
    int r = packer_run(gdir, 0, 1, jprogress, &j, err, sizeof err);
    (*env)->ReleaseStringUTFChars(env, jgame, game);
    (*env)->ReleaseStringUTFChars(env, jsave, save);
    return r ? (*env)->NewStringUTF(env, err) : NULL;
}
#endif

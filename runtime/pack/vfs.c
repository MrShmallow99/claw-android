/* Game file access: save overlay, loose game folder, then game.pak (see vfs.h). */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
#include "vfs.h"
#include "pack_format.h"
#include "edlrec.h"
#include "../platform.h"
#include "../../third_party/lzma/LzmaDec.h"
#include "drflac.h"

void rt_log(const char *fmt, ...);

/* ---- pack index ------------------------------------------------------------------ */
typedef struct {
    char *name;
    int kind;
    uint64_t size, off, len;
    /* PACK_PIECES decode cache, shared by all handles of the entry */
    struct PiecesCache *pc;
} PackEntry;

static int g_pack_fd = -1;
static uint64_t g_pack_base;
static PackEntry *g_entries;
static uint32_t g_nentries;
/* case-insensitive hash index over g_entries (open addressing, -1 = empty) */
static int32_t *g_hash;
static uint32_t g_hash_mask;

static uint32_t ci_hash(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        h = (h ^ c) * 16777619u;
    }
    return h;
}

/* directory prefixes of all entries, hashed (case-insensitive) */
static char **g_dirs;
static uint32_t g_dir_mask;

static void dir_add(const char *name, size_t n) {
    uint32_t h = ci_hash(name, n) & g_dir_mask;
    while (g_dirs[h]) {
        if (strlen(g_dirs[h]) == n && !strncasecmp(g_dirs[h], name, n)) return;
        h = (h + 1) & g_dir_mask;
    }
    g_dirs[h] = strndup(name, n);
}

static void pack_index_build(void) {
    uint32_t cap = 16;
    while (cap < g_nentries * 2) cap <<= 1;
    g_hash = malloc(cap * sizeof *g_hash);
    memset(g_hash, 0xff, cap * sizeof *g_hash);
    g_hash_mask = cap - 1;
    for (uint32_t i = 0; i < g_nentries; i++) {
        uint32_t h = ci_hash(g_entries[i].name, (size_t)-1) & g_hash_mask;
        while (g_hash[h] >= 0) h = (h + 1) & g_hash_mask;
        g_hash[h] = (int32_t)i;
    }
    g_dirs = calloc(cap * 2, sizeof(char *));
    g_dir_mask = cap * 2 - 1;
    for (uint32_t i = 0; i < g_nentries; i++) {
        const char *nm = g_entries[i].name;
        for (const char *sl = strchr(nm, '/'); sl; sl = strchr(sl + 1, '/')) dir_add(nm, (size_t)(sl - nm));
    }
}

static int pack_pread(void *buf, size_t n, uint64_t off) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = pread(g_pack_fd, p, n, (off_t)(g_pack_base + off));
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
        off += (uint64_t)r;
    }
    return 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

int vfs_have_pack(void) { return g_pack_fd >= 0; }

static void prefetch_start(void);

int vfs_pack_open(const char *path, uint64_t base) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    g_pack_fd = fd;
    g_pack_base = base;
    uint8_t h[24];
    if (pack_pread(h, sizeof h, 0) || memcmp(h, PACK_MAGIC, 4) || le32(h + 4) != PACK_VERSION) goto fail;
    g_nentries = le32(h + 8);
    uint32_t ilen = le32(h + 12);
    uint64_t ioff = le64(h + 16);
    uint8_t *idx = malloc(ilen);
    if (!idx || pack_pread(idx, ilen, ioff)) { free(idx); goto fail; }
    g_entries = calloc(g_nentries, sizeof(PackEntry));
    const uint8_t *p = idx, *end = idx + ilen;
    for (uint32_t i = 0; i < g_nentries; i++) {
        if (p + 2 > end) { free(idx); goto fail; }
        unsigned nl = p[0] | p[1] << 8;
        p += 2;
        if (p + nl + 25 > end) { free(idx); goto fail; }
        PackEntry *e = &g_entries[i];
        e->name = malloc(nl + 1);
        memcpy(e->name, p, nl);
        e->name[nl] = 0;
        p += nl;
        e->kind = *p++;
        e->size = le64(p);
        e->off = le64(p + 8);
        e->len = le64(p + 16);
        p += 24;
    }
    free(idx);
    pack_index_build();
    rt_log("pack: %s (+%llu), %u entries", path, (unsigned long long)base, g_nentries);
    prefetch_start();
    return 0;
fail:
    close(fd);
    g_pack_fd = -1;
    return -1;
}

/* Finds an uncompressed (stored) member of a zip file such as the APK. */
int vfs_zip_find_stored(const char *zip, const char *name, uint64_t *off, uint64_t *size) {
    int fd = open(zip, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int ret = -1;
    struct stat st;
    if (fstat(fd, &st)) goto out;
    size_t tail = st.st_size < 65536 + 22 ? (size_t)st.st_size : 65536 + 22;
    uint8_t *t = malloc(tail);
    if (pread(fd, t, tail, st.st_size - (off_t)tail) != (ssize_t)tail) { free(t); goto out; }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (le32(t + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(t); goto out; }
    uint32_t cd_size = le32(t + eocd + 12), cd_off = le32(t + eocd + 16);
    free(t);
    uint8_t *cd = malloc(cd_size);
    if (pread(fd, cd, cd_size, cd_off) != (ssize_t)cd_size) { free(cd); goto out; }
    size_t nl = strlen(name);
    for (uint32_t p = 0; p + 46 <= cd_size;) {
        if (le32(cd + p) != 0x02014b50) break;
        unsigned method = cd[p + 10] | cd[p + 11] << 8;
        uint32_t csz = le32(cd + p + 20);
        unsigned fnl = cd[p + 28] | cd[p + 29] << 8, exl = cd[p + 30] | cd[p + 31] << 8, cml = cd[p + 32] | cd[p + 33] << 8;
        uint32_t lho = le32(cd + p + 42);
        if (fnl == nl && !memcmp(cd + p + 46, name, nl) && method == 0) {
            uint8_t lh[30];
            if (pread(fd, lh, 30, lho) == 30 && le32(lh) == 0x04034b50) {
                *off = (uint64_t)lho + 30 + (lh[26] | lh[27] << 8) + (lh[28] | lh[29] << 8);
                *size = csz;
                ret = 0;
            }
            break;
        }
        p += 46 + fnl + exl + cml;
    }
    free(cd);
out:
    close(fd);
    return ret;
}

static PackEntry *pack_find(const char *rel) {
    if (!g_hash) return NULL;
    for (uint32_t h = ci_hash(rel, (size_t)-1) & g_hash_mask; g_hash[h] >= 0; h = (h + 1) & g_hash_mask)
        if (!strcasecmp(g_entries[g_hash[h]].name, rel)) return &g_entries[g_hash[h]];
    return NULL;
}

/* is rel a directory prefix of some pack entry? */
static int pack_is_dir(const char *rel) {
    size_t n = strlen(rel);
    if (!n) return 1;
    if (!g_dirs) return 0;
    for (uint32_t h = ci_hash(rel, n) & g_dir_mask; g_dirs[h]; h = (h + 1) & g_dir_mask)
        if (strlen(g_dirs[h]) == n && !strncasecmp(g_dirs[h], rel, n)) return 1;
    return 0;
}

/* ---- LZMA ------------------------------------------------------------------------- */
static void *lz_alloc(ISzAllocPtr p, size_t size) { (void)p; return malloc(size); }
static void lz_free(ISzAllocPtr p, void *a) { (void)p; free(a); }
static const ISzAlloc g_lz_alloc = {lz_alloc, lz_free};

/* One-shot decode of a whole .lzma blob read from the pack. */
static int lzma_decode_all(uint64_t off, uint64_t len, uint8_t *dst, size_t dstlen) {
    if (len < 13) return -1;
    uint8_t *src = malloc((size_t)len);
    if (!src || pack_pread(src, (size_t)len, off)) { free(src); return -1; }
    SizeT dl = dstlen, sl = (SizeT)(len - 13);
    ELzmaStatus st;
    SRes r = LzmaDecode(dst, &dl, src + 13, &sl, src, LZMA_PROPS_SIZE, LZMA_FINISH_ANY, &st, &g_lz_alloc);
    free(src);
    return r == SZ_OK && dl == dstlen ? 0 : -1;
}

/* Streaming decode of a .lzma blob in the pack. */
typedef struct {
    uint64_t off, len, pos;     /* source range in the pack, read position */
    uint64_t raw_left;
    CLzmaDec dec;
    int inited;
    uint8_t in[1 << 16];
    size_t in_pos, in_len;
} LzStream;

static int lzs_open(LzStream *s, uint64_t off, uint64_t len, uint64_t raw_len) {
    memset(s, 0, sizeof *s);
    uint8_t h[13];
    if (len < 13 || pack_pread(h, 13, off)) return -1;
    LzmaDec_Construct(&s->dec);
    if (LzmaDec_Allocate(&s->dec, h, LZMA_PROPS_SIZE, &g_lz_alloc) != SZ_OK) return -1;
    LzmaDec_Init(&s->dec);
    s->inited = 1;
    s->off = off;
    s->len = len;
    s->pos = 13;
    s->raw_left = raw_len;
    return 0;
}

static void lzs_close(LzStream *s) {
    if (s->inited) LzmaDec_Free(&s->dec, &g_lz_alloc);
    s->inited = 0;
}

/* reads exactly n bytes; returns 0 on success */
static int lzs_read(LzStream *s, uint8_t *dst, size_t n) {
    if (n > s->raw_left) return -1;
    while (n) {
        if (s->in_pos == s->in_len) {
            uint64_t left = s->len - s->pos;
            size_t want = left < sizeof s->in ? (size_t)left : sizeof s->in;
            if (want && pack_pread(s->in, want, s->off + s->pos)) return -1;
            s->pos += want;
            s->in_pos = 0;
            s->in_len = want;
        }
        SizeT ol = n, il = s->in_len - s->in_pos;
        ELzmaStatus st;
        SRes r = LzmaDec_DecodeToBuf(&s->dec, dst, &ol, s->in + s->in_pos, &il, LZMA_FINISH_ANY, &st);
        if (r != SZ_OK) return -1;
        s->in_pos += il;
        dst += ol;
        n -= ol;
        s->raw_left -= ol;
        if (ol == 0 && il == 0) return -1;
    }
    return 0;
}

/* ---- PACK_PIECES ------------------------------------------------------------------- */
typedef struct {
    uint64_t src_off;
    uint32_t src_len, raw_len;
    int codec;
    uint64_t resid_off;
    uint8_t *data;              /* decoded, or NULL */
    uint64_t last_use;
} PBlock;

typedef struct {
    uint64_t out_off;
    uint32_t out_len;
    int type, xor, channels;
    uint64_t arg;
    uint32_t flac_len;
    uint8_t *data;              /* decoded FLAC piece, or NULL */
    uint64_t last_use;
} PPiece;

typedef struct PiecesCache {
    pthread_mutex_t mu;
    uint32_t nblocks, npieces;
    PBlock *blocks;
    PPiece *pieces;
} PiecesCache;

static pthread_mutex_t g_pc_init_mu = PTHREAD_MUTEX_INITIALIZER;

static PiecesCache *pieces_load(PackEntry *e) {
    pthread_mutex_lock(&g_pc_init_mu);
    if (e->pc) {
        pthread_mutex_unlock(&g_pc_init_mu);
        return e->pc;
    }
    PiecesCache *pc = calloc(1, sizeof *pc);
    pthread_mutex_init(&pc->mu, NULL);
    uint8_t b4[4];
    uint64_t p = e->off;
    if (pack_pread(b4, 4, p)) goto fail;
    pc->nblocks = le32(b4);
    p += 4;
    uint8_t *tab = malloc((size_t)pc->nblocks * PACK_BLOCK_SIZE + 4);
    if (pack_pread(tab, (size_t)pc->nblocks * PACK_BLOCK_SIZE + 4, p)) { free(tab); goto fail; }
    pc->blocks = calloc(pc->nblocks + 1, sizeof(PBlock));
    uint64_t ro = 0;
    for (uint32_t i = 0; i < pc->nblocks; i++) {
        const uint8_t *b = tab + (size_t)i * PACK_BLOCK_SIZE;
        PBlock *k = &pc->blocks[i];
        k->src_off = e->off + le64(b);
        k->src_len = le32(b + 8);
        k->raw_len = le32(b + 12);
        k->codec = b[16];
        k->resid_off = ro;
        ro += k->raw_len;
    }
    pc->npieces = le32(tab + (size_t)pc->nblocks * PACK_BLOCK_SIZE);
    p += (uint64_t)pc->nblocks * PACK_BLOCK_SIZE + 4;
    free(tab);
    tab = malloc((size_t)pc->npieces * PACK_PIECE_SIZE + 1);
    if (pack_pread(tab, (size_t)pc->npieces * PACK_PIECE_SIZE, p)) { free(tab); goto fail; }
    pc->pieces = calloc(pc->npieces + 1, sizeof(PPiece));
    for (uint32_t i = 0; i < pc->npieces; i++) {
        const uint8_t *b = tab + (size_t)i * PACK_PIECE_SIZE;
        PPiece *q = &pc->pieces[i];
        q->out_off = le64(b);
        q->out_len = le32(b + 8);
        q->type = b[12];
        q->xor = b[13];
        q->channels = b[14] ? b[14] : 1;
        q->arg = le64(b + 16);
        q->flac_len = le32(b + 24);
        if (q->type == PIECE_FLAC) q->arg += e->off;
    }
    free(tab);
    e->pc = pc;
    pthread_mutex_unlock(&g_pc_init_mu);
    return pc;
fail:
    free(pc->blocks);
    free(pc);
    pthread_mutex_unlock(&g_pc_init_mu);
    rt_log("pack: bad entry %s", e->name);
    return NULL;
}

/* ---- decoded-data budget: least recently used blocks/pieces are dropped -------------- */
#ifdef __ANDROID__
#define CACHE_BUDGET (96u << 20)
#else
#define CACHE_BUDGET (256u << 20)
#endif
typedef struct { struct PiecesCache *pc; uint32_t idx; int is_piece; } CacheRef;
static CacheRef *g_cached;
static int g_ncached, g_capcached;
static uint64_t g_cache_bytes, g_use_clock;
static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static uint8_t **ref_slot(CacheRef *r, uint32_t *len, uint64_t **last);

/* called with pc->mu held for the cache that is adding; victims are trylocked */
static void cache_add(struct PiecesCache *pc, uint32_t idx, int is_piece, uint32_t len) {
    pthread_mutex_lock(&g_cache_mu);
    if (g_ncached == g_capcached) {
        g_capcached = g_capcached ? g_capcached * 2 : 256;
        g_cached = realloc(g_cached, sizeof(CacheRef) * (size_t)g_capcached);
    }
    g_cached[g_ncached++] = (CacheRef){pc, idx, is_piece};
    g_cache_bytes += len;
    for (int tries = 0; g_cache_bytes > CACHE_BUDGET && g_ncached > 1 && tries < 64; tries++) {
        int victim = -1;
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < g_ncached - 1; i++) { /* never the one just added */
            uint32_t l;
            uint64_t *last;
            ref_slot(&g_cached[i], &l, &last);
            if (*last < oldest) { oldest = *last; victim = i; }
        }
        if (victim < 0) break;
        CacheRef r = g_cached[victim];
        int self = r.pc == pc;
        if (!self && pthread_mutex_trylock(&r.pc->mu) != 0) {
            /* busy: just move it to the back of the age order */
            uint32_t l;
            uint64_t *last;
            ref_slot(&g_cached[victim], &l, &last);
            *last = ++g_use_clock;
            continue;
        }
        uint32_t l;
        uint64_t *last;
        uint8_t **slot = ref_slot(&r, &l, &last);
        free(*slot);
        *slot = NULL;
        g_cache_bytes -= l;
        g_cached[victim] = g_cached[--g_ncached];
        if (!self) pthread_mutex_unlock(&r.pc->mu);
    }
    pthread_mutex_unlock(&g_cache_mu);
}

/* decoded data of block i (cached); call with pc->mu held */
static const uint8_t *block_data(PiecesCache *pc, uint32_t i) {
    PBlock *k = &pc->blocks[i];
    k->last_use = __atomic_add_fetch(&g_use_clock, 1, __ATOMIC_RELAXED);
    if (k->data) return k->data;
    uint8_t *d = malloc(k->raw_len ? k->raw_len : 1);
    int r = k->codec == BLOCK_RAW ? pack_pread(d, k->raw_len, k->src_off)
                                  : lzma_decode_all(k->src_off, k->src_len, d, k->raw_len);
    if (r) {
        free(d);
        return NULL;
    }
    k->data = d;
    cache_add(pc, i, 0, k->raw_len);
    return d;
}

static const uint8_t *flac_piece_data(PiecesCache *pc, PPiece *q) {
    q->last_use = __atomic_add_fetch(&g_use_clock, 1, __ATOMIC_RELAXED);
    if (q->data) return q->data;
    uint8_t *src = malloc(q->flac_len);
    if (!src || pack_pread(src, q->flac_len, q->arg)) { free(src); return NULL; }
    drflac *fl = drflac_open_memory(src, q->flac_len, NULL);
    uint8_t *d = NULL;
    if (fl) {
        d = malloc(q->out_len ? q->out_len : 1);
        drflac_uint64 frames = q->out_len / 2 / (unsigned)q->channels;
        drflac_uint64 got = drflac_read_pcm_frames_s16(fl, frames, (drflac_int16 *)d);
        drflac_close(fl);
        if (got != frames) {
            free(d);
            d = NULL;
        } else {
            /* stored little-endian like the original file */
            for (uint32_t i = 0; i + 1 < q->out_len; i += 2) {
                int16_t v;
                memcpy(&v, d + i, 2);
                uint16_t u = (uint16_t)v;
                d[i] = (uint8_t)u;
                d[i + 1] = (uint8_t)(u >> 8);
            }
            if (q->xor)
                for (uint32_t i = 0; i < q->out_len; i++) d[i] ^= (uint8_t)(i + q->xor - 1);
        }
    }
    free(src);
    q->data = d;
    if (d) cache_add(pc, (uint32_t)(q - pc->pieces), 1, q->out_len);
    return d;
}

static uint8_t **ref_slot(CacheRef *r, uint32_t *len, uint64_t **last) {
    if (r->is_piece) {
        PPiece *q = &r->pc->pieces[r->idx];
        *len = q->out_len;
        *last = &q->last_use;
        return &q->data;
    }
    PBlock *k = &r->pc->blocks[r->idx];
    *len = k->raw_len;
    *last = &k->last_use;
    return &k->data;
}

/* copies [off, off+n) of the entry into dst; returns bytes copied */
static size_t pieces_read(PiecesCache *pc, uint64_t off, uint8_t *dst, size_t n) {
    size_t done = 0;
    pthread_mutex_lock(&pc->mu);
    /* binary search for the first piece containing off */
    uint32_t lo = 0, hi = pc->npieces;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (pc->pieces[mid].out_off + pc->pieces[mid].out_len <= off) lo = mid + 1;
        else hi = mid;
    }
    for (uint32_t i = lo; i < pc->npieces && done < n; i++) {
        PPiece *q = &pc->pieces[i];
        if (q->out_off > off + done) break;
        uint64_t inner = off + done - q->out_off;
        size_t take = (size_t)(q->out_len - inner);
        if (take > n - done) take = n - done;
        if (q->type == PIECE_FLAC) {
            const uint8_t *d = flac_piece_data(pc, q);
            if (!d) break;
            memcpy(dst + done, d + inner, take);
            done += take;
        } else {
            uint64_t ro = q->arg + inner;
            size_t left = take;
            while (left) {
                uint32_t blo = 0, bhi = pc->nblocks;
                while (blo < bhi) {
                    uint32_t mid = (blo + bhi) / 2;
                    if (pc->blocks[mid].resid_off + pc->blocks[mid].raw_len <= ro) blo = mid + 1;
                    else bhi = mid;
                }
                if (blo >= pc->nblocks) goto out;
                const uint8_t *d = block_data(pc, blo);
                if (!d) goto out;
                PBlock *k = &pc->blocks[blo];
                size_t in = (size_t)(ro - k->resid_off), t = k->raw_len - in;
                if (t > left) t = left;
                memcpy(dst + done, d + in, t);
                done += t;
                left -= t;
                ro += t;
            }
        }
    }
out:
    pthread_mutex_unlock(&pc->mu);
    return done;
}

/* Background decoding must never take CPU time from the game or the audio
 * thread (on a phone that is heard as crackling). */
static void background_priority(void) {
#ifdef __linux__
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
}

/* Decode the big random-access entries in the background, so the game
 * never waits for decompression when it loads a level. */
static void *prefetch_thread(void *arg) {
    (void)arg;
    background_priority();
    for (uint32_t i = 0; i < g_nentries; i++) {
        PackEntry *e = &g_entries[i];
        if (e->kind != PACK_PIECES || e->size < (4u << 20) || e->name[0] == '\x01') continue; /* not solid groups */
        PiecesCache *pc = pieces_load(e);
        if (!pc) continue;
        for (uint32_t b = 0; b < pc->nblocks; b++) {
            pthread_mutex_lock(&pc->mu);
            block_data(pc, b);
            pthread_mutex_unlock(&pc->mu);
        }
        for (uint32_t q = 0; q < pc->npieces; q++) {
            pthread_mutex_lock(&pc->mu);
            if (pc->pieces[q].type == PIECE_FLAC) flac_piece_data(pc, &pc->pieces[q]);
            pthread_mutex_unlock(&pc->mu);
        }
        rt_log("pack: %s ready", e->name);
    }
    return NULL;
}

static void prefetch_start(void) {
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &a, prefetch_thread, NULL);
    pthread_attr_destroy(&a);
}

/* ---- PACK_ECM ------------------------------------------------------------------------ */
typedef struct {
    uint8_t *data;
    size_t len;
} Seg;

typedef struct {
    PackEntry *e;
    uint32_t nchunks;
    uint64_t soff[3], slen[3], sraw[3];
    /* producer state (owned by the producer thread) */
    LzStream rest, dec, recon;
    uint8_t *recon_buf;         /* the whole recon stream (small) */
    size_t recon_len;
    ByteRd rr;
    uint32_t chunk;             /* next chunk to produce */
    int stage;                  /* 0 header, 1 chunks, 2 tail, 3 done */
    /* queue */
    pthread_t th;
    int th_running;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Seg q[3];
    int qhead, qn;
    int stop, eof, error;
    /* consumer */
    Seg cur;
    uint64_t cur_start;         /* logical offset of cur.data[0] */
} Ecm;

static int ecm_produce(Ecm *m, Seg *out) {
    uint64_t v;
    if (m->stage == 0) {
        if (br_var(&m->rr, &v)) return -1;
        out->len = (size_t)v;
        out->data = malloc(out->len ? out->len : 1);
        if (lzs_read(&m->rest, out->data, out->len)) return -1;
        m->stage = 1;
        return 0;
    }
    if (m->stage == 1 && m->chunk >= m->nchunks) m->stage = 2;
    if (m->stage == 2) {
        if (br_var(&m->rr, &v)) return -1;
        out->len = (size_t)v;
        out->data = malloc(out->len ? out->len : 1);
        if (lzs_read(&m->rest, out->data, out->len)) return -1;
        m->stage = 3;
        return 0;
    }
    if (m->stage == 3) return 1;
    uint64_t prefix;
    unsigned mode;
    if (br_var(&m->rr, &prefix) || br_u8(&m->rr, &mode)) return -1;
    if (mode == 0) {
        uint64_t raw;
        if (br_var(&m->rr, &raw)) return -1;
        out->len = (size_t)(prefix + raw);
        out->data = malloc(out->len ? out->len : 1);
        if (lzs_read(&m->rest, out->data, out->len)) return -1;
    } else {
        uint64_t dl;
        if (br_var(&m->rr, &dl)) return -1;
        long bl = edlrec_blob_size(&m->rr);
        if (bl < 0) return -1;
        out->len = (size_t)prefix + (size_t)bl;
        out->data = malloc(out->len);
        uint8_t *dec = malloc(dl ? (size_t)dl : 1);
        int ok = !lzs_read(&m->rest, out->data, (size_t)prefix) && !lzs_read(&m->dec, dec, (size_t)dl) &&
                 edlrec_rebuild(dec, (size_t)dl, &m->rr, out->data + prefix, (size_t)bl) == bl;
        free(dec);
        if (!ok) return -1;
    }
    m->chunk++;
    return 0;
}

static void *ecm_thread(void *arg) {
    Ecm *m = arg;
    background_priority();
    for (;;) {
        pthread_mutex_lock(&m->mu);
        while (m->qn == 2 && !m->stop) pthread_cond_wait(&m->cv, &m->mu);
        int stop = m->stop;
        pthread_mutex_unlock(&m->mu);
        if (stop) break;
        Seg s = {0};
        int r = ecm_produce(m, &s);
        pthread_mutex_lock(&m->mu);
        if (r) {
            free(s.data);
            if (r < 0) {
                m->error = 1;
                rt_log("pack: movie %s: decode error at chunk %u", m->e->name, m->chunk);
            }
            m->eof = 1;
            pthread_cond_broadcast(&m->cv);
            pthread_mutex_unlock(&m->mu);
            break;
        }
        m->q[(m->qhead + m->qn) % 3] = s;
        m->qn++;
        pthread_cond_broadcast(&m->cv);
        pthread_mutex_unlock(&m->mu);
    }
    return NULL;
}

static void ecm_stop(Ecm *m) {
    if (m->th_running) {
        pthread_mutex_lock(&m->mu);
        m->stop = 1;
        pthread_cond_broadcast(&m->cv);
        pthread_mutex_unlock(&m->mu);
        pthread_join(m->th, NULL);
        m->th_running = 0;
    }
    while (m->qn) {
        free(m->q[m->qhead].data);
        m->qhead = (m->qhead + 1) % 3;
        m->qn--;
    }
    free(m->cur.data);
    m->cur.data = NULL;
    m->cur.len = 0;
    lzs_close(&m->rest);
    lzs_close(&m->dec);
    free(m->recon_buf);
    m->recon_buf = NULL;
}

static int ecm_start(Ecm *m) {
    m->stop = m->eof = m->error = 0;
    m->qhead = m->qn = 0;
    m->chunk = 0;
    m->stage = 0;
    m->cur_start = 0;
    m->cur.data = NULL;
    m->cur.len = 0;
    LzStream rs;
    if (lzs_open(&rs, m->soff[2], m->slen[2], m->sraw[2])) return -1;
    m->recon_len = (size_t)m->sraw[2];
    m->recon_buf = malloc(m->recon_len ? m->recon_len : 1);
    int r = lzs_read(&rs, m->recon_buf, m->recon_len);
    lzs_close(&rs);
    if (r) return -1;
    m->rr.p = m->recon_buf;
    m->rr.end = m->recon_buf + m->recon_len;
    if (lzs_open(&m->rest, m->soff[0], m->slen[0], m->sraw[0])) return -1;
    if (lzs_open(&m->dec, m->soff[1], m->slen[1], m->sraw[1])) return -1;
    if (pthread_create(&m->th, NULL, ecm_thread, m)) return -1;
    m->th_running = 1;
    return 0;
}

static Ecm *ecm_open(PackEntry *e) {
    uint8_t h[4 + 3 * PACK_STREAM_SIZE];
    if (pack_pread(h, sizeof h, e->off)) return NULL;
    Ecm *m = calloc(1, sizeof *m);
    m->e = e;
    m->nchunks = le32(h);
    for (int i = 0; i < 3; i++) {
        const uint8_t *s = h + 4 + i * PACK_STREAM_SIZE;
        m->soff[i] = e->off + le64(s);
        m->slen[i] = le64(s + 8);
        m->sraw[i] = le64(s + 16);
    }
    pthread_mutex_init(&m->mu, NULL);
    pthread_cond_init(&m->cv, NULL);
    if (ecm_start(m)) {
        ecm_stop(m);
        free(m);
        return NULL;
    }
    return m;
}

static void ecm_close(Ecm *m) {
    ecm_stop(m);
    pthread_mutex_destroy(&m->mu);
    pthread_cond_destroy(&m->cv);
    free(m);
}

/* makes cur cover pos if possible; returns 0 if pos is inside cur */
static int ecm_seek_to(Ecm *m, uint64_t pos) {
    if (pos < m->cur_start) {
        ecm_stop(m);
        if (ecm_start(m)) return -1;
    }
    while (pos >= m->cur_start + m->cur.len) {
        Seg s;
        pthread_mutex_lock(&m->mu);
        while (m->qn == 0 && !m->eof) pthread_cond_wait(&m->cv, &m->mu);
        if (m->qn == 0) {
            pthread_mutex_unlock(&m->mu);
            return -1;
        }
        s = m->q[m->qhead];
        m->qhead = (m->qhead + 1) % 3;
        m->qn--;
        pthread_cond_broadcast(&m->cv);
        pthread_mutex_unlock(&m->mu);
        m->cur_start += m->cur.len;
        free(m->cur.data);
        m->cur = s;
    }
    return 0;
}

/* ---- loose files ----------------------------------------------------------------- */
/* Case-insensitive lookup of rel below root; writes the real path into out. */
int vfs_ci_lookup(const char *root, const char *rel, char *out, size_t n) {
    char cur[1024];
    snprintf(cur, sizeof cur, "%s", root);
    const char *p = rel;
    while (*p) {
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - s);
        if (*p == '/') p++;
        char comp[256];
        if (len >= sizeof comp) return -1;
        memcpy(comp, s, len);
        comp[len] = 0;
        char cand[1200];
        snprintf(cand, sizeof cand, "%s/%s", cur, comp);
        struct stat st;
        if (stat(cand, &st) == 0) {
            snprintf(cur, sizeof cur, "%s", cand);
            continue;
        }
        int found = 0;
        DIR *d = opendir(cur);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcasecmp(e->d_name, comp)) {
                    snprintf(cand, sizeof cand, "%s/%s", cur, e->d_name);
                    snprintf(cur, sizeof cur, "%s", cand);
                    found = 1;
                    break;
                }
            }
            closedir(d);
        }
        if (!found) return -1;
    }
    snprintf(out, n, "%s", cur);
    return 0;
}

int vfs_loose_path(const char *rel, char *out, size_t outsz) {
    if (vfs_ci_lookup(platform_save_dir(), rel, out, outsz) == 0) return 0;
    if (vfs_ci_lookup(platform_game_dir(), rel, out, outsz) == 0) return 0;
    return -1;
}

/* ---- directory listing: overlay, loose folder and pack merged, names unique (ci) ---- */
typedef struct { char **names; int *dirs; int n, cap; } NameSet;

static int nameset_has(NameSet *ns, const char *name) {
    for (int i = 0; i < ns->n; i++)
        if (!strcasecmp(ns->names[i], name)) return 1;
    return 0;
}

static void nameset_add(NameSet *ns, const char *name, size_t len, int dir) {
    char tmp[256];
    if (len >= sizeof tmp) return;
    memcpy(tmp, name, len);
    tmp[len] = 0;
    if (nameset_has(ns, tmp)) return;
    if (ns->n == ns->cap) {
        ns->cap = ns->cap ? ns->cap * 2 : 64;
        ns->names = realloc(ns->names, ns->cap * sizeof *ns->names);
        ns->dirs = realloc(ns->dirs, ns->cap * sizeof *ns->dirs);
    }
    ns->names[ns->n] = strdup(tmp);
    ns->dirs[ns->n++] = dir;
}

static void list_loose(NameSet *ns, const char *root, const char *rel) {
    char path[1200];
    if (vfs_ci_lookup(root, rel, path, sizeof path)) return;
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[1500];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (stat(full, &st)) continue;
        nameset_add(ns, e->d_name, strlen(e->d_name), S_ISDIR(st.st_mode));
    }
    closedir(d);
}

int vfs_list_dir(const char *rel, void (*cb)(void *ctx, const char *name, int is_dir), void *ctx) {
    NameSet ns = {0};
    int is_dir = 0;
    if (vfs_stat(rel, &is_dir, NULL) || !is_dir) return -1;
    list_loose(&ns, platform_save_dir(), rel);
    list_loose(&ns, platform_game_dir(), rel);
    size_t n = strlen(rel);
    for (uint32_t i = 0; i < g_nentries; i++) {
        const char *nm = g_entries[i].name;
        if (nm[0] == '\x01') continue; /* solid groups are internal */
        if (n && (strncasecmp(nm, rel, n) || nm[n] != '/')) continue;
        const char *rest = nm + (n ? n + 1 : 0);
        const char *sl = strchr(rest, '/');
        nameset_add(&ns, rest, sl ? (size_t)(sl - rest) : strlen(rest), sl != NULL);
    }
    for (int i = 0; i < ns.n; i++) {
        cb(ctx, ns.names[i], ns.dirs[i]);
        free(ns.names[i]);
    }
    free(ns.names);
    free(ns.dirs);
    return 0;
}

/* ---- VFile ------------------------------------------------------------------------- */
enum { VF_STDIO, VF_STORED, VF_PIECES, VF_ECM, VF_SOLID };

struct VFile {
    int kind;
    FILE *fp;
    PackEntry *e;
    PiecesCache *pc;
    Ecm *ecm;
    int64_t pos;
    uint64_t base;   /* VF_SOLID: offset in the group's stream */
};

VFile *vfs_from_stdio(FILE *fp) {
    if (!fp) return NULL;
    VFile *f = calloc(1, sizeof *f);
    f->kind = VF_STDIO;
    f->fp = fp;
    return f;
}

VFile *vfs_open(const char *rel) {
    char path[1200];
    if (vfs_loose_path(rel, path, sizeof path) == 0) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) return vfs_from_stdio(fopen(path, "rb"));
    }
    PackEntry *e = g_pack_fd >= 0 ? pack_find(rel) : NULL;
    if (!e) return NULL;
    VFile *f = calloc(1, sizeof *f);
    f->e = e;
    switch (e->kind) {
    case PACK_STORED:
        f->kind = VF_STORED;
        break;
    case PACK_PIECES:
        f->kind = VF_PIECES;
        f->pc = pieces_load(e);
        if (!f->pc) { free(f); return NULL; }
        break;
    case PACK_SOLID:
        if (e->len >= g_nentries) { free(f); return NULL; }
        f->kind = VF_SOLID;
        f->pc = pieces_load(&g_entries[e->len]);
        f->base = e->off;
        if (!f->pc) { free(f); return NULL; }
        break;
    case PACK_ECM:
        f->kind = VF_ECM;
        f->ecm = ecm_open(e);
        if (!f->ecm) { free(f); return NULL; }
        break;
    default:
        free(f);
        return NULL;
    }
    return f;
}

int vfs_stat(const char *rel, int *is_dir, int64_t *size) {
    char path[1200];
    struct stat st;
    if (vfs_loose_path(rel, path, sizeof path) == 0 && stat(path, &st) == 0) {
        if (is_dir) *is_dir = S_ISDIR(st.st_mode);
        if (size) *size = st.st_size;
        return 0;
    }
    if (g_pack_fd < 0) return -1;
    PackEntry *e = pack_find(rel);
    if (e) {
        if (is_dir) *is_dir = 0;
        if (size) *size = (int64_t)e->size;
        return 0;
    }
    if (pack_is_dir(rel)) {
        if (is_dir) *is_dir = 1;
        if (size) *size = 0;
        return 0;
    }
    return -1;
}

size_t vfs_read(VFile *f, void *buf, size_t n) {
    if (f->kind == VF_STDIO) return fread(buf, 1, n, f->fp);
    int64_t size = (int64_t)f->e->size;
    if (f->pos >= size) return 0;
    if ((int64_t)n > size - f->pos) n = (size_t)(size - f->pos);
    size_t got = 0;
    switch (f->kind) {
    case VF_STORED:
        got = pack_pread(buf, n, f->e->off + (uint64_t)f->pos) ? 0 : n;
        break;
    case VF_PIECES:
        got = pieces_read(f->pc, (uint64_t)f->pos, buf, n);
        break;
    case VF_SOLID:
        got = pieces_read(f->pc, f->base + (uint64_t)f->pos, buf, n);
        break;
    case VF_ECM: {
        Ecm *m = f->ecm;
        uint8_t *d = buf;
        while (got < n) {
            uint64_t p = (uint64_t)f->pos + got;
            if (ecm_seek_to(m, p)) break;
            size_t in = (size_t)(p - m->cur_start), t = m->cur.len - in;
            if (t > n - got) t = n - got;
            memcpy(d + got, m->cur.data + in, t);
            got += t;
        }
        break;
    }
    }
    f->pos += (int64_t)got;
    return got;
}

size_t vfs_write(VFile *f, const void *buf, size_t n) {
    return f->kind == VF_STDIO ? fwrite(buf, 1, n, f->fp) : 0;
}

int64_t vfs_seek(VFile *f, int64_t off, int whence) {
    if (f->kind == VF_STDIO) {
        if (fseeko(f->fp, off, whence)) return -1;
        return ftello(f->fp);
    }
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->pos : (int64_t)f->e->size;
    if (base + off < 0) return -1;
    f->pos = base + off;
    return f->pos;
}

int64_t vfs_tell(VFile *f) { return f->kind == VF_STDIO ? ftello(f->fp) : f->pos; }

int64_t vfs_size(VFile *f) {
    if (f->kind != VF_STDIO) return (int64_t)f->e->size;
    struct stat st;
    fflush(f->fp);
    return fstat(fileno(f->fp), &st) ? -1 : st.st_size;
}

FILE *vfs_stdio(VFile *f) { return f->kind == VF_STDIO ? f->fp : NULL; }

void vfs_close(VFile *f) {
    if (!f) return;
    if (f->kind == VF_STDIO) fclose(f->fp);
    if (f->kind == VF_ECM) ecm_close(f->ecm);
    free(f);
}

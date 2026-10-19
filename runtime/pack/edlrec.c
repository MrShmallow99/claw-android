/* EDL blob reconstruction, see edlrec.h. */
#include <stdlib.h>
#include <string.h>
#include "edlrec.h"

#define MAX_MATCH 258
#define MAX_DIST 32767
#define TOO_FAR 4096
#ifndef SCAN_MAX
#define SCAN_MAX 258
#endif
#define CHAIN_LIMIT 4096
#ifndef HASH_BITS
#define HASH_BITS 16
#endif
#define HASH_SIZE (1 << HASH_BITS)

static const uint8_t LBASE[29] = {0,1,2,3,4,5,6,7,8,10,12,14,16,20,24,28,32,40,48,56,64,80,96,112,128,160,192,224,255};
static const uint8_t LEXTRA[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t DBASE[30] = {0,1,2,3,4,6,8,12,16,24,32,48,64,96,128,192,256,384,512,768,1024,1536,2048,3072,4096,6144,8192,12288,16384,24576};
static const uint8_t DEXTRA[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

/* ---- byte buffers ---- */
void bb_put(ByteBuf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + n) c *= 2;
        b->p = realloc(b->p, c);
        b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
void bb_u8(ByteBuf *b, unsigned v) {
    uint8_t c = (uint8_t)v;
    bb_put(b, &c, 1);
}
void bb_var(ByteBuf *b, uint64_t v) {
    while (v >= 0x80) {
        bb_u8(b, (unsigned)(v & 0x7f) | 0x80);
        v >>= 7;
    }
    bb_u8(b, (unsigned)v);
}
int br_u8(ByteRd *r, unsigned *v) {
    if (r->p >= r->end) return -1;
    *v = *r->p++;
    return 0;
}
int br_var(ByteRd *r, uint64_t *v) {
    uint64_t x = 0;
    for (int s = 0; s < 64; s += 7) {
        unsigned c;
        if (br_u8(r, &c)) return -1;
        x |= (uint64_t)(c & 0x7f) << s;
        if (!(c & 0x80)) {
            *v = x;
            return 0;
        }
    }
    return -1;
}

/* ---- matcher ---- */
static inline unsigned hash3(const uint8_t *s) { return (((unsigned)s[0] << 16 | (unsigned)s[1] << 8 | s[2]) * 2654435761u) >> (32 - HASH_BITS); }

int matcher_init(Matcher *m, const uint8_t *d, size_t n) {
    m->d = d;
    m->n = n;
    m->head = malloc(HASH_SIZE * sizeof(int32_t));
    m->prev = malloc((n + 1) * sizeof(int32_t));
    if (!m->head || !m->prev) return -1;
    memset(m->head, 0xff, HASH_SIZE * sizeof(int32_t));
    m->inserted = 0;
    m->cache_p = (size_t)-1;
    return 0;
}

void matcher_free(Matcher *m) {
    free(m->head);
    free(m->prev);
    m->head = m->prev = NULL;
}

static void insert_upto(Matcher *m, size_t p) {
    while (m->inserted < p) {
        size_t q = m->inserted++;
        if (q + 2 < m->n) {
            unsigned h = hash3(m->d + q);
            m->prev[q] = m->head[h];
            m->head[h] = (int32_t)q;
        }
    }
}

static void find_raw(Matcher *m, size_t p, size_t maxq, unsigned *len, unsigned *dist);

/* longest match at p using only earlier positions q <= maxq */
static void find_upto(Matcher *m, size_t p, size_t maxq, unsigned *len, unsigned *dist) {
    if (m->cache_p == p && m->cache_q == maxq) {
        *len = m->cache_len;
        *dist = m->cache_dist;
        return;
    }
    find_raw(m, p, maxq, len, dist);
    m->cache_p = p;
    m->cache_q = maxq;
    m->cache_len = *len;
    m->cache_dist = *dist;
}

static void find_raw(Matcher *m, size_t p, size_t maxq, unsigned *len, unsigned *dist) {
    *len = 0;
    *dist = 0;
    if (p + 2 >= m->n || p == 0) return;
    insert_upto(m, p);
    const uint8_t *s = m->d + p;
    size_t maxl = m->n - p < SCAN_MAX ? m->n - p : SCAN_MAX;
    unsigned best = 2, bestd = 0;
    int chain = CHAIN_LIMIT;
    for (int32_t q = m->head[hash3(s)]; q >= 0 && chain-- > 0; q = m->prev[q]) {
        if ((size_t)q > maxq) continue;
        size_t d = p - (size_t)q;
        if (d > MAX_DIST) break;
        const uint8_t *t = m->d + q;
        if (t[best] != s[best] || t[0] != s[0] || t[1] != s[1] || t[2] != s[2]) continue;
        unsigned l = 3;
        while (l < maxl && t[l] == s[l]) l++;
        if (l > best) {
            best = l;
            bestd = (unsigned)d;
            if (l >= maxl) break;
        }
    }
    if (best < 3) return;
    *len = best;
    *dist = bestd;
}

void matcher_find(Matcher *m, size_t p, unsigned *len, unsigned *dist) { find_upto(m, p, p - 1, len, dist); }

/* The original encoder is zlib-like with lazy matching, except that its
 * look-ahead search at p+1 runs before p itself is indexed, and a deferred
 * match found by the look-ahead is reused as-is. */
void edlrec_predict(Matcher *m, size_t p, int *state, unsigned *len, unsigned *dist) {
    unsigned l, d;
    if (*state && p >= 2) find_upto(m, p, p - 2, &l, &d);
    else if (*state) l = d = 0;
    else matcher_find(m, p, &l, &d);
    *len = 0;
    *dist = 0;
    if (l) {
        unsigned l1 = 0, d1 = 0;
        find_upto(m, p + 1, p - 1, &l1, &d1);
        if (l1 <= l) {
            *len = l < MAX_MATCH ? l : MAX_MATCH;
            *dist = d;
        }
    }
    *state = l != 0; /* meaningful only if the token taken at p is a literal */
}



int edlrec_lcode(unsigned len) {
    unsigned v = len - 3;
    if (v >= 255) return 257 + 28;
    int s = 27;
    while (LBASE[s] > v) s--;
    return 257 + s;
}

int edlrec_dcode(unsigned dist) {
    unsigned v = dist - 1;
    int s = 29;
    while (DBASE[s] > v) s--;
    return s;
}

/* ---- zlib-style Huffman lengths (trees.c build_tree / gen_bitlen) ---- */
#define MAXN 512
typedef struct { uint32_t freq; int dad; int len; } Node;

static int smaller(const Node *t, const uint8_t *depth, int n, int m) {
    return t[n].freq < t[m].freq || (t[n].freq == t[m].freq && depth[n] <= depth[m]);
}

static void pqdownheap(const Node *t, const uint8_t *depth, int *heap, int heap_len, int k) {
    int v = heap[k];
    int j = k << 1;
    while (j <= heap_len) {
        if (j < heap_len && smaller(t, depth, heap[j + 1], heap[j])) j++;
        if (smaller(t, depth, v, heap[j])) break;
        heap[k] = heap[j];
        k = j;
        j <<= 1;
    }
    heap[k] = v;
}

void edlrec_build_lengths(const uint32_t *freq, int elems, uint8_t *outlen) {
    Node t[2 * MAXN + 1];
    uint8_t depth[2 * MAXN + 1];
    int heap[2 * MAXN + 1];
    int heap_len = 0, heap_max = 2 * MAXN + 1, max_code = -1, node = elems;
    memset(t, 0, sizeof t);
    memset(depth, 0, sizeof depth);
    for (int n = 0; n < elems; n++) {
        t[n].freq = freq[n];
        if (freq[n]) {
            heap[++heap_len] = max_code = n;
            depth[n] = 0;
        } else {
            t[n].len = 0;
        }
    }
    while (heap_len < 2) {
        int nn = heap[++heap_len] = max_code < 2 ? ++max_code : 0;
        t[nn].freq = 1;
        depth[nn] = 0;
    }
    for (int n = heap_len / 2; n >= 1; n--) pqdownheap(t, depth, heap, heap_len, n);
    do {
        int n = heap[1];
        heap[1] = heap[heap_len--];
        pqdownheap(t, depth, heap, heap_len, 1);
        int m = heap[1];
        heap[--heap_max] = n;
        heap[--heap_max] = m;
        t[node].freq = t[n].freq + t[m].freq;
        depth[node] = (uint8_t)((depth[n] >= depth[m] ? depth[n] : depth[m]) + 1);
        t[n].dad = t[m].dad = node;
        heap[1] = node++;
        pqdownheap(t, depth, heap, heap_len, 1);
    } while (heap_len >= 2);
    heap[--heap_max] = heap[1];

    /* gen_bitlen */
    int bl_count[16] = {0};
    int overflow = 0;
    const int max_length = 15;
    t[heap[heap_max]].len = 0;
    int h;
    for (h = heap_max + 1; h < 2 * MAXN + 1; h++) {
        int n = heap[h];
        int bits = t[t[n].dad].len + 1;
        if (bits > max_length) bits = max_length, overflow++;
        t[n].len = bits;
        if (n > max_code) continue;
        bl_count[bits]++;
    }
    if (overflow) {
        do {
            int bits = max_length - 1;
            while (bl_count[bits] == 0) bits--;
            bl_count[bits]--;
            bl_count[bits + 1] += 2;
            bl_count[max_length]--;
            overflow -= 2;
        } while (overflow > 0);
        h = 2 * MAXN + 1;
        for (int bits = max_length; bits != 0; bits--) {
            int n = bl_count[bits];
            while (n != 0) {
                int m = heap[--h];
                if (m > max_code) continue;
                if (t[m].len != bits) t[m].len = bits;
                n--;
            }
        }
    }
    for (int n = 0; n < elems; n++) outlen[n] = n <= max_code ? (uint8_t)t[n].len : 0;
    if (max_code < 0) memset(outlen, 0, (size_t)elems);
}

/* ---- bit writer and canonical codes ---- */
typedef struct {
    uint8_t *p;
    size_t cap;
    uint64_t pos;
    int err;
} BitW;

static inline void putbit(BitW *w, unsigned b) {
    size_t i = (size_t)(w->pos >> 3);
    if (i >= w->cap) {
        w->err = 1;
        return;
    }
    if (b) w->p[i] |= (uint8_t)(1u << (w->pos & 7));
    w->pos++;
}
static inline void putbits(BitW *w, unsigned v, int n) {
    for (int i = 0; i < n; i++) putbit(w, (v >> i) & 1);
}

typedef struct {
    int nsym;
    uint8_t len[MAXN];
    uint16_t code[MAXN];
} Code;

static void code_build(Code *c) {
    int count[16] = {0}, next[16];
    for (int i = 0; i < c->nsym; i++) count[c->len[i]]++;
    count[0] = 0;
    int code = 0;
    for (int l = 1; l < 16; l++) {
        code = (code + count[l - 1]) << 1;
        next[l] = code;
    }
    for (int i = 0; i < c->nsym; i++)
        if (c->len[i]) c->code[i] = (uint16_t)next[c->len[i]]++;
}
static inline void put_sym(BitW *w, const Code *c, int s) {
    int l = c->len[s];
    for (int i = l - 1; i >= 0; i--) putbit(w, (c->code[s] >> i) & 1);
}

/* ---- rebuild ---- */
typedef struct { uint32_t len, dist; } Tok;

long edlrec_blob_size(const ByteRd *r0) {
    ByteRd r = *r0;
    unsigned type;
    uint64_t csize;
    if (br_u8(&r, &type) || br_var(&r, &csize)) return -1;
    return (long)csize;
}

long edlrec_rebuild(const uint8_t *dec, size_t n, ByteRd *r, uint8_t *out, size_t outcap) {
    unsigned type;
    uint64_t csize;
    if (br_u8(r, &type) || br_var(r, &csize)) return -1;
    if (csize < 12 || csize > outcap) return -1;
    memset(out, 0, (size_t)csize);
    memcpy(out, "EDL", 3);
    out[3] = (uint8_t)type;
    for (int i = 0; i < 4; i++) out[4 + i] = (uint8_t)(csize >> (8 * i));
    for (int i = 0; i < 4; i++) out[8 + i] = (uint8_t)((uint64_t)n >> (8 * i));
    if ((type & 0x7f) == 0) {
        if (12 + n > csize) return -1;
        memcpy(out + 12, dec, n);
        return (long)csize;
    }
    BitW w = {out + 12, (size_t)csize - 12, 0, 0};
    Matcher m;
    if (matcher_init(&m, dec, n)) return -1;
    Code lit, dist;
    lit.nsym = dist.nsym = 0;
    unsigned prevlen = 0;
    size_t pos = 0;
    int pstate = 0;
    uint64_t nblocks;
    Tok *toks = NULL;
    size_t tokcap = 0;
    long ret = -1;
    if (br_var(r, &nblocks)) goto done;
    for (uint64_t bi = 0; bi < nblocks; bi++) {
        unsigned kind;
        if (br_u8(r, &kind)) goto done;
        if (kind == 0) {
            uint64_t cnt;
            if (br_var(r, &cnt) || pos + cnt > n) goto done;
            putbit(&w, 0);
            putbits(&w, (unsigned)cnt, 15);
            for (uint64_t i = 0; i < cnt; i++) putbits(&w, dec[pos++], 8);
        } else {
            /* table headers */
            uint64_t nsf[2];
            ByteRd lenx[2];
            uint64_t nflag[2];
            ByteRd flagr[2];
            for (int t = 0; t < 2; t++) {
                if (br_var(r, &nsf[t])) goto done;
                lenx[t] = *r;
                if (r->p + nsf[t] > r->end) goto done;
                r->p += nsf[t];
                if (br_var(r, &nflag[t])) goto done;
                flagr[t] = *r;
                for (uint64_t i = 0; i < nflag[t]; i++) {
                    uint64_t v;
                    if (br_var(r, &v)) goto done;
                }
            }
            /* tokens */
            uint64_t blen, ncorr;
            if (br_var(r, &blen) || br_var(r, &ncorr)) goto done;
            size_t bend = pos + (size_t)blen, ntok = 0;
            if (bend > n) goto done;
            uint64_t skip = 0;
            int have_corr = 0;
            if (ncorr) {
                if (br_var(r, &skip)) goto done;
                have_corr = 1;
            }
            size_t p = pos;
            while (p < bend) {
                unsigned l, d;
                int st = pstate;
                edlrec_predict(&m, p, &st, &l, &d);
                if (have_corr && skip == 0) {
                    uint64_t cl, cd = 0;
                    if (br_var(r, &cl)) goto done;
                    if (cl && br_var(r, &cd)) goto done;
                    l = (unsigned)cl;
                    d = (unsigned)cd;
                    if (--ncorr) {
                        if (br_var(r, &skip)) goto done;
                    } else {
                        have_corr = 0;
                    }
                } else if (have_corr) {
                    skip--;
                }
                pstate = l ? 0 : st;
                if (l && (d == 0 || d > p || p + l > n)) goto done;
                if (ntok == tokcap) {
                    tokcap = tokcap ? tokcap * 2 : 65536;
                    toks = realloc(toks, tokcap * sizeof(Tok));
                }
                toks[ntok].len = l;
                toks[ntok].dist = d;
                ntok++;
                p += l ? l : 1;
            }
            if (p != bend || have_corr) goto done;
            /* frequencies -> predicted lengths -> actual lengths */
            uint32_t lf[286] = {0}, df[30] = {0};
            p = pos;
            for (size_t i = 0; i < ntok; i++) {
                if (toks[i].len) {
                    lf[edlrec_lcode(toks[i].len)]++;
                    df[edlrec_dcode(toks[i].dist)]++;
                    p += toks[i].len;
                } else {
                    lf[dec[p++]]++;
                }
            }
            lf[256]++;
            uint8_t pl[2][MAXN];
            memset(pl, 0, sizeof pl);
            edlrec_build_lengths(lf, 286, pl[0]);
            edlrec_build_lengths(df, 30, pl[1]);
            putbit(&w, 1);
            for (int t = 0; t < 2; t++) {
                Code *c = t == 0 ? &lit : &dist;
                putbits(&w, (unsigned)nsf[t], 9);
                if (!nsf[t]) continue;
                if (nsf[t] > MAXN) goto done;
                c->nsym = (int)nsf[t];
                uint64_t nextflag = (uint64_t)-1;
                uint64_t fl_left = nflag[t];
                uint64_t acc = 0;
                if (fl_left) {
                    br_var(&flagr[t], &nextflag);
                    acc = nextflag;
                }
                for (int i = 0; i < c->nsym; i++) {
                    unsigned x = 0;
                    br_u8(&lenx[t], &x);
                    unsigned len = (pl[t][i] ^ x) & 15;
                    int flag = len != prevlen;
                    if (fl_left && (uint64_t)i == acc) {
                        flag = !flag;
                        if (--fl_left) {
                            br_var(&flagr[t], &nextflag);
                            acc += nextflag + 1;
                        }
                    }
                    putbit(&w, (unsigned)flag);
                    if (flag) {
                        putbits(&w, len, 4);
                        prevlen = len;
                    }
                    c->len[i] = (uint8_t)prevlen;
                }
                code_build(c);
            }
            /* symbols */
            p = pos;
            for (size_t i = 0; i < ntok; i++) {
                if (toks[i].len) {
                    int s = edlrec_lcode(toks[i].len);
                    if (s >= lit.nsym || !lit.len[s]) goto done;
                    put_sym(&w, &lit, s);
                    putbits(&w, toks[i].len - 3 - LBASE[s - 257], LEXTRA[s - 257]);
                    int ds = edlrec_dcode(toks[i].dist);
                    if (ds >= dist.nsym || !dist.len[ds]) goto done;
                    put_sym(&w, &dist, ds);
                    putbits(&w, toks[i].dist - 1 - DBASE[ds], DEXTRA[ds]);
                    p += toks[i].len;
                } else {
                    if (dec[p] >= lit.nsym || !lit.len[dec[p]]) goto done;
                    put_sym(&w, &lit, dec[p]);
                    p++;
                }
            }
            if (lit.nsym <= 256 || !lit.len[256]) goto done;
            put_sym(&w, &lit, 256);
            pos = bend;
        }
        putbit(&w, bi + 1 == nblocks);
    }
    /* trailing bytes after the bitstream */
    {
        unsigned hi;
        uint64_t extra;
        if (br_u8(r, &hi) || br_var(r, &extra)) goto done;
        size_t i = (size_t)(w.pos >> 3);
        if (w.pos & 7) {
            if (i >= w.cap) goto done;
            w.p[i] |= (uint8_t)(hi << (w.pos & 7));
            i++;
        }
        if (i + extra != w.cap || (size_t)(r->end - r->p) < extra) goto done;
        memcpy(w.p + i, r->p, (size_t)extra);
        r->p += extra;
    }
    if (pos != n || w.err) goto done;
    ret = (long)csize;
done:
    free(toks);
    matcher_free(&m);
    return ret;
}

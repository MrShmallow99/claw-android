/* Exact reconstruction of the movies' EDL video blobs.
 *
 * An EDL blob is LZ77 + Huffman (Deflate-like) data produced by a zlib-style
 * encoder.  The pack stores the *decoded* bytes (which compress much better
 * with LZMA) plus a small "recon" record; edlrec_rebuild() re-runs a
 * zlib-style parser over the decoded bytes and, guided by the recon record,
 * writes back the original blob bit for bit. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t *p;
    size_t n, cap;
} ByteBuf;

void bb_put(ByteBuf *b, const void *d, size_t n);
void bb_u8(ByteBuf *b, unsigned v);
void bb_var(ByteBuf *b, uint64_t v);

typedef struct {
    const uint8_t *p, *end;
} ByteRd;
int br_u8(ByteRd *r, unsigned *v);
int br_var(ByteRd *r, uint64_t *v);

/* Rebuilds the EDL blob for decoded data `dec` (n bytes) from the recon record.
 * out must have room for the blob (its size is the first field of the record).
 * Returns the blob size or -1. */
long edlrec_rebuild(const uint8_t *dec, size_t n, ByteRd *recon, uint8_t *out, size_t outcap);
/* Size of the blob described by the recon record at r (without consuming it). */
long edlrec_blob_size(const ByteRd *r);

/* ---- shared with an encoder ---- */
typedef struct {
    const uint8_t *d;
    size_t n;
    int32_t *head, *prev;      /* hash chains */
    size_t inserted;
    size_t cache_p, cache_q;   /* last search, reused by the next position */
    unsigned cache_len, cache_dist;
} Matcher;
int matcher_init(Matcher *m, const uint8_t *d, size_t n);
void matcher_free(Matcher *m);
/* longest match at p (len, nearest dist among longest); len < 3 = none */
void matcher_find(Matcher *m, size_t p, unsigned *len, unsigned *dist);
/* the parser's choice at p: len 0 = literal.  *state carries the parser state
 * between tokens (start with 0); after the call it holds the state to use for
 * the next position if a literal is taken at p (after a match it is 0). */
void edlrec_predict(Matcher *m, size_t p, int *state, unsigned *len, unsigned *dist);
/* zlib-style Huffman code lengths (max 15) for freq[0..n) */
void edlrec_build_lengths(const uint32_t *freq, int n, uint8_t *len);
int edlrec_lcode(unsigned len);  /* length (3..258) -> symbol 257.. */
int edlrec_dcode(unsigned dist); /* distance (1..32768) -> symbol */

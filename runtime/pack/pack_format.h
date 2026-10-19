/* game.pak layout (little-endian), written by tools/mkpack.py.
 *
 *   header: "GPAK" u32 version, u32 nentries, u32 index_len, u64 index_off
 *   index:  per entry: u16 name_len, name, u8 kind, u64 size, u64 data_off, u64 data_len
 *
 * PACK_STORED  data is the file itself.
 * PACK_PIECES  u32 nblocks, nblocks x Block, u32 npieces, npieces x Piece, payloads.
 *              The file is a sequence of pieces; a RESID piece takes bytes from the
 *              "residual stream" (the blocks, back to back), a FLAC piece is 16-bit
 *              PCM sound stored as FLAC (optionally XOR-scrambled with a byte counter,
 *              as the game stores some sounds).
 * PACK_ECM     u32 nchunks, then three LZMA streams (Stream x 3): rest, dec, recon.
 *              recon: var header_len, then per chunk: var prefix_len, u8 mode,
 *              mode 0: var raw_len (body copied from rest),
 *              mode 1: var dec_len + edlrec record (body rebuilt from dec),
 *              and finally var tail_len.
 * PACK_SOLID   a file inside a solid group: data_off is its offset in the decoded
 *              stream of the group entry whose index position is data_len.
 * All LZMA data is in the .lzma ("alone") format: 5 property bytes, u64 size, stream. */
#pragma once
#include <stdint.h>

#define PACK_MAGIC "GPAK"
#define PACK_VERSION 1

enum { PACK_STORED = 0, PACK_PIECES = 1, PACK_ECM = 2, PACK_SOLID = 3 };
enum { BLOCK_RAW = 0, BLOCK_LZMA = 1 };
enum { PIECE_RESID = 0, PIECE_FLAC = 1 };

#define PACK_BLOCK_SIZE 20   /* u64 src_off, u32 src_len, u32 raw_len, u8 codec, 3 pad */
#define PACK_PIECE_SIZE 28   /* u64 out_off, u32 out_len, u8 type, u8 xor, u8 channels, u8 pad,
                                u64 arg (resid offset | flac src_off), u32 flac src_len */
#define PACK_STREAM_SIZE 24  /* u64 src_off, u64 src_len, u64 raw_len */

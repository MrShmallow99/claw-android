#!/usr/bin/env python3
"""Builds game.pak from the game folder.  Every file is stored losslessly; the
runtime (runtime/pack/vfs.c) gives the game back the exact original bytes.
tools/devicepack (runtime/pack/packer.c) builds the same pack without Python or the
ffmpeg command; this script is the reference implementation.

    tools/mkpack.py <game dir> -o game.pak [--config games/<name>/game.ini]

Each file goes to the handler chosen by the first matching game.ini [pack]
"rule = GLOB HANDLER" (default: lzma).  Handlers:
    lzma      LZMA blocks (or stored when that is smaller)
    solid     many small files of one folder compressed together (LZMA, with
              16-bit PCM sounds as FLAC pieces): far better than one stream each
    stored    as is
    skip      left out of the pack (only for files the game never opens!)
    wav       RIFF WAVE with 16-bit PCM: the samples as FLAC, the rest LZMA
Byte-identical files are stored once.  Add a handler for each new format worth it.
Verify the result with tools/packverify.c.  Layout: runtime/pack/pack_format.h."""
import argparse
import fnmatch
import hashlib
import lzma
import os
import struct
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)

PACK_STORED, PACK_PIECES, PACK_ECM, PACK_SOLID = 0, 1, 2, 3
SOLID_GROUP = 8 << 20  # bytes of files per solid group
BLOCK_RAW, BLOCK_LZMA = 0, 1
PIECE_RESID, PIECE_FLAC = 0, 1
RESID_BLOCK = 4 << 20


def lzma_alone(data, dict_size=None):
    """.lzma ("alone") stream: 5 property bytes, u64 size, data."""
    f = {'id': lzma.FILTER_LZMA1, 'preset': 9 | lzma.PRESET_EXTREME}
    f['dict_size'] = dict_size or max(1 << 16, min(1 << 26, len(data)))
    return lzma.compress(data, format=lzma.FORMAT_ALONE, filters=[f])


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw)


# ---- FLAC -------------------------------------------------------------------------
def strip_flac(data, keep_seektable):
    """Keeps STREAMINFO (and SEEKTABLE if asked) and the audio frames."""
    assert data[:4] == b'fLaC'
    p = 4
    blocks = []
    while True:
        hdr = data[p]
        last = hdr & 0x80
        btype = hdr & 0x7f
        ln = int.from_bytes(data[p + 1:p + 4], 'big')
        body = data[p + 4:p + 4 + ln]
        if btype == 0 or (btype == 3 and keep_seektable):
            blocks.append((btype, body))
        p += 4 + ln
        if last:
            break
    out = bytearray(b'fLaC')
    for i, (t, b) in enumerate(blocks):
        out.append(t | (0x80 if i == len(blocks) - 1 else 0))
        out += len(b).to_bytes(3, 'big') + b
    return bytes(out + data[p:])


def flac_encode_s16(pcm, channels, rate=22050):
    r = run(['ffmpeg', '-loglevel', 'error', '-f', 's16le', '-ar', str(rate), '-ac', str(channels), '-i', '-',
             '-c:a', 'flac', '-compression_level', '12', '-f', 'flac', '-'], input=pcm)
    return strip_flac(r.stdout, keep_seektable=False)


# ---- generic files (PACK_PIECES) ----------------------------------------------------
def build_pieces(data, flac_ranges):
    """flac_ranges: sorted list of (start, length, xor, channels, flac_bytes)."""
    pieces = []
    resid = bytearray()
    flacs = []
    pos = 0
    for start, length, xor, channels, fl in flac_ranges:
        if start > pos:
            pieces.append([pos, start - pos, PIECE_RESID, 0, 0, len(resid), 0])
            resid += data[pos:start]
        pieces.append([start, length, PIECE_FLAC, xor, channels, len(flacs), len(fl)])
        flacs.append(fl)
        pos = start + length
    if pos < len(data):
        pieces.append([pos, len(data) - pos, PIECE_RESID, 0, 0, len(resid), 0])
        resid += data[pos:]
    blocks = []
    for i in range(0, len(resid), RESID_BLOCK):
        raw = bytes(resid[i:i + RESID_BLOCK])
        z = lzma_alone(raw)
        blocks.append((BLOCK_LZMA, z, len(raw)) if len(z) < len(raw) else (BLOCK_RAW, raw, len(raw)))
    head = 4 + len(blocks) * 20 + 4 + len(pieces) * 28
    out = bytearray()
    payload = bytearray()
    out += struct.pack('<I', len(blocks))
    for codec, z, raw_len in blocks:
        out += struct.pack('<QIIB3x', head + len(payload), len(z), raw_len, codec)
        payload += z
    flac_off = []
    for fl in flacs:
        flac_off.append(head + len(payload))
        payload += fl
    out += struct.pack('<I', len(pieces))
    for out_off, out_len, ptype, xor, ch, arg, flen in pieces:
        if ptype == PIECE_FLAC:
            arg = flac_off[arg]
        out += struct.pack('<QIBBBxQI', out_off, out_len, ptype, xor, ch, arg, flen)
    assert len(out) == head
    return bytes(out + payload)


def plain_entry(data):
    """Small files: LZMA if it helps, else stored."""
    if len(data) < 64:
        return PACK_STORED, data
    body = build_pieces(data, [])
    return (PACK_PIECES, body) if len(body) < len(data) else (PACK_STORED, data)


# ---- RIFF WAVE: 16-bit PCM samples as FLAC -------------------------------------------
def wav_entry(data):
    """Generic: the data chunk of a 16-bit PCM WAV becomes a FLAC piece (kept only
    when it beats LZMA by 5%); headers and any other chunks go to the residual."""
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        return plain_entry(data)
    p, fmt, dat = 12, None, None
    while p + 8 <= len(data):
        cid, ln = data[p:p + 4], struct.unpack_from('<I', data, p + 4)[0]
        if cid == b'fmt ' and ln >= 16:
            fmt = struct.unpack_from('<HHIIHH', data, p + 8)
        elif cid == b'data':
            dat = (p + 8, min(ln, len(data) - p - 8))
        p += 8 + ln + (ln & 1)
    if not fmt or not dat:
        return plain_entry(data)
    tag, ch, rate, _, align, bits = fmt
    if tag != 1 or bits != 16 or ch not in (1, 2):
        return plain_entry(data)
    start, n = dat[0], dat[1] - dat[1] % (2 * ch)
    if n < 4096:
        return plain_entry(data)
    fl = flac_encode_s16(data[start:start + n], ch, rate)
    if len(fl) >= len(lzma.compress(data[start:start + n], preset=6)) * 0.95:
        return plain_entry(data)
    return PACK_PIECES, build_pieces(data, [(start, n, 0, ch, fl)])


# ---- solid groups: the files of a folder concatenated into one PIECES stream ---------
def wav_pcm16_range(data):
    """(offset, length, channels, rate) of a 16-bit PCM WAV's samples, or None."""
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        return None
    p, fmt, dat = 12, None, None
    while p + 8 <= len(data):
        cid, ln = data[p:p + 4], struct.unpack_from('<I', data, p + 4)[0]
        if cid == b'fmt ' and ln >= 16:
            fmt = struct.unpack_from('<HHIIHH', data, p + 8)
        elif cid == b'data':
            dat = (p + 8, min(ln, len(data) - p - 8))
        p += 8 + ln + (ln & 1)
    if not fmt or not dat or fmt[0] != 1 or fmt[5] != 16 or fmt[1] not in (1, 2):
        return None
    n = dat[1] - dat[1] % (2 * fmt[1])
    return (dat[0], n, fmt[1], fmt[2]) if n >= 8192 else None


def solid_group(paths):
    """-> (body, [(offset, length) per file])"""
    blob = bytearray()
    spans, flacs = [], []
    for path in paths:
        data = open(path, 'rb').read()
        off = len(blob)
        spans.append((off, len(data)))
        r = wav_pcm16_range(data)
        if r:
            start, n, ch, rate = r
            fl = flac_encode_s16(data[start:start + n], ch, rate)
            if len(fl) < n * 0.6:
                flacs.append((off + start, n, 0, ch, fl))
        blob += data
    return build_pieces(bytes(blob), flacs), len(blob), spans


# ---- main -------------------------------------------------------------------------------
def collect(game_dir):
    """name -> source path"""
    files = {}
    for root, dirs, names in os.walk(game_dir):
        for n in names:
            p = os.path.join(root, n)
            rel = os.path.relpath(p, game_dir).replace(os.sep, '/')
            if not rel.startswith('game.pak'):
                files[rel] = p
    return files


def load_rules(config):
    if not config:
        return [('*.wav', 'wav')]
    sys.path.insert(0, HERE)
    import gameini
    rules = []
    for v in gameini.get_all(gameini.load(config), 'pack', 'rule'):
        glob, handler = v.split()[:2]
        rules.append((glob.lower(), handler.lower()))
    return rules


def handler_for(name, rules):
    for glob, handler in rules:
        if fnmatch.fnmatchcase(name.lower(), glob) or fnmatch.fnmatchcase(os.path.basename(name).lower(), glob):
            return handler
    return 'lzma'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game_dir')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('-c', '--config', help='games/<name>/game.ini ([pack] rules)')
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count())
    a = ap.parse_args()
    rules = load_rules(a.config)
    files = collect(a.game_dir)
    handlers = {n: handler_for(n, rules) for n in files}
    for n in sorted(n for n, h in handlers.items() if h == 'skip'):
        print('  %-24s skipped' % n, file=sys.stderr)
    files = {n: p for n, p in files.items() if handlers[n] != 'skip'}
    unknown = sorted({h for h in handlers.values()} - {'skip', 'lzma', 'stored', 'wav', 'solid'})
    if unknown:
        sys.exit('unknown pack handler(s): %s' % ', '.join(unknown))
    # byte-identical files are stored once
    digest, first_of = {}, {}
    for n in sorted(files):
        h = hashlib.sha256(open(files[n], 'rb').read()).digest()
        digest[n] = h
        first_of.setdefault(h, n)
    entries = []
    # solid groups: unique files of one folder, in name order, up to SOLID_GROUP bytes each
    groups, cur_dir, cur, cur_size = [], None, [], 0
    for n in sorted(files, key=lambda x: (os.path.dirname(x).lower(), x.lower())):
        if handlers[n] != 'solid' or first_of[digest[n]] != n:
            continue
        d = os.path.dirname(n).lower()
        size = os.path.getsize(files[n])
        if cur and (d != cur_dir or cur_size + size > SOLID_GROUP):
            groups.append(cur)
            cur, cur_size = [], 0
        cur.append(n)
        cur_dir = d
        cur_size += size
    if cur:
        groups.append(cur)
    solid_of = {}
    with ProcessPoolExecutor(a.jobs) as ex:
        gfuts = [ex.submit(solid_group, [files[n] for n in g]) for g in groups]
        for gi, (g, fu) in enumerate(zip(groups, gfuts)):
            body, blob_len, spans = fu.result()
            gname = '\x01solid/%05d' % gi
            entries.append((gname, PACK_PIECES, blob_len, body))
            for n, (off, ln) in zip(g, spans):
                solid_of[n] = (gname, off)
            print('  solid %-40s %4d files %10d -> %10d' % (os.path.dirname(g[0])[:40], len(g), blob_len, len(body)),
                  file=sys.stderr)
        for n in sorted(files):
            if first_of[digest[n]] != n:
                print('  %-24s same bytes as %s, stored once' % (n, first_of[digest[n]]), file=sys.stderr)
                entries.append((n, 'alias', 0, first_of[digest[n]]))
                continue
            if n in solid_of:
                entries.append((n, 'solid', os.path.getsize(files[n]), solid_of[n]))
                continue
            data = open(files[n], 'rb').read()
            h = handlers[n]
            if h == 'stored':
                kind, body = PACK_STORED, data
            elif h == 'wav':
                kind, body = wav_entry(data)
            else:
                kind, body = plain_entry(data)
            entries.append((n, kind, len(data), body))
            print('  %-24s %10d -> %10d' % (n, len(data), len(body)), file=sys.stderr)
    # solid members and aliases must follow the entries they point to
    entries.sort(key=lambda e: (e[1] == 'solid') + 2 * (e[1] == 'alias'))
    # write
    with open(a.output, 'wb') as f:
        f.write(b'\0' * 24)
        index = bytearray()
        placed = {}
        position = {}
        for name, kind, size, body in entries:
            position[name] = len(placed)
            if kind == 'alias':
                kind, size, off, ln = placed[body]
            elif kind == 'solid':
                # data_off = offset in the group's decoded stream, data_len = the group's index position
                kind, off, ln = PACK_SOLID, body[1], position[body[0]]
            else:
                off = f.tell()
                f.write(body)
                ln = len(body)
            placed[name] = (kind, size, off, ln)
            nb = name.encode()
            index += struct.pack('<H', len(nb)) + nb + struct.pack('<BQQQ', kind, size, off, ln)
        ioff = f.tell()
        f.write(index)
        f.seek(0)
        f.write(b'GPAK' + struct.pack('<IIIQ', 1, len(entries), len(index), ioff))
    print('%s: %d entries, %.1f MB' % (a.output, len(entries), os.path.getsize(a.output) / 1e6), file=sys.stderr)


if __name__ == '__main__':
    main()

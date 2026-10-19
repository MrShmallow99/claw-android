#!/usr/bin/env python3
"""Triage a game folder before porting: is the recompile-and-reimplement approach a fit?

    tools/venv/bin/python tools/triage.py <game folder> [--exe NAME] [--recomp] [--json]

Reports the exe format, the imported APIs grouped by subsystem (and how many the
template runtime already implements), DLLs shipped with the game, signs of copy
protection or packing, instruction-set use, and the data files by type with how
compressible they are.  Ends with a verdict.  --recomp also runs the full code
discovery + code generation and lists unsupported instruction forms (slower).
It is a heuristic report: confirm anything surprising with REA / a disassembler."""
import argparse
import collections
import json
import lzma
import math
import os
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)

SYSTEM_DLLS = {
    'kernel32', 'user32', 'gdi32', 'advapi32', 'winmm', 'shell32', 'ole32', 'oleaut32', 'comctl32',
    'comdlg32', 'version', 'msvcrt', 'ntdll', 'imm32', 'ddraw', 'dsound', 'dinput', 'dinput8', 'd3d8',
    'd3d9', 'd3dim', 'd3drm', 'd3dx8', 'd3dx9', 'opengl32', 'glu32', 'wsock32', 'ws2_32', 'dplayx',
    'dplay', 'wininet', 'mpr', 'msacm32', 'msvfw32', 'avifil32', 'vfw32', 'winspool', 'lz32', 'shlwapi',
    'setupapi', 'rpcrt4', 'mfc42', 'msvcp60', 'msvcirt', 'msvcrt20', 'msvcrt40', 'wing32', 'dciman32',
    'quartz', 'strmiids', 'amstream', 'dxguid', 'imagehlp', 'psapi', 'secur32', 'crypt32', 'urlmon',
}
GROUPS = [
    ('3D (needs a GPU translation layer)', r'^(d3d.*|opengl32|glu32|glide.*|3dfx.*)$'),
    ('2D video', r'^(ddraw|wing32|dciman32|gdi32)$'),
    ('audio', r'^(dsound|msacm32|winmm)$'),
    ('input', r'^(dinput8?)$'),
    ('video playback', r'^(msvfw32|avifil32|vfw32|quartz|amstream)$'),
    ('network (usually stub)', r'^(wsock32|ws2_32|dplayx?|wininet)$'),
    ('C++ runtime DLLs (recompile or reimplement)', r'^(mfc.*|msvcp.*|msvcirt|msvcrt\d+)$'),
    ('core Win32', r'.*'),
]
# third-party middleware often shipped as DLLs next to the exe
MIDDLEWARE = {
    'smackw32': 'Smacker video (RAD)', 'binkw32': 'Bink video (RAD)', 'mss32': 'Miles Sound System',
    'mss16': 'Miles Sound System', 'ir32_32': 'Indeo video codec', 'ir41_32': 'Indeo video codec',
    'ir50_32': 'Indeo video codec', 'glide2x': '3dfx Glide 2', 'glide3x': '3dfx Glide 3', 'qmixer': 'QSound mixer',
    'a3d': 'Aureal A3D', 'eax': 'Creative EAX', 'dplayerx': 'SafeDisc component', 'drvmgt': 'SafeDisc component',
    'secdrv': 'SafeDisc driver', 'cms16': 'SecuROM component', 'cms_95': 'SecuROM component',
}
PROTECTION_SECTIONS = {
    'stxt774': 'SafeDisc', 'stxt371': 'SafeDisc', '.icd': 'SafeDisc', 'securom': 'SecuROM', '.cms_t': 'SecuROM',
    '.cms_d': 'SecuROM', 'upx0': 'UPX packer', 'upx1': 'UPX packer', '.aspack': 'ASPack packer',
    '.adata': 'ASPack packer', 'petite': 'Petite packer', '.perplex': 'Perplex packer', '.tsuarch': 'tElock',
    '.neolit': 'NeoLite packer', '.shrink': 'Shrinker packer', '.laser': 'LaserLock', 'pec1': 'PECompact',
    'pec2': 'PECompact', '.wwp32': 'WWPack32', '.vmp0': 'VMProtect', '.themida': 'Themida',
}
PROTECTION_FILES = {
    '00000001.tmp': 'SafeDisc', 'clcd32.dll': 'SafeDisc', 'clcd16.dll': 'SafeDisc', 'clokspl.exe': 'SafeDisc',
    'dplayerx.dll': 'SafeDisc', 'drvmgt.dll': 'SafeDisc', 'secdrv.sys': 'SafeDisc', 'cms16.dll': 'SecuROM',
    'cms_95.dll': 'SecuROM', 'cms_nt.dll': 'SecuROM', 'lasrlk.dll': 'LaserLock', 'nolaser.dll': 'LaserLock',
}
MAGICS = [
    (b'RIFF', 8, b'WAVE', 'WAV audio'), (b'RIFF', 8, b'AVI ', 'AVI video'), (b'SMK2', 0, None, 'Smacker video'),
    (b'SMK4', 0, None, 'Smacker video'), (b'BIK', 0, None, 'Bink video'), (b'KB2', 0, None, 'Bink 2 video'),
    (b'OggS', 0, None, 'Ogg'), (b'fLaC', 0, None, 'FLAC'), (b'ID3', 0, None, 'MP3'), (b'PK\x03\x04', 0, None, 'ZIP'),
    (b'MThd', 0, None, 'MIDI'), (b'BM', 0, None, 'BMP image'), (b'\x89PNG', 0, None, 'PNG image'),
    (b'\xff\xd8\xff', 0, None, 'JPEG image'), (b'GIF8', 0, None, 'GIF image'), (b'MZ', 0, None, 'DOS/Windows binary'),
    (b'FORM', 8, b'XMID', 'XMIDI music'), (b'Creative Voice', 0, None, 'VOC audio'), (b'\x1f\x8b', 0, None, 'gzip'),
    (b'BZh', 0, None, 'bzip2'), (b'7z\xbc\xaf', 0, None, '7z'), (b'MSCF', 0, None, 'CAB archive'),
]


def entropy(b):
    if not b:
        return 0.0
    c = collections.Counter(b)
    n = len(b)
    return -sum(v / n * math.log2(v / n) for v in c.values())


def pcm16_like(chunk):
    """True if the bytes look like 16-bit little-endian PCM audio (smooth signal:
    sample-to-sample differences much smaller than the samples)."""
    n = len(chunk) // 2
    if n < 4096:
        return False
    v = struct.unpack('<%dh' % n, chunk[:n * 2])
    amp = sum(abs(x) for x in v) / n
    diff = sum(abs(v[i] - v[i - 1]) for i in range(1, n)) / n
    return amp > 200 and diff < amp * 0.5


def magic_of(head):
    for m, off2, m2, name in MAGICS:
        if head.startswith(m) and (m2 is None or head[off2:off2 + len(m2)] == m2):
            return name
    if len(head) >= 64 and head[:3] == b'\x00\x00\x01' and head[3] in (0xba, 0xb3):
        return 'MPEG video'
    return None


def exe_format(path):
    d = open(path, 'rb').read(4096)
    if d[:2] != b'MZ':
        return 'not an MZ executable', None
    off = struct.unpack_from('<I', d, 0x3c)[0] if len(d) >= 0x40 else 0
    with open(path, 'rb') as f:
        f.seek(off)
        sig = f.read(4)
    if sig[:4] == b'PE\0\0':
        return 'PE', None
    if sig[:2] == b'NE':
        return 'NE (16-bit Windows 3.x)', 'Win16: not supported by this recompiler'
    if sig[:2] in (b'LE', b'LX'):
        return 'LE/LX (DOS extender, e.g. DOS/4GW)', 'DOS game: use a DOSBox-based port instead'
    return 'MZ (DOS)', 'DOS game: use a DOSBox-based port instead'


def implemented_imports():
    """imp_<DLL>_<Name> functions present in the template runtime."""
    have = set()
    for root, _, names in os.walk(os.path.join(PORT, 'runtime')):
        for n in names:
            if n.endswith('.c'):
                for m in re.finditer(r'\bimp_([A-Za-z0-9]+)_([A-Za-z0-9_@]+)\s*\(', open(os.path.join(root, n), errors='replace').read()):
                    have.add((m.group(1).upper(), m.group(2)))
    return have


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game_dir')
    ap.add_argument('--exe')
    ap.add_argument('--recomp', action='store_true')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    rep = collections.OrderedDict()
    blockers, warnings, notes = [], [], []

    files = []
    for root, _, names in os.walk(a.game_dir):
        for n in names:
            p = os.path.join(root, n)
            if os.path.isfile(p):
                files.append((os.path.relpath(p, a.game_dir).replace(os.sep, '/'), p, os.path.getsize(p)))
    exes = [f for f in files if f[0].lower().endswith('.exe')]
    if a.exe:
        exe = next((f for f in files if f[0].lower() == a.exe.lower()), None)
    else:  # the biggest exe that is not an installer/setup
        cands = [f for f in exes if not re.search(r'setup|install|uninst|autorun|launch|config|patch|_isdel|regist', f[0], re.I)]
        exe = max(cands or exes, key=lambda f: f[2], default=None)
    if not exe:
        sys.exit('no .exe found; pass --exe')
    rep['exe'] = exe[0]
    fmt, why = exe_format(exe[1])
    rep['format'] = fmt
    if why:
        blockers.append(why)

    if fmt == 'PE':
        import pefile
        pe = pefile.PE(exe[1])
        mach = pe.FILE_HEADER.Machine
        rep['machine'] = {0x14c: 'x86 (32-bit)', 0x8664: 'x86-64 (64-bit)'}.get(mach, hex(mach))
        if mach != 0x14c:
            blockers.append('not a 32-bit x86 exe: the recompiler handles x86-32 only')
        clr = pe.OPTIONAL_HEADER.DATA_DIRECTORY[14]
        if clr.VirtualAddress:
            blockers.append('.NET exe: decompile the managed code instead (not this pipeline)')
        rep['image_base'] = hex(pe.OPTIONAL_HEADER.ImageBase)
        rep['subsystem'] = {2: 'GUI', 3: 'console'}.get(pe.OPTIONAL_HEADER.Subsystem, pe.OPTIONAL_HEADER.Subsystem)
        if pe.OPTIONAL_HEADER.ImageBase + pe.OPTIONAL_HEADER.SizeOfImage > 0x80000000:
            warnings.append('image above 2 GB: adjust runtime/memory.c')
        if not hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
            notes.append('no relocations (normal for exes): the image must load at its preferred base, which the runtime does')
        # sections: protection / packing
        secs = []
        for s in pe.sections:
            name = s.Name.rstrip(b'\0').decode('latin1')
            data = s.get_data()
            e = entropy(data[:1 << 20])
            secs.append({'name': name, 'vsize': s.Misc_VirtualSize, 'rawsize': s.SizeOfRawData,
                         'entropy': round(e, 2), 'exec': bool(s.Characteristics & 0x20000000),
                         'write': bool(s.Characteristics & 0x80000000)})
            for k, v in PROTECTION_SECTIONS.items():
                if name.lower().startswith(k):
                    blockers.append('section %s: %s; the code is encrypted or packed, so you need an unprotected exe' % (name, v))
            if s.Characteristics & 0x20000000 and e > 7.2 and s.SizeOfRawData > 4096:
                warnings.append('executable section %s has entropy %.2f: probably packed or encrypted' % (name, e))
            if s.Characteristics & 0x20000000 and s.Characteristics & 0x80000000:
                warnings.append('section %s is writable and executable: possible self-modifying code' % name)
        rep['sections'] = secs
        if hasattr(pe, 'DIRECTORY_ENTRY_TLS') and pe.DIRECTORY_ENTRY_TLS.struct.AddressOfCallBacks:
            warnings.append('TLS callbacks present: run them before the entry point (runtime/main.c)')
        # imports
        have = implemented_imports()
        imports = collections.defaultdict(list)
        for entry in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
            dll = entry.dll.decode('latin1').lower().rsplit('.', 1)[0]
            for imp in entry.imports:
                nm = imp.name.decode('latin1') if imp.name else 'ord%d' % imp.ordinal
                imports[dll].append(nm)
        groups = collections.OrderedDict((g, {}) for g, _ in GROUPS)
        total = done = 0
        for dll, names in sorted(imports.items()):
            g = next(g for g, rx in GROUPS if re.match(rx, dll))
            impl = [n for n in names if (dll.upper(), n) in have]
            groups[g][dll] = {'count': len(names), 'implemented': len(impl),
                              'missing': sorted(set(names) - set(impl))}
            total += len(names)
            done += len(impl)
            if dll not in SYSTEM_DLLS:
                warnings.append('imports from %s.dll (%s): not a Windows DLL; recompile it too or reimplement it'
                                % (dll, MIDDLEWARE.get(dll, 'shipped with the game?')))
        rep['imports'] = {g: v for g, v in groups.items() if v}
        rep['imports_total'] = total
        rep['imports_already_implemented'] = done
        if any(re.match(GROUPS[0][1], d) for d in imports):
            blockers.append('imports a 3D API (%s): the template only presents software-rendered frames; '
                            'you need a Direct3D/OpenGL/Glide to GLES translation layer'
                            % ', '.join(d for d in imports if re.match(GROUPS[0][1], d)))
        strs = open(exe[1], 'rb').read()
        dyn = sorted({m.decode('latin1').lower() for m in re.findall(rb'([A-Za-z0-9_]{3,16}\.(?:dll|DLL))', strs)}
                     - {d + '.dll' for d in imports})
        if dyn:
            # LoadLibrary'd DLLs: their exports are served by runtime/win32/modules.c
            rep['dlls_named_in_strings'] = dyn
            notes.append('DLLs named in the exe strings, probably loaded with LoadLibrary at runtime (their '
                         'exports go in runtime/win32/modules.c): %s' % ', '.join(dyn))
            d3 = [d for d in dyn if re.match(GROUPS[0][1], d[:-4])]
            if d3:
                warnings.append('may load a 3D API at runtime (%s): fine if the game also has a software '
                                'renderer (pick it in its config), otherwise a GPU translation layer is needed'
                                % ', '.join(d3))
        if 'ddraw' in imports and re.search(rb'Direct3D|IDirect3D|d3dim', strs):
            warnings.append('ddraw import plus Direct3D strings: check whether the game requires D3D or has a '
                            'software renderer you can select')
        # instruction-set scan (linear sweep of executable sections: a heuristic)
        try:
            from capstone import Cs, CS_ARCH_X86, CS_MODE_32
            md = Cs(CS_ARCH_X86, CS_MODE_32)
            mn = collections.Counter()
            for s in pe.sections:
                if s.Characteristics & 0x20000000:
                    code, base, off = s.get_data(), pe.OPTIONAL_HEADER.ImageBase + s.VirtualAddress, 0
                    while off < len(code):  # disasm_lite stops at undecodable bytes: skip them
                        last = off
                        for addr, size, m, _ in md.disasm_lite(code[off:], base + off):
                            mn[m] += 1
                            last = addr - base + size
                        off = max(last, off) + 1 if last == off or last < len(code) else last
            sse = {k: v for k, v in mn.items() if re.match(r'^(.*ps|.*pd|.*ss|.*sd|movdq.|pshuf.|cvt.*|ucomis.|comis.|ldmxcsr|stmxcsr|xorp.|andp.|orp.)$', k)
                   and ' ' not in k and k not in ('movsd', 'cmpsd', 'stosd', 'lodsd', 'scasd', 'insd', 'outsd')}
            amd = {k: v for k, v in mn.items() if k in ('femms', 'pavgusb', 'pfadd', 'pfmul', 'pfsub', 'pf2id', 'pi2fd', 'prefetch')}
            mmx = sum(v for k, v in mn.items() if k.startswith(('padd', 'psub', 'pmul', 'punpck', 'pack', 'psll', 'psrl', 'psra', 'pand', 'por', 'pxor')) or k in ('emms', 'movq', 'movd'))
            rep['cpu'] = {'mmx_insns': mmx, 'sse_like': dict(sorted(sse.items(), key=lambda x: -x[1])[:12]),
                          '3dnow': amd}
            if sum(sse.values()) > 50:
                warnings.append('SSE instructions present (%d, a linear-sweep estimate): codegen.py needs SSE support'
                                % sum(sse.values()))
            if amd:
                warnings.append('3DNow! instructions present: usually an optional code path; check the CPU detection')
        except ImportError:
            notes.append('capstone not installed: no instruction-set scan')

    # protection files and shipped DLLs
    for rel, p, sz in files:
        b = os.path.basename(rel).lower()
        if b in PROTECTION_FILES:
            blockers.append('%s present: %s copy protection; you need an unprotected exe' % (rel, PROTECTION_FILES[b]))
    shipped = [rel for rel, p, sz in files if rel.lower().endswith('.dll')]
    if shipped:
        rep['shipped_dlls'] = {d: MIDDLEWARE.get(os.path.basename(d).lower()[:-4], '') for d in shipped}

    # data files by type
    by_type = collections.defaultdict(lambda: {'files': 0, 'bytes': 0, 'samples': []})
    for rel, p, sz in files:
        with open(p, 'rb') as f:
            head = f.read(64)
        kind = magic_of(head) or ('.' + rel.rsplit('.', 1)[-1].lower() if '.' in os.path.basename(rel) else '(no ext)')
        t = by_type[kind]
        t['files'] += 1
        t['bytes'] += sz
        t['samples'].append((sz, p))
    data = []
    for kind, t in sorted(by_type.items(), key=lambda x: -x[1]['bytes']):
        ratio = None
        if t['bytes'] > 256 << 10:
            raw = zb = pcm = 0
            for sz, p in sorted(t['samples'], reverse=True)[:3]:
                with open(p, 'rb') as f:
                    f.seek(max(0, sz // 2 - (512 << 10)))
                    chunk = f.read(1 << 20)
                raw += len(chunk)
                zb += len(lzma.compress(chunk, preset=6))
                pcm += sum(pcm16_like(chunk[i:i + 65536]) for i in range(0, len(chunk), 65536))
            ratio = round(zb / raw, 3) if raw else None
        hint = ''
        if ratio is not None:
            if pcm >= 3 and 'video' not in kind:
                hint = 'parts look like 16-bit PCM: FLAC (find the sounds; see the solid and wav handlers)'
            elif ratio > 0.95:
                hint = 'already compressed: only a codec-aware re-encode would help'
            elif 'WAV' in kind or kind in ('.raw', '.pcm', '.snd'):
                hint = 'PCM: try FLAC (wav handler)'
            elif ratio < 0.6:
                hint = 'compresses well with LZMA'
        data.append({'type': kind, 'files': t['files'], 'mb': round(t['bytes'] / 1e6, 2),
                     'lzma_ratio': ratio, 'hint': hint})
    rep['data'] = data
    rep['total_mb'] = round(sum(f[2] for f in files) / 1e6, 1)
    small = [rel for rel, p, sz in files if rel.lower().endswith(('.ecm', '.smk', '.bik', '.avi', '.mpg')) and sz < 64 << 10]
    if small:
        notes.append('tiny video files (%s...): the install may hold placeholders, so get the full files from the CD'
                     % ', '.join(small[:3]))

    if a.recomp and fmt == 'PE' and not blockers:
        sys.path.insert(0, os.path.join(PORT, 'recomp'))
        from codegen import Generator
        with tempfile.TemporaryDirectory() as td:
            g = Generator(exe[1], td, {}, False)
            g.run()
            rep['recomp'] = {'functions': len(g.disc.entries), 'discovery_notes': len(g.disc.notes),
                             'unsupported_forms': dict(sorted(g.unsupported.items(), key=lambda x: -x[1]))}
            if g.unsupported:
                warnings.append('%d unsupported instruction forms: extend recomp/codegen.py' % len(g.unsupported))

    rep['blockers'] = blockers
    rep['warnings'] = warnings
    rep['notes'] = notes
    if blockers:
        verdict = 'POOR FIT as is: resolve the blockers first (or choose another approach / version)'
    elif warnings:
        verdict = 'POSSIBLE: workable, with extra work for each warning'
    else:
        verdict = 'GOOD FIT: 32-bit Windows exe, no 3D API, no protection found'
    rep['verdict'] = verdict

    if a.json:
        print(json.dumps(rep, indent=1))
        return
    print('== %s  (%s, %s)' % (rep['exe'], rep['format'], rep.get('machine', '?')))
    if 'imports' in rep:
        print('\nImports: %d, already implemented in the template runtime: %d'
              % (rep['imports_total'], rep['imports_already_implemented']))
        for g, dlls in rep['imports'].items():
            print('  %s' % g)
            for dll, v in dlls.items():
                miss = v['missing']
                print('    %-12s %3d imports, %3d implemented%s' % (dll, v['count'], v['implemented'],
                      ('; missing: ' + ' '.join(miss[:12]) + (' ...' if len(miss) > 12 else '')) if miss else ''))
    if 'sections' in rep:
        print('\nSections: ' + ', '.join('%s(%s%s e=%.1f)' % (s['name'], 'x' if s['exec'] else '', 'w' if s['write'] else '',
                                                           s['entropy']) for s in rep['sections']))
    if 'cpu' in rep:
        c = rep['cpu']
        print('CPU: MMX %d insns; SSE-like %s; 3DNow! %s' % (c['mmx_insns'], c['sse_like'] or 'none', c['3dnow'] or 'none'))
    if 'shipped_dlls' in rep:
        print('\nDLLs shipped with the game: ' + ', '.join('%s%s' % (d, ' (%s)' % m if m else '') for d, m in rep['shipped_dlls'].items()))
    print('\nData (%.1f MB):' % rep['total_mb'])
    for d in rep['data'][:20]:
        print('  %-28s %5d files %9.2f MB  lzma %-6s %s' % (d['type'][:28], d['files'], d['mb'],
                                                         d['lzma_ratio'] if d['lzma_ratio'] is not None else '-', d['hint']))
    if 'recomp' in rep:
        r = rep['recomp']
        print('\nRecompile: %d functions, %d discovery notes, unsupported: %s' % (r['functions'], r['discovery_notes'],
                                                                            r['unsupported_forms'] or 'none'))
    for title, items in (('BLOCKERS', blockers), ('Warnings', warnings), ('Notes', notes)):
        if items:
            print('\n' + title + ':')
            for i in items:
                print('  - ' + i)
    print('\nVerdict: ' + verdict)


if __name__ == '__main__':
    main()

"""PE image loading and recursive-descent code discovery for a 32-bit Windows exe."""
import struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone import x86_const as X

BRANCH_JCC = {
    'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp', 'jnp',
    'jl', 'jge', 'jle', 'jg', 'jecxz', 'jcxz', 'loop', 'loope', 'loopne',
}


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.entry = self.base + self.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.size = self.pe.OPTIONAL_HEADER.SizeOfImage
        self.mem = bytearray(self.size)
        hdr = self.pe.get_memory_mapped_image()[:0x1000]
        self.mem[:len(hdr)] = hdr
        self.sections = []
        for s in self.pe.sections:
            va = self.base + s.VirtualAddress
            data = s.get_data()[:max(s.Misc_VirtualSize, 0) or s.SizeOfRawData]
            raw = data[:s.SizeOfRawData]
            off = s.VirtualAddress
            self.mem[off:off + len(raw)] = raw
            self.sections.append((s.Name.rstrip(b'\0').decode(), va, s.Misc_VirtualSize))
        text = [s for s in self.sections if s[0] == '.text'][0]
        self.text_lo, self.text_hi = text[1], text[1] + text[2]
        self.imports = {}  # iat address -> (dll, name)
        for e in self.pe.DIRECTORY_ENTRY_IMPORT:
            for i in e.imports:
                self.imports[i.address] = (e.dll.decode(), i.name.decode() if i.name else 'ord%d' % i.ordinal)
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True

    def in_text(self, a):
        return self.text_lo <= a < self.text_hi

    def u32(self, a):
        o = a - self.base
        return struct.unpack_from('<I', self.mem, o)[0]

    def u8(self, a):
        return self.mem[a - self.base]

    def decode(self, a):
        o = a - self.base
        for ins in self.md.disasm(bytes(self.mem[o:o + 16]), a, 1):
            return ins
        return None

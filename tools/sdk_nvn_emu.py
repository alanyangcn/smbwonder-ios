#!/usr/bin/env python3
"""Run the target sdk's own NVN implementation (the console driver code inside the sdk module) on the Mac with
unicorn, as ground truth for host NVN answers. The sdk carries its NVN name table (strings sdk+0xb3de9f, u32 offsets
+0xb41808) and implementation table (+0xd36fd8, one pointer per name).

    from sdk_nvn_emu import SdkNvn
    nvn = SdkNvn()
    nvn.texture_storage(target=1, fmt=0x25, width=64, height=64, depth=1, levels=7, flags=0) -> (size, alignment)

Usage: sdk_nvn_emu.py <target> <format> <w> <h> <d> <levels> [flags] [samples]
"""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from nvn_inventory import load_nso, relocations
import symbolize
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_UNMAPPED, UC_HOOK_CODE, UC_HOOK_BLOCK
import collections
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, \
    UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

NAMES, OFFS, IMPL = 0xb3de9f, 0xb41808, 0xd36fd8
BASE, STACK, HEAP, TRAP, STUBS = 0x100000000, 0x7f000000, 0x7e000000, 0x7d000000, 0x7c000000

def plt_relocations(img):
    """JUMP_SLOT / GLOB_DAT relocations: GOT offset -> (symbol name, defined value or None)."""
    mod = struct.unpack_from('<I', img, 4)[0]; o = mod + struct.unpack_from('<i', img, mod + 4)[0]; tags = {}
    while True:
        t, v = struct.unpack_from('<QQ', img, o); o += 16
        if t == 0: break
        tags.setdefault(t, v)
    symtab, strtab = tags[6], tags[5]; out = {}
    for table, size in ((tags.get(23), tags.get(2)), (tags.get(7), tags.get(8))):
        if not table: continue
        for i in range(0, size, 24):
            off, info, add = struct.unpack_from('<QQq', img, table + i)
            if info & 0xffffffff not in (1025, 1026): continue   # GLOB_DAT, JUMP_SLOT
            sym = info >> 32; st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', img, symtab + 24 * sym)
            name = img[strtab + st_name:img.index(b'\0', strtab + st_name)].decode()
            out[off] = (name, st_value + add if st_shndx else None)
    return out

class SdkNvn:
    def __init__(self):
        img, _, _ = load_nso(symbolize.EXEFS / 'sdk')
        image = bytearray(img)
        rel = relocations(img)
        for off, add in rel.items(): struct.pack_into('<Q', image, off, BASE + add)
        # Imports resolve to stubs that stop with the import's name (none is expected in the queried paths);
        # symbols the sdk defines itself resolve into the image.
        self.stub_names = {}
        for n, (off, (name, value)) in enumerate(sorted(plt_relocations(img).items())):
            target = BASE + value if value is not None else STUBS + 4 * n
            if value is None: self.stub_names[target] = name
            struct.pack_into('<Q', image, off, target)
        self.impl = {}
        for i in range(2000):
            o = struct.unpack_from('<I', img, OFFS + 4 * i)[0]; a = NAMES + o
            end = img.find(b'\0', a)
            if end < 0 or end - a > 80: break
            name = img[a:end].decode(errors='replace')
            if not name or not name[0].isalpha(): break
            ptr = rel.get(IMPL + 8 * i)
            if ptr: self.impl[name] = BASE + ptr
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        size = (len(image) + 0xfff) & ~0xfff
        self.uc.mem_map(BASE, size); self.uc.mem_write(BASE, bytes(image))
        self.uc.mem_map(STACK - 0x100000, 0x100000); self.uc.mem_map(HEAP, 0x100000); self.uc.mem_map(TRAP, 0x1000)
        self.uc.mem_map(STUBS, 0x100000)
        self.uc.mem_write(TRAP, b'\x00\x00\x20\xd4')   # brk #0 (never reached: emulation stops at TRAP)
        self.fault = None
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        self.trail = collections.deque(maxlen=24)
        self.uc.hook_add(UC_HOOK_BLOCK, lambda uc, addr, size, data: self.trail.append(addr))
        self.device = HEAP + 0x80000            # zeroed stand-in; recorded if a call reads it
        # NvRmGpuDeviceChooseKind(nvrm device, attributes, &kind) asks the GPU driver for a PTE kind; there is no driver
        # here. choose_kind: None = report failure (the sdk then falls back), else the kind to report.
        self.choose_kind = None; self.kind_queries = []
        self.uc.hook_add(UC_HOOK_CODE, self._choose_kind, begin=BASE + 0x436f90, end=BASE + 0x436f90)
    def _choose_kind(self, uc, address, size, data):
        from unicorn.arm64_const import UC_ARM64_REG_X2
        attrs = bytes(uc.mem_read(uc.reg_read(UC_ARM64_REG_X1), 16)); out = uc.reg_read(UC_ARM64_REG_X2)
        self.kind_queries.append(attrs.hex())
        if self.choose_kind is None: uc.reg_write(UC_ARM64_REG_X0, 1)
        else: uc.mem_write(out, struct.pack('<I', self.choose_kind)); uc.reg_write(UC_ARM64_REG_X0, 0)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
    def _unmapped(self, uc, access, address, size, value, data):
        self.fault = address; return False
    def call(self, name, *args):
        fn = self.impl[name]; regs = [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4]
        for r, v in zip(regs, args): self.uc.reg_write(r, v & 0xffffffffffffffff)
        self.uc.reg_write(UC_ARM64_REG_SP, STACK - 0x1000); self.uc.reg_write(UC_ARM64_REG_LR, TRAP)
        self.fault = None
        try: self.uc.emu_start(fn, TRAP, count=1_000_000)
        except Exception as e:
            pc = self.uc.reg_read(UC_ARM64_REG_PC)
            trail = ' '.join(f'sdk+{a - BASE:#x}' if a >= BASE else hex(a) for a in self.trail)
            raise RuntimeError(f'trail: {trail}\n{name} faulted at {self.fault and hex(self.fault)} pc={pc:#x} import={self.stub_names.get(pc)}: {e}')
        return self.uc.reg_read(UC_ARM64_REG_X0)
    def texture_storage(self, target, fmt, width, height, depth, levels, flags=0, samples=0):
        b = HEAP
        self.uc.mem_write(b, b'\0' * 0x100)
        self.call('TextureBuilderSetDevice', b, self.device)
        self.call('TextureBuilderSetDefaults', b)
        self.call('TextureBuilderSetTarget', b, target)
        self.call('TextureBuilderSetFormat', b, fmt)
        self.call('TextureBuilderSetSize3D', b, width, height, depth)
        self.call('TextureBuilderSetLevels', b, levels)
        self.call('TextureBuilderSetFlags', b, flags)
        if samples: self.call('TextureBuilderSetSamples', b, samples)
        return self.call('TextureBuilderGetStorageSize', b), self.call('TextureBuilderGetStorageAlignment', b)

if __name__ == '__main__':
    a = [int(x, 0) for x in sys.argv[1:]]
    nvn = SdkNvn()
    for kind in (None, 0xfe, 0xdb):
        nvn.choose_kind = kind; size, align = nvn.texture_storage(*a)
        print(f'kind={kind} size={size:#x} alignment={align:#x} queries={nvn.kind_queries[-1:]}')

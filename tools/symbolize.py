#!/usr/bin/env python3
"""把日志里的 <模块>+<偏移>（如 BLOCKED_CALLER 的 sdk+0x38648c、main+0x68eff0）换成符号名。
Symbolize module+offset addresses from device logs against the update-262144 ExeFS NSOs.

  python3 tools/symbolize.py sdk+0x38648c main+0x68eff0      # nearest exported symbol (+ PLT stub targets)
  python3 tools/symbolize.py --dis 24 sdk+0x3863a8            # also disassemble N instructions (needs capstone)
  python3 tools/symbolize.py --find ProfanityFilter sdk        # list defined symbols whose demangled name matches
Needs lz4 (NSO segments); --dis needs capstone. Demangling uses c++filt if present.
"""
import argparse, bisect, shutil, struct, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from nvn_inventory import load_nso
EXEFS = ROOT / 'work/game/update-262144-nstool/0'

def demangle(names):
    if not names or not shutil.which('c++filt'): return list(names)
    out = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True).stdout.splitlines()
    return out if len(out) == len(names) else list(names)

class Module:
    def __init__(self, name):
        self.img, _, _ = load_nso(EXEFS / name)
        img = self.img
        mod = struct.unpack_from('<I', img, 4)[0]
        dyn = mod + struct.unpack_from('<i', img, mod + 4)[0]
        d, o = {}, dyn
        while True:
            t, v = struct.unpack_from('<QQ', img, o); o += 16
            if t == 0: break
            d.setdefault(t, v)
        symtab, strtab = d[6], d[5]
        self.symbols, names = [], []
        def name_at(i):
            n = struct.unpack_from('<I', img, symtab + i * 24)[0]
            return img[strtab + n:img.index(b'\0', strtab + n)].decode()
        for i in range((strtab - symtab) // 24):
            _, _, _, shndx, value, size = struct.unpack_from('<IBBHQQ', img, symtab + i * 24)
            if shndx and value: self.symbols.append((value, size, name_at(i)))
        self.symbols.sort(); self.starts = [s[0] for s in self.symbols]
        self.got = {}   # GOT slot -> imported/interposable symbol (JUMP_SLOT relocations)
        if 23 in d:
            for i in range(d.get(2, 0) // 24):
                off, info, _ = struct.unpack_from('<QQq', img, d[23] + i * 24)
                self.got[off] = name_at(info >> 32)

    def nearest(self, off):
        i = bisect.bisect_right(self.starts, off) - 1
        if i < 0: return None, 0, False
        value, size, name = self.symbols[i]
        return name, off - value, size == 0 or off - value < size

    def plt_target(self, off):
        """adrp x16; ldr x17, [x16, #lo]; add x16, x16, #lo; br x17 -> GOT symbol."""
        w = struct.unpack_from('<IIII', self.img, off)
        if (w[0] & 0x9f00001f) != 0x90000010 or (w[1] & 0xffc003ff) != 0xf9400211 or w[3] != 0xd61f0220: return None
        immlo, immhi = (w[0] >> 29) & 3, (w[0] >> 5) & 0x7ffff
        page = ((off & ~0xfff) + (((immhi << 2 | immlo) << 12) if not (immhi >> 18) else (((immhi << 2 | immlo) - (1 << 21)) << 12)))
        return self.got.get(page + ((w[1] >> 10) & 0xfff) * 8)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('addresses', nargs='*')
    ap.add_argument('--dis', type=int, default=0, metavar='N')
    ap.add_argument('--find', metavar='TEXT')
    a = ap.parse_args()
    modules = {}
    def module(n):
        if n not in modules: modules[n] = Module(n)
        return modules[n]
    if a.find:
        for m in a.addresses or ['sdk', 'main']:
            mm = module(m)
            for (v, sz, _), dn in zip(mm.symbols, demangle([s[2] for s in mm.symbols])):
                if a.find in dn: print(f'{m}+{v:#x} size={sz} {dn}')
        return
    for text in a.addresses:
        m, off = text.split('+'); off = int(off, 16); mm = module(m)
        name, delta, inside = mm.nearest(off)
        label = f'{demangle([name])[0]}+{delta:#x}' if name else '?'
        if not inside: label += ' (past symbol end: local function)'
        plt = mm.plt_target(off)
        if plt: label = f'PLT stub -> {demangle([plt])[0]}'
        print(f'{text}  {label}')
        if a.dis:
            import capstone
            md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
            for ins in md.disasm(mm.img[off:off + a.dis * 4], off):
                note = ''
                if ins.mnemonic == 'bl':
                    t = int(ins.op_str.lstrip('#'), 16); p = mm.plt_target(t)
                    n, dl, _ = mm.nearest(t)
                    note = f'   ; {demangle([p])[0]}' if p else (f'   ; {demangle([n])[0]}+{dl:#x}' if n else '')
                print(f'    {ins.address:#x}: {ins.mnemonic} {ins.op_str}{note}')

if __name__ == '__main__':
    main()

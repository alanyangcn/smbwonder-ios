#!/usr/bin/env python3
"""Extract every NVN shader program from the game's RomFS archive (work/build/wonder-romfs.wrom) for the offline shader
build (wonder_shader_build.py --offline). No game data in source.

BNSH layout (relative to the 'BNSH' magic, as found in SecredUber.Product.100.product.Nin_NX_NVN.bfsha; round153):
  header 0x60 bytes, 'grsc' block at +0x60: +0x1c u32 variation count, +0x20 u64 variation array offset;
  variation 0x40 bytes: +0x10 u64 program offset;
  program: +0x08 u64 stage info offsets[6] (vertex, hull, domain, geometry, fragment, compute; 0 = absent);
  stage info: +0x08 u64 control offset, +0x10 u64 code offset, +0x18 u32 code size, +0x1c u32 control size (0x880).
The control section starts with 0x98761234 and the code with 0x12345678 (graphics) or 0x12345679 (compute). Keys are the first 16 hex digits of
sha256(control[0:0x880] || code[0:length]) with length = the u32 at control +0x704, as runtime/nvn_draw_census.h computes them (round191:
a control section alone is shared by stages with different code).
Output (<out>/): stages/<key>.control.bin, stages/<key>.code.bin, programs.json {"programs": [{"vs","fs"} | {"cs"}],
"sources": {key: romfs path}}, census/<program>/ (see write_census) and census-dirs.txt.
Usage: extract_shaders.py <wonder-romfs.wrom> --out <dir>
"""
import argparse, hashlib, json, struct
from pathlib import Path

CONTROL_MAGIC, CODE_MAGICS = 0x98761234, (0x12345678, 0x12345679)   # graphics, compute
STAGE_NAMES = ('vs', 'hs', 'ds', 'gs', 'fs', 'cs')
CONTAINERS = ('bfsha', 'sarc', 'esetb', 'blarc', 'bfarc', 'genvb')

def romfs_entries(path):
    f = open(path, 'rb'); magic, count, index_len, _ = struct.unpack('<8sQQQ', f.read(32))
    if magic != b'WONDER01': raise ValueError(f'{path}: not a WONDER01 archive')
    index = f.read(index_len); at, offset = 0, 32 + index_len
    for _ in range(count):
        length, size = struct.unpack_from('<HQ', index, at); at += 10
        yield index[at:at + length].decode(), offset, size, f
        at += length; offset += size

def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def stage_key(control, code): return hashlib.sha256(control[:0x880] + code[:u32(control, 0x704)]).hexdigest()[:16]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]

def bnsh_programs(data, base):
    """Programs of the BNSH at data[base:], as lists of 6 (control, code) byte pairs or None."""
    grsc = base + 0x60
    if data[grsc:grsc + 4] != b'grsc': raise ValueError(f'BNSH at {base:#x} has no grsc block at +0x60')
    count, array = u32(data, grsc + 0x1c), u64(data, grsc + 0x20)
    for v in range(count):
        program = u64(data, base + array + 0x40 * v + 0x10)
        if not program: continue
        stages = []
        for s in range(6):
            info = u64(data, base + program + 8 + 8 * s)
            if not info: stages.append(None); continue
            control, code = u64(data, base + info + 8), u64(data, base + info + 0x10)
            code_size, control_size = u32(data, base + info + 0x18), u32(data, base + info + 0x1c)
            c, k = data[base + control:base + control + control_size], data[base + code:base + code + code_size]
            if control_size < 0x880 or u32(c, 0) != CONTROL_MAGIC: raise ValueError(f'BNSH {base:#x} program {program:#x} stage {s}: bad control section')
            if code_size < 4 or u32(k, 0) != CODE_MAGICS[s == 5]: raise ValueError(f'BNSH {base:#x} program {program:#x} stage {s}: bad code')
            stages.append((c, k))
        yield stages

STAGE_NUMBERS = {'vs': 0, 'fs': 1, 'cs': 5}
LOCAL_SIZE, SHARED_SIZE, C1_OFFSET, CODE_LENGTH = 0x748, 0x710, 0x700, 0x704   # control offsets (runtime/nvn_draw_census.h/.c)

def write_census(out, programs):
    """One census-shaped directory per program (census/<vs>-<fs>, census/cs-<cs>) for wonder_shader_build.py: the
    stages' control / code and their sizes, no bindings ('offline': true). The build then answers texture types from
    the instructions and vertex attributes / colour outputs as float; the host checks those against each draw's
    bound formats and stops by name when they differ (nvn_queue.c)."""
    dirs = []
    for keys in programs:
        names = ['cs'] if 'cs' in keys else [n for n in ('vs', 'fs') if n in keys]
        if set(keys) - set(names) or not names or (names != ['cs'] and names != ['vs', 'fs']): continue   # hull/domain/geometry: not built
        d = out / 'census' / ('cs-' + keys['cs'] if names == ['cs'] else f"{keys['vs']}-{keys['fs']}")
        d.mkdir(parents=True, exist_ok=True); stages = []
        for index, n in enumerate(names):
            control = (out / 'stages' / f'{keys[n]}.control.bin').read_bytes(); code = (out / 'stages' / f'{keys[n]}.code.bin').read_bytes()
            (d / f'stage-{index}.control.bin').write_bytes(control); (d / f'stage-{index}.code.bin').write_bytes(code)
            stages.append({'index': index, 'stage': STAGE_NUMBERS[n], 'name': n, 'key': keys[n], 'code_size': u32(control, CODE_LENGTH), 'c1_offset': u32(control, C1_OFFSET)})
        census = {'offline': True, 'depth_mode': 0, 'window_origin': 1, 'program': {'stages': stages}, 'attribs': [], 'textures': [], 'images': [], 'targets': {'colors': []}}
        if names == ['cs']:
            control = (out / 'stages' / f"{keys['cs']}.control.bin").read_bytes()
            census['dispatch'] = {'groups': [0, 0, 0], 'local': list(struct.unpack_from('<3I', control, LOCAL_SIZE)), 'shared': u32(control, SHARED_SIZE)}
        (d / 'draw.json').write_text(json.dumps(census) + '\n'); dirs.append(str(d) + '/')
    (out / 'census-dirs.txt').write_text(' '.join(dirs) + '\n')
    return dirs

def extract(romfs, out):
    import zstandard
    (out / 'stages').mkdir(parents=True, exist_ok=True)
    programs, sources, seen = [], {}, set()
    for name, offset, size, f in romfs_entries(romfs):
        if not any(c in name for c in CONTAINERS): continue
        f.seek(offset); data = f.read(size)
        if name.endswith('.zs'): data = zstandard.ZstdDecompressor().decompressobj().decompress(data)
        at = data.find(b'BNSH')
        while at >= 0:
            for stages in bnsh_programs(data, at):
                keys = {}
                for s, pair in enumerate(stages):
                    if not pair: continue
                    control, code = pair; key = stage_key(control, code); keys[STAGE_NAMES[s]] = key
                    if key not in sources:
                        sources[key] = name
                        (out / 'stages' / f'{key}.control.bin').write_bytes(control); (out / 'stages' / f'{key}.code.bin').write_bytes(code)
                program = tuple(sorted(keys.items()))
                if program not in seen: seen.add(program); programs.append(keys)
            at = data.find(b'BNSH', at + 4)
    (out / 'programs.json').write_text(json.dumps({'programs': programs, 'sources': sources}, indent=0) + '\n')
    return programs, sources

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('romfs', type=Path); p.add_argument('--out', type=Path, required=True)
    a = p.parse_args(); programs, sources = extract(a.romfs, a.out); dirs = write_census(a.out, programs)
    kinds = {}
    for pr in programs: k = '+'.join(sorted(pr)); kinds[k] = kinds.get(k, 0) + 1
    print(f'{len(programs)} programs, {len(sources)} stages:', ', '.join(f'{v} {k}' for k, v in sorted(kinds.items())), f'; {len(dirs)} census directories')

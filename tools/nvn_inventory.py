#!/usr/bin/env python3
"""Wonder main 的 NVN 取址表、静态调用点与 API 版本要求。
Wonder main: NVN proc-address slots, static call sites and the API version gate.

Wonder 的加载函数把 nvnDeviceGetProcAddress 的结果经 GOT 写入 BSS 槽，写入发生在
下一次取址之前（str x0 存的是上一次 blr 的结果），与 D2R 的 nvn_slots.py 形态不同。
Usage: nvn_inventory.py <main.nso> <out.tsv> [--json out.json] [--header nvn_names.h]
"""
import argparse, json, struct, sys

LOADER = 0x1080ff0          # nvnLoadCProcs 等价函数（v1.2.0 / 262144），换版本须重新定位
VERSION_GATE = 0x5bb930     # GetInteger(API_MAJOR/MINOR) 与比较所在块


def load_nso(path):
    import lz4.block
    d = open(path, 'rb').read()
    if d[:4] != b'NSO0':
        raise ValueError('not an NSO')
    flags = struct.unpack_from('<I', d, 0xc)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    csz = struct.unpack_from('<III', d, 0x60)
    img = bytearray(segs[2][1] + segs[2][2])
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        if flags >> i & 1:
            raw = lz4.block.decompress(raw, uncompressed_size=sz)
        if len(raw) != sz:
            raise ValueError('segment size mismatch')
        img[mo:mo + sz] = raw
    return bytes(img), segs[0][2], d[0x40:0x60].hex()


def relocations(img):
    mod = struct.unpack_from('<I', img, 4)[0]
    if img[mod:mod + 4] != b'MOD0':
        raise ValueError('MOD0 missing')
    o = mod + struct.unpack_from('<i', img, mod + 4)[0]
    tags = {}
    while True:
        t, v = struct.unpack_from('<QQ', img, o); o += 16
        if t == 0:
            break
        tags.setdefault(t, v)
    rela, size = tags[7], tags[8]
    out = {}
    for i in range(0, size, 24):
        off, info, add = struct.unpack_from('<QQq', img, rela + i)
        if info & 0xffffffff == 1027:      # R_AARCH64_RELATIVE
            out[off] = add
    return out


def adrp(x, pc):
    imm = ((x >> 29) & 3) | (((x >> 5) & 0x7ffff) << 2)
    if imm & (1 << 20):
        imm -= 1 << 21
    return (pc & ~0xfff) + (imm << 12)


def cstr(img, a):
    return img[a:img.index(b'\0', a)].decode()


def parse_loader(img, rel, loader=LOADER):
    w = lambda pc: struct.unpack_from('<I', img, pc)[0]
    reg, cur, pending, slots, pc = {}, None, None, {}, loader
    while w(pc) != 0xd65f03c0:
        x = w(pc)
        if x & 0x9f000000 == 0x90000000:
            reg[x & 31] = adrp(x, pc)
        elif x & 0xffc00000 == 0x91000000 and (x & 31) == 1 and (x >> 5) & 31 in reg:
            cur = cstr(img, reg[(x >> 5) & 31] + ((x >> 10) & 0xfff))
        elif x & 0xffc00000 == 0xf9400000 and (x >> 5) & 31 in reg:      # ldr xD,[xN,#imm] from GOT
            got = reg[(x >> 5) & 31] + ((x >> 10) & 0xfff) * 8
            if got not in rel:
                raise ValueError('loader GOT slot %#x has no relocation' % got)
            reg[x & 31] = rel[got]
        elif x & 0xffc00000 == 0xf9000000 and (x & 31) == 0 and (x >> 5) & 31 in reg:
            if pending is None:
                raise ValueError('store before first lookup at %#x' % pc)
            slots[reg[(x >> 5) & 31] + ((x >> 10) & 0xfff) * 8] = pending
            pending = None
        elif x & 0xfffffc1f == 0xd63f0000:      # blr: this lookup's result is stored next
            if pending is not None:
                raise ValueError('lookup result for %s never stored' % pending)
            pending = cur
        pc += 4
    if pending is not None:                     # last result stored after final blr? handled by epilogue store
        raise ValueError('final lookup %s not stored' % pending)
    if len(set(slots.values())) != len(slots):
        raise ValueError('duplicate names in loader')
    return slots


def writes(x, r):
    op0 = (x >> 25) & 0xf
    if x & 0xfc000000 == 0x94000000 or x & 0xfffffc1f == 0xd63f0000:
        return r <= 18
    if op0 in (0b1000, 0b1001, 0b0101, 0b1101):
        return (x & 31) == r
    if op0 & 0b0101 == 0b0100:
        L = (x >> 22) & 1
        if (x >> 27) & 7 == 0b101:
            wb = (x >> 23) & 3 in (1, 3)
            return (L and ((x & 31) == r or ((x >> 10) & 31) == r)) or (wb and ((x >> 5) & 31) == r)
        if (x >> 27) & 7 == 0b111 and (x >> 24) & 3 == 0:
            wb = (x >> 21) & 1 == 0 and (x >> 10) & 3 in (1, 3)
            return (L and (x & 31) == r) or (wb and ((x >> 5) & 31) == r)
        if (x >> 27) & 7 == 0b111:
            return bool(L) and (x & 31) == r
        if (x >> 27) & 7 == 0b011:
            return (x & 31) == r
    return False


def call_sites(img, text_end, slots):
    import numpy as np
    w = np.frombuffer(img, dtype='<u4', count=text_end // 4)
    offs = {a & 0xfff for a in slots}
    cand = np.nonzero((w & 0xffc00000) == 0xf9400000)[0]
    uses = {a: [] for a in slots}
    for i in cand:
        x = int(w[i]); off = ((x >> 10) & 0xfff) * 8; rn = (x >> 5) & 31
        if off not in offs or rn == 31:
            continue
        for j in range(i - 1, max(i - 4000, -1), -1):
            y = int(w[j])
            if y == 0xd65f03c0:
                break
            if writes(y, rn):
                if y & 0x9f000000 == 0x90000000 and (y & 31) == rn and adrp(y, j * 4) + off in uses:
                    uses[adrp(y, j * 4) + off].append(int(i) * 4)
                break
    return uses


def version_gate(img, slots):
    """Return (major_required, minor_min) from the cmp immediates after GetInteger(0/1)."""
    getint = {v: k for k, v in slots.items()}['nvnDeviceGetInteger']
    found = []
    for pc in range(VERSION_GATE, VERSION_GATE + 0x60, 4):
        x = struct.unpack_from('<I', img, pc)[0]
        if x & 0x7f80001f == 0x7100001f:   # cmp wN,#imm
            found.append((x >> 10) & 0xfff)
    if len(found) < 2:
        raise ValueError('API version comparison not found at %#x' % VERSION_GATE)
    return {'getinteger_slot': '%x' % getint, 'major_equals': found[0], 'minor_at_least': found[1], 'site': '%x' % VERSION_GATE}


def write_header(path, module_id, slots, gate):
    names = [slots[s] for s in sorted(slots)]
    with open(path, 'w') as f:
        f.write('// Generated by tools/nvn_inventory.py --header; do not edit.\n'
                '// Wonder main %s loader %#x: %d names in BSS slot order.\n'
                '#pragma once\n#define WONDER_NVN_API_MAJOR %d\n#define WONDER_NVN_API_MINOR_MIN %d\n'
                '#define WONDER_NVN_NAME_COUNT %d\n' % (module_id[:16], LOADER, len(names), gate['major_equals'], gate['minor_at_least'], len(names)))
        f.write('#ifndef __ASSEMBLER__\nstatic const struct { const char *name; unsigned slot; } wonder_nvn_names[WONDER_NVN_NAME_COUNT] = {\n')
        for s in sorted(slots):
            f.write('    {"%s", %#x},\n' % (slots[s], s))
        f.write('};\n#endif\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('nso'); ap.add_argument('tsv'); ap.add_argument('--json')
    ap.add_argument('--header', help='write the loader name table (slot order) for runtime/nvn_bootstrap.c')
    a = ap.parse_args()
    img, text_end, module_id = load_nso(a.nso)
    slots = parse_loader(img, relocations(img))
    uses = call_sites(img, text_end, slots)
    with open(a.tsv, 'w') as f:
        f.write('# kind: evidence  Wonder main NVN slots and static call sites (nvn_inventory.py)\nname\tslot\tnuses\tfirst_use\n')
        for s in sorted(slots):
            u = uses[s]; f.write('%s\t%x\t%d\t%s\n' % (slots[s], s, len(u), '%x' % u[0] if u else '-'))
    gate = version_gate(img, slots)
    summary = {'module_id': module_id, 'loader': '%x' % LOADER, 'slots': len(slots),
               'called': sum(1 for s in slots if uses[s]), 'call_sites': sum(len(v) for v in uses.values()),
               'api_version': gate}
    if a.header:
        write_header(a.header, module_id, slots, gate)
    if a.json:
        json.dump(summary, open(a.json, 'w'), indent=2)
    print(json.dumps(summary))

if __name__ == '__main__':
    main()

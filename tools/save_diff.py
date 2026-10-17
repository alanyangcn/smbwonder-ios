#!/usr/bin/env python3
"""Diff two Wonder game_data.sav files flag by flag (save research for world unlocking, round208).

Format (game_data.sav, from GameData/GameDataList.Product.110.byml): header 0x20 bytes (u32 0x01020304, u32 1, u32 offset
of the data area); then (u32 hash, u32 value) entries grouped by type, separated by (0, type index) markers; hash =
MurmurHash3_x86_32(name, seed 0). Arrays: value = file offset of {u32 count, data}; BoolArray data is a bitset in u32
words, (U)IntArray / EnumArray u32 per element, UInt64Array u64. Definitions (types, defaults, enum names) come from
work/save-research/gdl.json (parsed GameDataList) and names recovered so far from work/save-research/gd_names.json.
Usage: save_diff.py <old game_data.sav> <new game_data.sav>
"""
import json, struct, sys
from pathlib import Path
RESEARCH = Path(__file__).resolve().parents[1] / 'work/save-research'

def load(path):
    d = Path(path).read_bytes()
    if struct.unpack_from('<I', d, 0)[0] != 0x01020304: raise ValueError(f'{path}: not a game_data.sav')
    n = (struct.unpack_from('<I', d, 8)[0] - 0x20) // 8
    return d, [struct.unpack_from('<II', d, 0x20 + 8 * i) for i in range(n)]

def array(d, t, off):
    n = struct.unpack_from('<I', d, off)[0]
    if t == 'BoolArray':
        return [(struct.unpack_from('<I', d, off + 4 + 4 * (i // 32))[0] >> (i % 32)) & 1 for i in range(n)]
    if t == 'UInt64Array': return list(struct.unpack_from('<%dQ' % n, d, off + 4))
    return list(struct.unpack_from('<%dI' % n, d, off + 4))

def flags(path, types):
    d, ents = load(path); out = {}
    for h, v in ents:
        if not h or h not in types: continue
        t = types[h][0]
        out[h] = (t, array(d, t, v) if t.endswith('Array') else v)
    return out

def main(old, new):
    gdl = json.loads((RESEARCH / 'gdl.json').read_text())['Data']
    names = {int(k): v for k, v in json.loads((RESEARCH / 'gd_names.json').read_text()).items()}
    types = {e['Hash']: (t, e) for t, es in gdl.items() for e in es}
    a, b = flags(old, types), flags(new, types)
    for h in sorted(set(a) | set(b), key=lambda h: names.get(h, str(h))):
        (t, x), (_, y) = a.get(h, (None, None)), b.get(h, (None, None))
        if x == y: continue
        name = names.get(h, f'#{h}')
        if t and t.endswith('Array') and x is not None and y is not None:
            changes = [(i, p, q) for i, (p, q) in enumerate(zip(x, y)) if p != q]
            print(f'{t:12s} {name:50s} len {len(y):3d} changed {changes}')
        else:
            print(f'{t:12s} {name:50s} {x} -> {y}')

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])

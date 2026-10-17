#!/usr/bin/env python3
"""Bold world unlock for a Wonder game_data.sav (round208, at the player's request; the original is backed up first).
Sets every 81-element BoolArray (world map point flags) to all true, every zero element of the 81-element UIntArrays
to 1 (non-zero values, e.g. the 7 seen in World 1, stay), and Motherseed00..05Finish to true. Every world struct's
IsVisitedWorld and IsHeardWorldNameByPrince members (GameDataList Struct entries: member name hash -> flag hash) are set
too: the grand world map shows a world's name only once it was visited or the prince named it, else "???"
(UIMapWorldNamePartsScreen, main+0x1d4b938 -> label "_Hidden"; Event_SetFlagVisitedWorld / ...HeardWorldNameByPrince). Array meanings are not
known per name (see tools/save_diff.py); this may leave story/demo state inconsistent.
Usage: save_unlock.py <in game_data.sav> <out game_data.sav>
"""
import json, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from save_diff import RESEARCH, load

def murmur3(data, seed=0):
    c1, c2 = 0xcc9e2d51, 0x1b873593; h = seed; n = len(data)
    for i in range(0, n // 4 * 4, 4):
        k = struct.unpack_from('<I', data, i)[0]; k = (k * c1) & 0xffffffff; k = ((k << 15) | (k >> 17)) & 0xffffffff; k = (k * c2) & 0xffffffff
        h ^= k; h = ((h << 13) | (h >> 19)) & 0xffffffff; h = (h * 5 + 0xe6546b64) & 0xffffffff
    t = data[n // 4 * 4:]; k = 0
    for i, b in enumerate(t): k |= b << (8 * i)
    if t: k = (k * c1) & 0xffffffff; k = ((k << 15) | (k >> 17)) & 0xffffffff; k = (k * c2) & 0xffffffff; h ^= k
    h ^= n; h ^= h >> 16; h = (h * 0x85ebca6b) & 0xffffffff; h ^= h >> 13; h = (h * 0xc2b2ae35) & 0xffffffff; h ^= h >> 16; return h

def unlock(src, dst):
    gdl = json.loads((RESEARCH / 'gdl.json').read_text())['Data']
    types = {e['Hash']: t for t, es in gdl.items() for e in es}
    d, ents = load(src); d = bytearray(d)
    seeds = {murmur3(f'Motherseed{i:02d}Finish'.encode()) for i in range(6)}
    members = {murmur3(b'IsVisitedWorld'), murmur3(b'IsHeardWorldNameByPrince')}
    world_flags = {m['Value'] for e in gdl['Struct'] for m in e['DefaultValue'] if m['Hash'] in members}
    counts = {'bool_arrays': 0, 'uint_elements': 0, 'seeds': 0, 'world_flags': 0}
    for i, (h, v) in enumerate(ents):
        t = types.get(h)
        if t in ('BoolArray', 'UIntArray') and struct.unpack_from('<I', d, v)[0] == 81:
            if t == 'BoolArray':
                struct.pack_into('<III', d, v + 4, 0xffffffff, 0xffffffff, (1 << (81 - 64)) - 1); counts['bool_arrays'] += 1
            else:
                for k in range(81):
                    if struct.unpack_from('<I', d, v + 4 + 4 * k)[0] == 0: struct.pack_into('<I', d, v + 4 + 4 * k, 1); counts['uint_elements'] += 1
        elif t == 'Bool' and h in seeds:
            struct.pack_into('<I', d, 0x20 + 8 * i + 4, 1); counts['seeds'] += 1
        elif t == 'Bool' and h in world_flags:
            struct.pack_into('<I', d, 0x20 + 8 * i + 4, 1); counts['world_flags'] += 1
    Path(dst).write_bytes(d)
    return counts

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    print(unlock(sys.argv[1], sys.argv[2]))

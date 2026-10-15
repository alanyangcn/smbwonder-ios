#!/usr/bin/env python3
"""Inventory embedded BNSH binary shader payloads; does not translate or render them.
Layout reference: https://github.com/KillzXGaming/BfshaLibrary/blob/main/ShaderLibrary/Structs.cs
and ShaderLibrary/Switch/BnshLoader.cs. Every pointer is bounded by its BNSH file.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

STAGES = ('vertex', 'tessellation_control', 'tessellation_evaluation', 'geometry', 'fragment', 'compute')

def inspect_bnsh(blob):
    def region(offset, size):
        if offset < 0 or size < 0 or offset + size > len(blob):
            raise ValueError(f'BNSH range outside container: {offset:#x}+{size:#x}')
        return blob[offset:offset + size]
    def read(fmt, offset):
        return struct.unpack(fmt, region(offset, struct.calcsize(fmt)))
    if region(0, 4) != b'BNSH':raise ValueError('Not a BNSH container')
    declared, = read('<I', 0x1c)
    if declared != len(blob):raise ValueError('BNSH declared size does not match container')
    block, = read('<H', 0x16)
    if region(block, 4) != b'grsc':raise ValueError('Unsupported BNSH first block')
    count, = read('<I', block + 0x1c)
    array, = read('<Q', block + 0x20)
    region(array, count * 0x40)
    variations = []
    for index in range(count):
        program, = read('<Q', array + index * 0x40 + 16)
        if not program:raise ValueError('Variation has no binary shader program')
        pointers = read('<6Q', program + 8)
        stages = []
        for name, pointer in zip(STAGES, pointers):
            if not pointer:continue
            region(pointer, 0x40)
            control, code, code_size, control_size = read('<QQII', pointer + 8)
            control_data = region(control, control_size)
            code_data = region(code, code_size)
            stages.append({'stage':name, 'bytecode_offset':code, 'bytecode_size':code_size,
                'bytecode_sha256':hashlib.sha256(code_data).hexdigest(),
                'control_offset':control, 'control_size':control_size,
                'control_sha256':hashlib.sha256(control_data).hexdigest()})
        variations.append({'index':index, 'program_offset':program, 'stages':stages})
    return {'size':declared, 'sha256':hashlib.sha256(blob).hexdigest(),
            'variation_count':count, 'variations':variations}


def inspect_file(path):
    source = path.read_bytes()
    data = source
    if path.suffix == '.zs':
        import zstandard
        # Game-specific resource budget; never accept unbounded compressed output.
        declared = zstandard.frame_content_size(data)
        if declared != zstandard.CONTENTSIZE_UNKNOWN and declared > 256 * 1024 * 1024:
            raise ValueError('Shader resource exceeds budget')
        data = zstandard.ZstdDecompressor().decompress(data, max_output_size=256 * 1024 * 1024)
        if len(data) > 256 * 1024 * 1024:raise ValueError('Shader resource exceeds budget')
    containers = []
    position = 0
    while True:
        position = data.find(b'BNSH', position)
        if position < 0:break
        if position + 0x20 > len(data):raise ValueError('Truncated embedded BNSH header')
        length, = struct.unpack_from('<I', data, position + 0x1c)
        if length < 0x20 or position + length > len(data):raise ValueError('Truncated embedded BNSH')
        record = inspect_bnsh(data[position:position + length])
        record['container_offset'] = position
        containers.append(record)
        position += length
    counts = Counter(stage['stage'] for record in containers for variation in record['variations'] for stage in variation['stages'])
    unique = {stage['bytecode_sha256'] for record in containers for variation in record['variations'] for stage in variation['stages']}
    return {'source_name':path.name, 'source_sha256':hashlib.sha256(source).hexdigest(),
            'decompressed_sha256':hashlib.sha256(data).hexdigest(), 'containers':containers,
            'stage_references':dict(counts), 'unique_bytecode_payloads':len(unique),
            'translated':False, 'reflection_validated':False}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', type=Path, nargs='+')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    records = [inspect_file(path) for path in args.inputs]
    args.output.write_text(json.dumps(records, indent=2) + '\n')
    for record in records:
        print(record['source_name'], record['stage_references'], 'unique payloads:', record['unique_bytecode_payloads'])

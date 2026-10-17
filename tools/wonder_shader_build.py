#!/usr/bin/env python3
"""Offline shader build from first-draw censuses (runtime/nvn_draw_census.h): Maxwell stage → SPIR-V (work/tools/wonder_xlate,
the Ryujinx translator front end) → MSL 3.2 with discrete bindings (spirv-cross) + a line-oriented reflection (.refl) the
host parses (runtime/shader_library.h). Translator queries are answered from the census, never defaulted:
texture targets from the bound texture descriptors, vertex attribute / fragment output component types from the bound
formats, depth remap from nvnDeviceSetDepthMode. Output: <key>.metal, <key>.refl, jobs.json, manifest.json.

Wonder SDK conventions (verified in sdk, not D2R's): uniform buffer r = c[3 + r] (BindUniformBuffer sdk+0x467950 writes
slot (r + 3) << 20); texture slot r handle = c[2][0x20 + 8r] (BindTexture sdk+0x468840 writes at r * 8 + 0x20).
.refl lines: stage vs|ps|cs; depth minus_one|zero (vertex); cbuf <r> <buffer>; cbufext <r> <bytes>; c1 <buffer> <hex>; support <buffer>; drv <bank>;
             drv 0 <buffer> (c[0] read only inside the buffer address tables, filled by the host);
             tex <r> <texture> <sampler> <type>; attr <location> <nvn attribute> <type>; fout <color> <type>;
             compute: local <x> <y> <z>; shared <bytes>; uptr <r> <buffer> (uniform slot r read through its address in
             c[0][0x210 + 16r]); img <r> <texture> <type> r|w|rw (image slot r = handle at c[2][0x120 + 8r]).
Usage: wonder_shader_build.py <census dir>... --out <dir>
"""
import argparse, hashlib, json, os, re, shutil, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
XLATE = ROOT / 'work/tools/wonder_xlate/wonder_xlate'
CROSS = ROOT / 'work/tools/spirv-cross/spirv-cross'
NVN_H = ROOT / 'vendor/d2r-mobile/dm/gfx/nvn/include/nvn.h'
CROSS_FLAGS = ['--msl', '--msl-ios', '--msl-version', '30200', '--msl-decoration-binding']
TEX_TABLE, TEX_SLOTS = 0x20, 32
IMG_TABLE, IMG_SLOTS = 0x120, 8   # nvnCommandBufferBindImage sdk+0x46a280: slot r at c[2][0x120 + 8r]
TARGETS = {0: '1d', 1: '2d', 2: '3d', 3: '1darray', 4: '2darray', 5: '2dms', 6: '2dmsarray', 7: '2d', 8: 'cube', 9: 'cubearray', 10: 'buffer'}
STAGES = {0: 'vs', 1: 'ps', 5: 'cs'}   # census stage → translator stage; others stop until a draw needs them

def sha(data): return hashlib.sha256(data).hexdigest()
_formats = None
def component_type(fmt):
    """NVN format → translator component type, from the format's name in nvn.h (…UI = uint, …I = sint, else float)."""
    global _formats
    if _formats is None:
        _formats = {int(v, 16): n for n, v in re.findall(r'NVN_FORMAT_(\w+) = (0x[0-9A-Fa-f]+)', NVN_H.read_text())}
    name = _formats.get(fmt)
    if not name or name in ('NONE', 'LARGE'): raise ValueError(f'NVN format {fmt} has no component type')
    return 'uint' if name.endswith('UI') else 'sint' if name.endswith('I') else 'float'
def texture_type(target):
    if target not in TARGETS: raise ValueError(f'NVN texture target {target} unknown')
    return TARGETS[target]
def image_slot(handle_word, cbuf_slot):
    """Translator image handle (word offset into c[2]) → NVN image slot."""
    if cbuf_slot not in (-1, 2): raise ValueError(f'image handle read from c[{cbuf_slot}], not c[2]')
    offset = handle_word * 4 - IMG_TABLE
    if offset < 0 or offset % 8 or offset // 8 >= IMG_SLOTS: raise ValueError(f'image handle word {handle_word} is outside c[2][0x120..0x160)')
    return offset // 8
def image_format(fmt):
    """NVN format → Ryujinx TextureFormat name for image load/store (R8 → R8Unorm, RGBA16F → R16G16B16A16Float, ...)."""
    global _formats
    if _formats is None: component_type(1)
    name = _formats.get(fmt, '')
    special = {'RGB10A2': 'R10G10B10A2Unorm', 'RGB10A2UI': 'R10G10B10A2Uint', 'R11G11B10F': 'R11G11B10Float'}
    if name in special: return special[name]
    m = re.fullmatch(r'(R|RG|RGBA)(8|16|32)(SN|UI|I|F)?', name)
    if not m: raise ValueError(f'NVN format {name or fmt} has no image format')
    comps, bits, kind = m.group(1), m.group(2), m.group(3) or ''
    return ''.join(f'{c}{bits}' for c in ('R', 'G', 'B', 'A')[:{'R': 1, 'RG': 2, 'RGBA': 4}[comps]]) + {'': 'Unorm', 'SN': 'Snorm', 'UI': 'Uint', 'I': 'Sint', 'F': 'Float'}[kind]
def texture_slot(handle_word, cbuf_slot):
    """Translator texture handle (32-bit word offset into the bindless constant buffer) → NVN texture slot."""
    if cbuf_slot not in (-1, 2): raise ValueError(f'texture handle read from c[{cbuf_slot}], not c[2]')
    offset = handle_word * 4 - TEX_TABLE
    if offset < 0 or offset % 8 or offset // 8 >= TEX_SLOTS: raise ValueError(f'texture handle word {handle_word} is outside c[2][0x20..0x120)')
    return offset // 8
def sampler_type_name(t):
    """Ryujinx SamplerType (1D 1, buffer 2, 2D 3, 3D 4, cube 5; Array 0x100, Multisample 0x200) -> reflection type."""
    base = {1: '1d', 2: 'buffer', 3: '2d', 4: '3d', 5: 'cube'}.get(t & 0xff)
    if not base: raise ValueError(f'sampler type {t:#x} unknown')
    return base + ('ms' if t & 0x200 else '') + ('array' if t & 0x100 else '')

STORAGE_BASE = {'vs': 0x110, 'cs': 0x310, 'ps': 0x510}   # Wonder SDK BindStorageBuffer table sdk+0xac558c (x16 bytes); compute per D2R dmsh_build
UNIFORM_POINTER_BASE = 0x210   # compute: uniform slot r is read through global memory at the address in c[0][0x210 + 16 r] (D2R dmsh_build)
def uniform_pointer_slot(cb_offset_words, stage):
    """Compute storage-style read whose address word lies in the uniform pointer table -> NVN uniform slot, else None."""
    offset = cb_offset_words * 4 - UNIFORM_POINTER_BASE
    return offset // 16 if stage == 'cs' and 0 <= offset < 0x100 and offset % 16 == 0 else None
def storage_slot(cb_offset_words, stage):
    """Storage buffer address word in c[0] -> NVN storage slot (c[0][base + 16 r])."""
    offset = cb_offset_words * 4 - STORAGE_BASE[stage]
    if offset < 0 or offset % 16 or offset // 16 > 15: raise ValueError(f'storage buffer address at c[0] word {cb_offset_words} is outside the {stage} table')
    return offset // 16

def driver_c0_ranges(stage):
    """Byte ranges of driver constant segment c[0] the host fills (nvn_queue.c driver_c0): the stage's storage buffer
    table (base + 16 r: u64 GPU address, u32 size) and, for compute, the uniform address table at 0x210 + 16 r."""
    ranges = [(STORAGE_BASE[stage], STORAGE_BASE[stage] + 0x100)]
    if stage == 'cs': ranges.append((UNIFORM_POINTER_BASE, UNIFORM_POINTER_BASE + 0x100))
    return ranges
def check_driver_c0(msl, binding, stage):
    """Reads of c[0] (the translator's global-to-storage rewrite subtracts buffer base addresses read from c[0]) must be
    constant offsets inside the tables the host fills; anything else would read zeros where Maxwell has driver data."""
    m = re.search(rf'constant \w+& (\w+) \[\[buffer\({binding}\)\]\]', msl)
    if not m: raise ValueError(f'c[0] binding {binding} missing from the MSL entry')
    name = m.group(1)
    if re.search(rf'wonder_cb\({name}\._m0,', msl): raise ValueError('c[0] read with a dynamic index')
    offsets = {int(i) * 16 for i in re.findall(rf'\b{name}\._m0\[(\d+)u?\]', msl)}
    bad = sorted(o for o in offsets if not any(lo <= o < hi for lo, hi in driver_c0_ranges(stage)))
    if bad: raise ValueError(f'c[0] read at {", ".join(hex(o) for o in bad)} outside the storage/uniform address tables')
def cbuf_kind(slot):
    if 3 <= slot <= 16: return ('cbuf', slot - 3)
    if slot == 1: return ('c1', None)
    if slot in (0, 2): return ('drv', slot)
    raise ValueError(f'constant buffer c[{slot}] is driver-internal')

# The load is clamped into range before the zero select: the compiler may evaluate both sides of a ternary (round69 bisect:
# the fault followed the shader, not its buffers or textures).
CB_GUARD = 'static inline __attribute__((unused)) float4 wonder_cb(constant float4 *a, int i) { float4 v = a[min(uint(i), 4095u)]; return uint(i) < 4096u ? v : float4(0.0); }\n'
def guard_cbuf_indices(msl):
    """Dynamic constant-buffer indices (<name>._m0[<expr>], expr not a literal) become wonder_cb(<name>._m0, <expr>):
    out-of-range reads return 0 as on Maxwell instead of faulting the GPU (every c[n] is declared float4[4096])."""
    out, i, pattern = [], 0, re.compile(r'\b(\w+_c\d+)\._m0\[')   # constant buffers only (vp_c3, fp_c1); not storage (vp_s0)
    while True:
        m = pattern.search(msl, i)
        if not m: out.append(msl[i:]); break
        depth, j = 1, m.end()
        while depth:
            depth += {'[': 1, ']': -1}.get(msl[j], 0); j += 1
        expr = msl[m.end():j - 1]
        out.append(msl[i:m.start()])
        out.append(msl[m.start():j] if re.fullmatch(r'\d+u?', expr.strip()) else f'wonder_cb({m.group(1)}._m0, {expr})')
        i = j
    text = ''.join(out)
    anchor = 'using namespace metal;\n'
    return text.replace(anchor, anchor + CB_GUARD, 1) if anchor in text else text

def support_bindings(msl):
    """[[buffer(n)]] entry parameters whose type is Ryujinx's support buffer (the struct holding render_scale float[73])."""
    structs = {m.group(1) for m in re.finditer(r'struct (\w+)\n\{[^}]*\bfloat4? _m\d+\[73\];[^}]*\}', msl)}   # float[73], or float4[73] when read
    m = re.search(r'^(?:vertex|fragment|kernel) \w+ main0\((.*)\)$', msl, re.M)
    if not m or not structs: return set()
    return {int(b) for t, b in re.findall(r'constant (\w+)& \w+ \[\[buffer\((\d+)\)\]\]', m.group(1)) if t in structs}

VARYING = re.compile(r'^\s+(\w+) \w+ \[\[user\(locn(\d+)\)\]\];', re.M)
def fragment_inputs(msl):
    """user(locnN) inputs of a fragment stage: {location: MSL type}."""
    m = re.search(r'struct main0_in\n\{(.*?)\n\};', msl, re.S)
    return {int(loc): t for t, loc in VARYING.findall(m.group(1))} if m else {}
def invariant_position(msl):
    """Vertex position as [[position, invariant]] (the host compiles with preserveInvariance): a depth pre-pass and the
    colour pass drawn with depth EQUAL (150 census draws) must compute bit-identical depth from the same inputs, as
    Maxwell does for identical instruction sequences (round161: course hedges showed the blue clear colour)."""
    out, n = re.subn(r'float4 gl_Position \[\[position\]\];', 'float4 gl_Position [[position, invariant]];', msl)
    if n != 1: raise ValueError(f'vertex stage declares [[position]] {n} times')
    return out

def pad_vertex_outputs(msl, wanted):
    """Declare every varying a fragment may read (locations 0..7, plus `wanted` {location: type} from paired fragments)
    that the vertex stage does not write: Metal rejects unmatched fragment inputs, Maxwell leaves them undefined; the
    added members are zero through `main0_out out = {};`."""
    m = re.search(r'(struct main0_out\n\{)(.*?)(\n\};)', msl, re.S)
    if not m or '[[position]]' not in m.group(2): return msl
    written = {int(loc) for _, loc in VARYING.findall(m.group(2))}
    needed = {loc: 'float4' for loc in range(8)}; needed.update(wanted)
    extra = ''.join(f'\n    {t} wonder_unwritten_locn{loc} [[user(locn{loc})]];' for loc, t in sorted(needed.items()) if loc not in written)
    return msl[:m.end(2)] + extra + msl[m.end(2):]

SAMPLER_SLOTS = 16   # Metal's per-stage sampler bindings; more go through an argument buffer (sampler_overflow)

def sampler_overflow(msl):
    """Samplers [[sampler(16..47)]] of a fragment entry move into an argument buffer `constant WonderSamplers&` at the
    highest free [[buffer(n)]]: the host writes their MTLResourceIDs there (round163: course ground fs 5fdc2502 binds
    17 samplers; left out, its draws were skipped and the ground showed the clear colour). Returns (msl, buffer or None)."""
    m = re.search(r'^(fragment) \w+ main0\((.*)\)$', msl, re.M)
    if not m: return msl, None
    params = m.group(2)
    over = [(name, int(i)) for name, i in re.findall(r', sampler (\w+) \[\[sampler\((\d+)\)\]\]', params) if int(i) >= SAMPLER_SLOTS]
    if not over: return msl, None
    if max(i for _, i in over) > 47: raise ValueError('sampler binding beyond 47')
    used = {int(b) for b in re.findall(r'\[\[buffer\((\d+)\)\]\]', params)}
    free = [b for b in range(30, -1, -1) if b not in used]
    if not free: raise ValueError('no free [[buffer(n)]] for the sampler argument buffer')
    buffer = free[0]; count = max(i for _, i in over) - SAMPLER_SLOTS + 1
    for name, i in over: params = params.replace(f', sampler {name} [[sampler({i})]]', '')
    params += f', constant WonderSamplers& wonder_smp [[buffer({buffer})]]'
    head, body = msl[:m.start()], msl[m.end():]
    for name, i in over: body = re.sub(rf'\b{name}\b', f'wonder_smp.s[{i - SAMPLER_SLOTS}]', body)
    decl = f'struct WonderSamplers\n{{\n    sampler s[{count}];\n}};\n// wonder_sampler_overflow {buffer} ' + ','.join(str(i) for _, i in over) + '\n\n'
    return head + decl + f'{m.group(1)} main0_out main0({params})' + body, buffer

def cbuf_extents(msl):
    """Bytes of each constant buffer parameter the stage reads (round222), by its [[buffer(n)]] binding: 16 x (largest
    literal float4 index + 1); 0 when any read uses a computed index (wonder_cb, guard_cbuf_indices). The host binds the
    game's buffer directly when this fits inside the bound size, instead of copying it into a zero-filled 64 KiB window."""
    m = re.search(r'^(?:vertex|fragment|kernel) \w+ main0\((.*)\)$', msl, re.M)
    names = dict(re.findall(r'constant \w+& (\w+_c\d+) \[\[buffer\((\d+)\)\]\]', m.group(1) if m else ''))
    out = {}
    for name, binding in names.items():
        if re.search(r'wonder_cb\(' + name + r'\._m0\b', msl): out[int(binding)] = 0; continue
        idx = [int(i) for i in re.findall(r'\b' + name + r'\._m0\[(\d+)u?\]', msl)]
        out[int(binding)] = 16 * (max(idx) + 1) if idx else 16
    return out

def entry_bindings(msl):
    """[[buffer(n)]] / [[texture(n)]] / [[sampler(n)]] parameters of the entry point, and stage_in attribute locations."""
    m = re.search(r'^(vertex|fragment|kernel) \w+ main0\((.*)\)$', msl, re.M)
    if not m: raise ValueError('no vertex/fragment/kernel main0 entry in MSL')
    params = m.group(2)
    ov = re.search(r'^// wonder_sampler_overflow (\d+) ([\d,]+)$', msl, re.M)
    return ({int(b) for b in re.findall(r'\[\[buffer\((\d+)\)\]\]', params)},
            {int(b) for b in re.findall(r'\[\[texture\((\d+)\)\]\]', params)},
            {int(b) for b in re.findall(r'\[\[sampler\((\d+)\)\]\]', params)} | ({int(i) for i in ov.group(2).split(',')} if ov else set()),
            sorted(int(a) for a in re.findall(r'\[\[attribute\((\d+)\)\]\]', msl)))

def job_for(census, stage_entry, code_path):
    stage_no, stage = stage_entry['stage'], STAGES.get(stage_entry['stage'])
    if not stage: raise ValueError(f"stage {stage_entry['name']} is not built yet")
    job = {'key': stage_entry['key'], 'stage': stage, 'code': str(code_path), 'tex': {}, 'img': {}, 'attr': {}, 'fout': {},
           # DebugMode (no Ryujinx AST optimizer) for every stage, as D2R dmsh_build defaults: the optimizer merges indirect-branch
           # (BRX) switch arms (round138: text vs 982e87b1faa0b87f kept 1 of 3 jump-table targets per BRX, so glyph UVs came from
           # the wrong fields and every text fragment was discarded). The Metal compiler removes the extra temporaries.
           'debug': True, 'reduced': False, 'depth_minus_one': census['depth_mode'] in (0, -1)}   # -1 = never set: NVN default NEAR_IS_MINUS_W
    for t in census['textures']:
        if t['stage'] == stage_no and t['texture']: job['tex'][str(t['index'])] = texture_type(t['texture']['target'])
    if stage == 'cs':
        d = census.get('dispatch')
        if not d: raise ValueError('compute stage without a dispatch census')
        job['local'] = d['local']; job['shared'] = d['shared']; del job['depth_minus_one']
        if not all(1 <= x <= 1024 for x in d['local']) or d['local'][0] * d['local'][1] * d['local'][2] > 1024: raise ValueError(f"threadgroup size {d['local']} beyond Metal's 1024 threads")
        if d['shared'] > 32768: raise ValueError(f"shared memory {d['shared']} bytes beyond Metal's 32 KiB threadgroup memory")
        for i in census.get('images', []):
            if i['stage'] == stage_no and i['texture']:
                job['img'][str(i['index'])] = {'type': texture_type(i['texture']['target']), 'fmt': image_format(i['texture']['format'])}
    elif stage == 'vs':
        for a in census['attribs']:
            if a['format']: job['attr'][str(a['location'])] = component_type(a['format'])
    else:
        for i, c in enumerate(census['targets']['colors']):
            if c: job['fout'][str(i)] = component_type(c['format'])
    return job

def reflect(census, stage_entry, job, res, msl, c1):
    buffers, textures, samplers, attributes = entry_bindings(msl)
    # Metal limits per stage: 31 textures, 16 samplers (round153: 12 offline stages bind samplers past 15).
    if any(t > 30 for t in textures): raise ValueError(f'texture binding {max(textures)} beyond Metal [[texture(30)]]')
    ov = re.search(r'^// wonder_sampler_overflow (\d+) ', msl, re.M)
    if not ov and any(s_ > 15 for s_ in samplers): raise ValueError(f'sampler binding {max(samplers)} beyond Metal\'s 16 samplers per stage')
    stage_no = stage_entry['stage']; lines = [f"stage {job['stage']}"]; used = set(); extents = cbuf_extents(msl)
    if ov: lines.append(f'smpov {ov.group(1)}'); used.add(int(ov.group(1)))   # samplers 16.. in the argument buffer (sampler_overflow)
    if job['stage'] == 'vs': lines.append('depth ' + ('minus_one' if job['depth_minus_one'] else 'zero'))
    if job['stage'] == 'cs': lines += ['local ' + ' '.join(map(str, job['local'])), f"shared {job['shared']}"]
    for cb in res['cbufs']:
        kind, r = cbuf_kind(cb['slot'])
        if cb['binding'] not in buffers: raise ValueError(f"cbuf c[{cb['slot']}] binding {cb['binding']} missing from the MSL entry")
        if kind == 'drv' and r == 0:
            check_driver_c0(msl, cb['binding'], job['stage']); lines.append(f"drv 0 {cb['binding']}")
        else: lines.append(f"cbuf {r} {cb['binding']}" if kind == 'cbuf' else f"c1 {cb['binding']} {c1.hex() or '-'}" if kind == 'c1' else f"drv {r}")
        if kind == 'cbuf': lines.append(f"cbufext {r} {extents.get(cb['binding'], 0)}")
        used.add(cb['binding'])
    if res['images'] and job['stage'] != 'cs': raise ValueError('images outside compute are not built yet')
    images_bound = {i['index'] for i in census.get('images', []) if i['stage'] == stage_no and i['texture']}
    for t in res['images']:
        r = image_slot(t['handle'], t['cbuf_slot'])
        if r not in images_bound: raise ValueError(f"image slot {r} is used but not bound to a registered texture in the census")
        if t['binding'] not in textures: raise ValueError(f"image binding {t['binding']} missing from the MSL entry")
        decl = re.search(rf'texture\w+<\w+(?:, access::(\w+))?> \w+ \[\[texture\({t["binding"]}\)\]\]', msl)
        if not decl: raise ValueError(f"image binding {t['binding']} has no texture declaration")
        access = {'read': 'r', 'write': 'w', 'read_write': 'rw'}.get(decl.group(1))
        if not access: raise ValueError(f"image binding {t['binding']} access {decl.group(1)}")
        declared = sampler_type_name(t['type'])
        if declared != job['img'][str(r)]['type']: raise ValueError(f"image slot {r}: shader declares {declared}, census bound {job['img'][str(r)]['type']}")
        lines.append(f"img {r} {t['binding']} {declared} {access}")
    for b in res['sbufs']:
        if b['cb_slot'] != 0: raise ValueError(f"storage buffer address in c[{b['cb_slot']}], not c[0]")
        if b['binding'] not in buffers: raise ValueError(f"storage binding {b['binding']} missing from the MSL entry")
        u = uniform_pointer_slot(b['cb_offset'], job['stage'])
        if u is not None:
            lines.append(f"uptr {u} {b['binding']}"); used.add(b['binding']); continue
        writes = re.search(rf'device \w+& \w+ \[\[buffer\({b["binding"]}\)\]\]', msl) and not re.search(rf'const device \w+& \w+ \[\[buffer\({b["binding"]}\)\]\]', msl)
        lines.append(f"sbuf {storage_slot(b['cb_offset'], job['stage'])} {b['binding']} {'rw' if writes else 'r'}"); used.add(b['binding'])
    bound = {t['index'] for t in census['textures'] if t['stage'] == stage_no and t['texture']}
    for t in res['textures']:
        if t['cbuf_slot'] not in (-1, 2):   # bindless: the handle is read at run time from a uniform buffer
            kind, r = cbuf_kind(t['cbuf_slot'] & 0xffff)
            if kind != 'cbuf': raise ValueError(f"bindless texture handle in c[{t['cbuf_slot']}]")
            if t['binding'] not in textures or t['binding'] not in samplers: raise ValueError(f"texture binding {t['binding']} missing from the MSL entry")
            lines.append(f"btex {r} {t['handle'] * 4} {t['binding']} {t['binding']} {sampler_type_name(t['type'])}")
            continue
        r = texture_slot(t['handle'], t['cbuf_slot'])
        if t['binding'] not in textures or t['binding'] not in samplers: raise ValueError(f"texture binding {t['binding']} missing from the MSL entry")
        if r not in bound:
            # Slot holding handle 0 when the census was taken (NVN's empty descriptor; the host binds an all-zero texture,
            # nvn_queue.c resolve_texture). The sampling instruction encodes the dimension; a type the translator had to
            # query (texture query, buffer fetch) would have left a note, rejected above. A texture the game binds there
            # later must match this type or the draw stops by name (round152: 5 stages left out for this).
            lines.append(f"tex {r} {t['binding']} {t['binding']} {sampler_type_name(t['type'])}")
            continue
        # The type the MSL declares (the translator's SamplerType), not the census target: the font shader samples the
        # 2D glyph cache with an array instruction (round127). Only that mismatch is accepted (Maxwell reads layer 0).
        declared, bound_type = sampler_type_name(t['type']), job['tex'][str(r)]
        if declared != bound_type and not (declared == '2darray' and bound_type == '2d'):
            raise ValueError(f"texture slot {r}: shader declares {declared}, census bound {bound_type}")
        lines.append(f"tex {r} {t['binding']} {t['binding']} {declared}")
    for b in sorted(support_bindings(msl) - used):
        lines.append(f'support {b}'); used.add(b)   # host fills it per D2R nvn_mtl.m (render_scale 1.0, BGRA flags 0)
    for b in sorted(buffers - used):
        raise ValueError(f'buffer({b}) in the MSL entry has no reflection (unknown binding)')
    # Buffer 15 (disabled-attribute defaults) and 16..30 (vertex streams) are vertex-stage bindings; other stages have 0..30.
    if job['stage'] == 'vs' and any(b >= 15 for b in buffers): raise ValueError('buffer index >= 15 collides with the default-attribute buffer (15) / vertex streams 16..30')
    if any(b > 30 for b in buffers): raise ValueError('buffer index beyond Metal [[buffer(30)]]')
    for a in attributes:
        # An attribute outside the census's vertex attribute state is disabled: Maxwell reads (0,0,0,1), which the host
        # supplies from the zero default buffer (round92); the translator answered its type as float.
        lines.append(f"attr {a} {a} {job['attr'].get(str(a), 'float')}")
    for color in range(8):
        if res['frag_out_map'] >> (4 * color) & 15:
            # An output without a bound target is discarded by NVN; it still needs a declared type (float when unbound).
            lines.append(f"fout {color} {job['fout'].get(str(color), 'float')}")
    return '\n'.join(lines) + '\n'

def stage_key(control, code):
    """Stage key (runtime/nvn_draw_census.h): first 16 hex digits of sha256(control[0:0x880] || code[0:length]), length =
    the u32 at control +0x704. Round191: 1395 control sections are shared by stages with different code."""
    length = int.from_bytes(control[0x704:0x708], 'little')
    if len(code) < length: raise ValueError(f'code is {len(code)} bytes, the control says {length}')
    return sha(control[:0x880] + code[:length])[:16]

def rekey(directory, census):
    """Censuses written before round191 name stages by the control-only key: recompute every stage key from its bytes."""
    for entry in census['program']['stages']:
        control = (directory / f"stage-{entry['index']}.control.bin").read_bytes()
        code = (directory / f"stage-{entry['index']}.code.bin").read_bytes()
        entry['key'] = stage_key(control, code)

def stage_job(directory, census, entry, tmp):
    """Census stage -> translator job (code written to tmp); ValueError when the stage cannot be built faithfully."""
    key = entry['key']
    control = (directory / f"stage-{entry['index']}.control.bin").read_bytes()
    code = (directory / f"stage-{entry['index']}.code.bin").read_bytes()
    if stage_key(control, code) != key: raise ValueError(f'{key}: control and code bytes do not hash to the stage key')
    # Graphics code = NVN header 0x30 (magic 78563412) + SPH 0x50 + instructions; the translator takes SPH + instructions.
    # Compute code = a 0x100-byte header (magic 79563412, the rest zero) + instructions (round136 cs-1fb9d970e6bbceb8:
    # instructions 0x4000 bytes from +0x100, c1 at +0x4100; the 58 compute controls in Lib/agl, Lib/gsys and Effect all
    # put c1 at align(0x100 + instruction bytes, 0x100)); the translator takes the instructions.
    if len(code) != entry['code_size']: raise ValueError(f'{key}: code size differs from the census')
    compute = entry['stage'] == 5
    if compute and (code[:4] != bytes.fromhex('79563412') or any(code[4:0x100])):
        raise ValueError(f'{key}: compute code does not start with the 0x100-byte NVN compute header (first bytes {code[:16].hex()})')
    if not compute and code[:4] != bytes.fromhex('78563412'): raise ValueError(f'{key}: graphics code lacks the NVN code header (first bytes {code[:16].hex()})')
    sph = tmp / f'{key}.sph'; sph.write_bytes(code[0x100:entry['c1_offset']] if compute else code[0x30:])
    job = job_for(census, entry, sph); job['c1'] = code[entry['c1_offset']:entry['code_size']].hex()
    return job

def build(census_dirs, out):
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, 'DOTNET_ROOT': str(ROOT / 'work/tools/dotnet')}
    jobs, sources, censuses, early_failed = [], {}, [], {}
    with tempfile.TemporaryDirectory(prefix='wonder-shaders-') as tmp:
        tmp = Path(tmp)
        for directory in map(Path, census_dirs):
            raw = (directory / 'draw.json').read_bytes(); census = json.loads(raw); censuses.append(census)
            try: rekey(directory, census)
            except (OSError, ValueError) as error: early_failed[f'census {directory}'] = {'stage': '-', 'reason': str(error)[:300], 'census_sha256': sha(raw)}; continue
            for entry in census['program']['stages']:
                key = entry['key']
                if key in sources or key in early_failed: continue
                try:
                    job = stage_job(directory, census, entry, tmp)
                except ValueError as error:   # recorded per key like translation failures; the rest of the build goes on
                    early_failed[key] = {'stage': STAGES.get(entry['stage'], str(entry['stage'])), 'reason': str(error)[:300], 'census_sha256': sha(raw)}
                    continue
                jobs.append(job); sources[key] = (census, entry, sha(raw))
        # Translator outputs are cached by job content (round197: a census round adds a handful of stages to ~9000): the
        # job without its temp code path, the code bytes and the translator source hash. Everything after the translator
        # runs on every stage as before, so a cached build equals a full one.
        translator_sha = subprocess.run([str(XLATE), '--source-sha'], check=True, capture_output=True, text=True, env=env).stdout.strip()
        cache = Path(os.environ.get('WONDER_XLATE_CACHE', ROOT / 'work/build/xlate-cache')); cache.mkdir(parents=True, exist_ok=True)
        def job_hash(j):
            return sha(json.dumps({k: v for k, v in j.items() if k != 'code'}, sort_keys=True).encode() + Path(j['code']).read_bytes() + translator_sha.encode())
        todo = []
        for j in jobs:
            h = cache / job_hash(j)
            if (h.with_suffix('.spv')).exists() and (h.with_suffix('.res.json')).exists():
                for suffix in ('.spv', '.res.json'): shutil.copyfile(h.with_suffix(suffix), tmp / f"{j['key']}{suffix}")
                if (h.with_suffix('.debug')).exists(): j['debug'] = True
            else: todo.append(j)
        if todo:
            (tmp / 'jobs.json').write_text(json.dumps(todo))
            subprocess.run([str(XLATE), str(tmp / 'jobs.json'), str(tmp)], env=env)   # failures are retried / reported per key
        retry = [dict(j, debug=True) for j in todo if not (tmp / f"{j['key']}.spv").exists()]
        if retry:   # DebugMode skips Ryujinx's AST optimizer (round77: d51f7f835e14a83b InvalidCastException in StructuredIr)
            (tmp / 'retry.json').write_text(json.dumps(retry))
            subprocess.run([str(XLATE), str(tmp / 'retry.json'), str(tmp)], env=env)
            for r in retry:
                for j in jobs:
                    if j['key'] == r['key']: j['debug'] = True
        for j in todo:
            if (tmp / f"{j['key']}.spv").exists() and (tmp / f"{j['key']}.res.json").exists():
                h = cache / job_hash(j)   # every job already runs in DebugMode (job_for), so a retry does not change its hash
                for suffix in ('.spv', '.res.json'): shutil.copyfile(tmp / f"{j['key']}{suffix}", h.with_suffix(suffix))
                if any(r['key'] == j['key'] for r in retry): h.with_suffix('.debug').write_text('retried in DebugMode\n')
        manifest = {'scope': 'Wonder draw-census shader build (MSL 3.2, discrete bindings)', 'spirv_cross_command': [str(CROSS), '<key>.spv', *CROSS_FLAGS],
                    'translator_source_sha': translator_sha,
                    'keys': {}}
        manifest['failed'] = dict(early_failed); built = {}
        for job in jobs:
            key = job['key']; census, entry, census_sha = sources[key]
            # A stage that cannot be built faithfully is left out (with its reason): its program then stops by name on device.
            try:
                if not (tmp / f'{key}.spv').exists(): raise ValueError('translator failed (also in DebugMode)')
                res = json.loads((tmp / f'{key}.res.json').read_text())
                notes = res['notes']
                # Offline stages (extract_shaders.py: no bindings) whose only notes are handle-typed texture reads the
                # translator answered as 2D are built (round208: 106 vertex stages): the host checks every draw's bound
                # target against the reflection and stops by name on a mismatch. Census stages keep the stop.
                if notes and census.get('offline') and all('无 PC 类型，按 2D' in n for n in notes):
                    manifest.setdefault('assumed_2d', {})[key] = notes; notes = []
                if notes: raise ValueError(f'translator notes {notes}')
                msl = guard_cbuf_indices(subprocess.run([str(CROSS), str(tmp / f'{key}.spv'), *CROSS_FLAGS], check=True, capture_output=True, text=True).stdout)
                msl, _ = sampler_overflow(msl)
                refl = reflect(census, entry, job, res, msl, bytes.fromhex(job['c1']))
            except (ValueError, subprocess.CalledProcessError) as error:
                manifest['failed'][key] = {'stage': job['stage'], 'reason': str(error)[:300], 'census_sha256': census_sha}
                continue
            built[key] = (job['stage'], msl, refl, census_sha)
        # Vertex stages declare the varyings their paired fragments read but they do not write (round78).
        reads = {}
        for census in censuses:
            keys = {s_['name']: s_['key'] for s_ in census['program']['stages']}
            if keys.get('vs') in built and keys.get('fs') in built:
                reads.setdefault(keys['vs'], {}).update(fragment_inputs(built[keys['fs']][1]))
            # Unpaired offline vertex stages (sharcb, round208) declare every input their archive's fragment stages read.
            for fs in census.get('pad_fs', []) if keys.get('vs') in built else []:
                if fs in built: reads.setdefault(keys['vs'], {}).update(fragment_inputs(built[fs][1]))
        for key, (stage, msl, refl, census_sha) in built.items():
            if stage == 'vs': msl = invariant_position(pad_vertex_outputs(msl, reads.get(key, {})))
            (out / f'{key}.metal').write_text(msl); (out / f'{key}.refl').write_text(refl)
            manifest['keys'][key] = {'stage': stage, 'msl_sha256': sha(msl.encode()), 'refl_sha256': sha(refl.encode()), 'census_sha256': census_sha}
    (out / 'jobs.json').write_text(json.dumps([{k: v for k, v in j.items() if k != 'code'} for j in jobs], indent=1) + '\n')
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n')
    return manifest

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('census', nargs='+', type=Path); p.add_argument('--out', type=Path, required=True)
    a = p.parse_args(); m = build(a.census, a.out); print(f"built {len(m['keys'])} stages into {a.out}; left out {len(m['failed'])}")
    for key, f in m['failed'].items(): print(f"  left out {key} ({f['stage']}): {f['reason']}")

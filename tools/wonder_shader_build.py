#!/usr/bin/env python3
"""Offline shader build from first-draw censuses (runtime/nvn_draw_census.h): Maxwell stage → SPIR-V (work/tools/wonder_xlate,
the Ryujinx translator front end) → MSL 2.1 with discrete bindings (spirv-cross) + a line-oriented reflection (.refl) the
host parses (runtime/shader_library.h). Translator queries are answered from the census, never defaulted:
texture targets from the bound texture descriptors, vertex attribute / fragment output component types from the bound
formats, depth remap from nvnDeviceSetDepthMode. Output: <key>.metal, <key>.refl, jobs.json, manifest.json.

Wonder SDK conventions (verified in sdk, not D2R's): uniform buffer r = c[3 + r] (BindUniformBuffer sdk+0x467950 writes
slot (r + 3) << 20); texture slot r handle = c[2][0x20 + 8r] (BindTexture sdk+0x468840 writes at r * 8 + 0x20).
.refl lines: stage vs|ps; depth minus_one|zero (vertex); cbuf <r> <buffer>; c1 <buffer> <hex>; support <buffer>; drv <bank>;
             tex <r> <texture> <sampler> <type>; attr <location> <nvn attribute> <type>; fout <color> <type>.
Usage: wonder_shader_build.py <census dir>... --out <dir>
"""
import argparse, hashlib, json, os, re, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
XLATE = ROOT / 'work/tools/wonder_xlate/wonder_xlate'
CROSS = ROOT / 'work/tools/spirv-cross/spirv-cross'
NVN_H = ROOT / 'vendor/d2r-mobile/dm/gfx/nvn/include/nvn.h'
CROSS_FLAGS = ['--msl', '--msl-ios', '--msl-version', '20100', '--msl-decoration-binding']
TEX_TABLE, TEX_SLOTS = 0x20, 32
TARGETS = {0: '1d', 1: '2d', 2: '3d', 3: '1darray', 4: '2darray', 5: '2dms', 6: '2dmsarray', 7: '2d', 8: 'cube', 9: 'cubearray', 10: 'buffer'}
STAGES = {0: 'vs', 1: 'ps'}   # census stage → translator stage; others stop until a draw needs them

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
def texture_slot(handle_word, cbuf_slot):
    """Translator texture handle (32-bit word offset into the bindless constant buffer) → NVN texture slot."""
    if cbuf_slot not in (-1, 2): raise ValueError(f'texture handle read from c[{cbuf_slot}], not c[2]')
    offset = handle_word * 4 - TEX_TABLE
    if offset < 0 or offset % 8 or offset // 8 >= TEX_SLOTS: raise ValueError(f'texture handle word {handle_word} is outside c[2][0x20..0x120)')
    return offset // 8
def cbuf_kind(slot):
    if 3 <= slot <= 16: return ('cbuf', slot - 3)
    if slot == 1: return ('c1', None)
    if slot in (0, 2): return ('drv', slot)
    raise ValueError(f'constant buffer c[{slot}] is driver-internal')

# The load is clamped into range before the zero select: the compiler may evaluate both sides of a ternary (round69 bisect:
# the fault followed the shader, not its buffers or textures).
CB_GUARD = 'static inline float4 wonder_cb(constant float4 *a, int i) { float4 v = a[min(uint(i), 4095u)]; return uint(i) < 4096u ? v : float4(0.0); }\n'
def guard_cbuf_indices(msl):
    """Dynamic constant-buffer indices (<name>._m0[<expr>], expr not a literal) become wonder_cb(<name>._m0, <expr>):
    out-of-range reads return 0 as on Maxwell instead of faulting the GPU (every c[n] is declared float4[4096])."""
    out, i, pattern = [], 0, re.compile(r'\b(\w+)\._m0\[')
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
    m = re.search(r'^(?:vertex|fragment) \w+ main0\((.*)\)$', msl, re.M)
    if not m or not structs: return set()
    return {int(b) for t, b in re.findall(r'constant (\w+)& \w+ \[\[buffer\((\d+)\)\]\]', m.group(1)) if t in structs}

VARYING = re.compile(r'^\s+(\w+) \w+ \[\[user\(locn(\d+)\)\]\];', re.M)
def fragment_inputs(msl):
    """user(locnN) inputs of a fragment stage: {location: MSL type}."""
    m = re.search(r'struct main0_in\n\{(.*?)\n\};', msl, re.S)
    return {int(loc): t for t, loc in VARYING.findall(m.group(1))} if m else {}
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

def entry_bindings(msl):
    """[[buffer(n)]] / [[texture(n)]] / [[sampler(n)]] parameters of the entry point, and stage_in attribute locations."""
    m = re.search(r'^(vertex|fragment) \w+ main0\((.*)\)$', msl, re.M)
    if not m: raise ValueError('no vertex/fragment main0 entry in MSL')
    params = m.group(2)
    return ({int(b) for b in re.findall(r'\[\[buffer\((\d+)\)\]\]', params)},
            {int(b) for b in re.findall(r'\[\[texture\((\d+)\)\]\]', params)},
            {int(b) for b in re.findall(r'\[\[sampler\((\d+)\)\]\]', params)},
            sorted(int(a) for a in re.findall(r'\[\[attribute\((\d+)\)\]\]', msl)))

def job_for(census, stage_entry, code_path):
    stage_no, stage = stage_entry['stage'], STAGES.get(stage_entry['stage'])
    if not stage: raise ValueError(f"stage {stage_entry['name']} is not built yet")
    job = {'key': stage_entry['key'], 'stage': stage, 'code': str(code_path), 'tex': {}, 'img': {}, 'attr': {}, 'fout': {},
           'debug': False, 'reduced': False, 'depth_minus_one': census['depth_mode'] in (0, -1)}   # -1 = never set: NVN default NEAR_IS_MINUS_W
    for t in census['textures']:
        if t['stage'] == stage_no and t['texture']: job['tex'][str(t['index'])] = texture_type(t['texture']['target'])
    if stage == 'vs':
        for a in census['attribs']:
            if a['format']: job['attr'][str(a['location'])] = component_type(a['format'])
    else:
        for i, c in enumerate(census['targets']['colors']):
            if c: job['fout'][str(i)] = component_type(c['format'])
    return job

def reflect(census, stage_entry, job, res, msl, c1):
    buffers, textures, samplers, attributes = entry_bindings(msl)
    stage_no = stage_entry['stage']; lines = [f"stage {job['stage']}"]; used = set()
    if job['stage'] == 'vs': lines.append('depth ' + ('minus_one' if job['depth_minus_one'] else 'zero'))
    for cb in res['cbufs']:
        kind, r = cbuf_kind(cb['slot'])
        if cb['binding'] not in buffers: raise ValueError(f"cbuf c[{cb['slot']}] binding {cb['binding']} missing from the MSL entry")
        lines.append(f"cbuf {r} {cb['binding']}" if kind == 'cbuf' else f"c1 {cb['binding']} {c1.hex() or '-'}" if kind == 'c1' else f"drv {r}")
        used.add(cb['binding'])
    if res['sbufs'] or res['images']: raise ValueError('storage buffers / images are not built yet')
    bound = {t['index'] for t in census['textures'] if t['stage'] == stage_no and t['texture']}
    for t in res['textures']:
        r = texture_slot(t['handle'], t['cbuf_slot'])
        if r not in bound: raise ValueError(f"texture slot {r} is read but not bound to a registered texture in the census")
        if t['binding'] not in textures or t['binding'] not in samplers: raise ValueError(f"texture binding {t['binding']} missing from the MSL entry")
        lines.append(f"tex {r} {t['binding']} {t['binding']} {job['tex'][str(r)]}")
    for b in sorted(support_bindings(msl) - used):
        lines.append(f'support {b}'); used.add(b)   # host fills it per D2R nvn_mtl.m (render_scale 1.0, BGRA flags 0)
    for b in sorted(buffers - used):
        raise ValueError(f'buffer({b}) in the MSL entry has no reflection (unknown binding)')
    if any(b >= 16 for b in buffers): raise ValueError('buffer index >= 16 collides with vertex streams 16..30')
    for a in attributes:
        if str(a) not in job['attr']: raise ValueError(f'attribute {a} is read but has no format in the census')
        lines.append(f"attr {a} {a} {job['attr'][str(a)]}")
    for color in range(8):
        if res['frag_out_map'] >> (4 * color) & 15:
            # An output without a bound target is discarded by NVN; it still needs a declared type (float when unbound).
            lines.append(f"fout {color} {job['fout'].get(str(color), 'float')}")
    return '\n'.join(lines) + '\n'

def build(census_dirs, out):
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, 'DOTNET_ROOT': str(ROOT / 'work/tools/dotnet')}
    jobs, sources, censuses = [], {}, []
    with tempfile.TemporaryDirectory(prefix='wonder-shaders-') as tmp:
        tmp = Path(tmp)
        for directory in map(Path, census_dirs):
            raw = (directory / 'draw.json').read_bytes(); census = json.loads(raw); censuses.append(census)
            for entry in census['program']['stages']:
                key = entry['key']
                if key in sources: continue
                control = (directory / f"stage-{entry['index']}.control.bin").read_bytes()
                code = (directory / f"stage-{entry['index']}.code.bin").read_bytes()
                if sha(control[:0x880])[:16] != key: raise ValueError(f'{key}: control bytes do not hash to the census key')
                if len(code) != entry['code_size'] or code[:4] != bytes.fromhex('78563412'): raise ValueError(f'{key}: code is not a full NVN graphics stage')
                sph = tmp / f'{key}.sph'; sph.write_bytes(code[0x30:])   # translator input: 0x50-byte SPH + instructions
                job = job_for(census, entry, sph); job['c1'] = code[entry['c1_offset']:entry['code_size']].hex()
                jobs.append(job); sources[key] = (census, entry, sha(raw))
        (tmp / 'jobs.json').write_text(json.dumps(jobs))
        subprocess.run([str(XLATE), str(tmp / 'jobs.json'), str(tmp)], env=env)   # failures are retried / reported per key
        retry = [dict(j, debug=True) for j in jobs if not (tmp / f"{j['key']}.spv").exists()]
        if retry:   # DebugMode skips Ryujinx's AST optimizer (round77: d51f7f835e14a83b InvalidCastException in StructuredIr)
            (tmp / 'retry.json').write_text(json.dumps(retry))
            subprocess.run([str(XLATE), str(tmp / 'retry.json'), str(tmp)], env=env)
            for r in retry:
                for j in jobs:
                    if j['key'] == r['key']: j['debug'] = True
        manifest = {'scope': 'Wonder draw-census shader build (MSL 2.1, discrete bindings)', 'spirv_cross_command': [str(CROSS), '<key>.spv', *CROSS_FLAGS],
                    'translator_source_sha': subprocess.run([str(XLATE), '--source-sha'], check=True, capture_output=True, text=True, env=env).stdout.strip(),
                    'keys': {}}
        manifest['failed'] = {}; built = {}
        for job in jobs:
            key = job['key']; census, entry, census_sha = sources[key]
            # A stage that cannot be built faithfully is left out (with its reason): its program then stops by name on device.
            try:
                if not (tmp / f'{key}.spv').exists(): raise ValueError('translator failed (also in DebugMode)')
                res = json.loads((tmp / f'{key}.res.json').read_text())
                if res['notes']: raise ValueError(f'translator notes {res["notes"]}')
                msl = guard_cbuf_indices(subprocess.run([str(CROSS), str(tmp / f'{key}.spv'), *CROSS_FLAGS], check=True, capture_output=True, text=True).stdout)
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
        for key, (stage, msl, refl, census_sha) in built.items():
            if stage == 'vs': msl = pad_vertex_outputs(msl, reads.get(key, {}))
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

"""Offline shader build from a draw census: keys, facts, reflection lines, MSL 3.2 flags.
Pure helpers always run; the end-to-end build needs work/ tools and a census (WONDER_CENSUS or round53)."""
from pathlib import Path
import hashlib, importlib.util, json, os, tempfile, unittest
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('wonder_shader_build', ROOT / 'tools/wonder_shader_build.py')
build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)
CENSUS = Path(os.environ.get('WONDER_CENSUS', ROOT / 'work/analysis/first-frame-round53/Documents/DrawCensus'))
TOOLS = (ROOT / 'work/tools/wonder_xlate/wonder_xlate').exists() and (ROOT / 'work/tools/spirv-cross/spirv-cross').exists()

class PureHelpers(unittest.TestCase):
    def test_format_component_types(self):
        self.assertEqual(build.component_type(41), 'float')   # RGBA16F (round53 vertex attribute)
        self.assertEqual(build.component_type(37), 'float')   # RGBA8 (render target)
        self.assertEqual(build.component_type(39), 'uint')    # RGBA8UI
        self.assertEqual(build.component_type(40), 'sint')    # RGBA8I
        with self.assertRaises(ValueError): build.component_type(0)
    def test_texture_slot_from_handle(self):
        self.assertEqual(build.texture_slot(8, -1), 0)        # word 8 = c[2] byte 0x20 = slot 0
        self.assertEqual(build.texture_slot(10, 2), 1)
        for bad in (7, 9, 0x48):                               # below the table, unaligned, past slot 31
            with self.assertRaises(ValueError): build.texture_slot(bad, -1)
        with self.assertRaises(ValueError): build.texture_slot(8, 3)   # handle from another constant buffer
    def test_constant_buffer_kinds(self):
        self.assertEqual(build.cbuf_kind(3), ('cbuf', 0))
        self.assertEqual(build.cbuf_kind(16), ('cbuf', 13))
        self.assertEqual(build.cbuf_kind(1), ('c1', None))
        self.assertEqual(build.cbuf_kind(0), ('drv', 0))
        self.assertEqual(build.cbuf_kind(2), ('drv', 2))
        with self.assertRaises(ValueError): build.cbuf_kind(17)
    def test_dynamic_constant_buffer_indices_are_guarded(self):
        # Maxwell returns 0 for constant reads outside the buffer; Metal would fault (round62-65, vs b767ff73ce766e58).
        src = ("using namespace metal;\n"
               "x = vp_c3._m0[33].z + vp_c3._m0[7u].x;\n"
               "_88 = vp_c3._m0[int(uint(int(_99)) >> uint(2))][int(_99) & 3];\n"
               "y = fp_c1._m0[i + f(a[2])];\n")
        out = build.guard_cbuf_indices(src)
        self.assertIn('vp_c3._m0[33].z + vp_c3._m0[7u].x', out)                                   # literal indices untouched
        self.assertIn('_88 = wonder_cb(vp_c3._m0, int(uint(int(_99)) >> uint(2)))[int(_99) & 3];', out)
        self.assertIn('y = wonder_cb(fp_c1._m0, i + f(a[2]));', out)                               # nested brackets kept whole
        self.assertIn('vp_s0._m0[int(k)]', build.guard_cbuf_indices('using namespace metal;\nz = vp_s0._m0[int(k)];\n'))   # storage buffers untouched
        # The load itself is clamped into range (the compiler may turn the ternary into load + select), then zeroed.
        self.assertIn('static inline __attribute__((unused)) float4 wonder_cb(constant float4 *a, int i) { float4 v = a[min(uint(i), 4095u)]; return uint(i) < 4096u ? v : float4(0.0); }', out)
        self.assertEqual(out.index('static inline __attribute__((unused)) float4 wonder_cb('), out.index('using namespace metal;\n') + len('using namespace metal;\n'))
    def test_constant_buffer_extents(self):
        # round222: bytes read per constant buffer binding; a computed index (wonder_cb) leaves it unknown (0).
        msl = ("fragment main0_out main0(constant _16& fp_c3 [[buffer(2)]], constant _16& fp_c1 [[buffer(3)]], constant _16& fp_c5 [[buffer(4)]])\n"
               "x = fp_c3._m0[3].z + fp_c3._m0[12u].x;\n"
               "y = wonder_cb(fp_c1._m0, i) + fp_c1._m0[2].x;\n")
        self.assertEqual(build.cbuf_extents(msl), {2: 13 * 16, 3: 0, 4: 16})
    def test_computed_uniform_reads_check_the_bound_size(self):
        # round234: wonder_cb on uniform buffers (c3..c16) in main0 -> wonder_cbz with the size table at a free buffer index.
        src = ("using namespace metal;\n"
               "fragment main0_out main0(constant _16& fp_c3 [[buffer(0)]], constant _16& fp_c1 [[buffer(2)]])\n"
               "{ x = fp_c3._m0[int(k)] + fp_c3._m0[4].x + fp_c1._m0[j]; }\n")
        out, b = build.size_guard(build.guard_cbuf_indices(src), 'ps')
        self.assertEqual(b, 1)
        self.assertIn('constant uint* wonder_cbsz [[buffer(1)]])', out)
        self.assertIn('wonder_cbz(fp_c3._m0, wonder_cbsz[0], int(k))', out)
        self.assertIn('wonder_cb(fp_c1._m0, j)', out)                       # shader constants (c1) keep the 64 KiB guard
        self.assertIn('static inline __attribute__((unused)) float4 wonder_cbz(', out)
        self.assertEqual(build.cbuf_extents(out), {0: 5 * 16, 2: 0})         # the literal read still bounds the extent
        same, none = build.size_guard('using namespace metal;\nvertex main0_out main0(constant _16& vp_c3 [[buffer(1)]])\n{ y = vp_c3._m0[2]; }\n', 'vs')
        self.assertIsNone(none)
        full = 'using namespace metal;\nvertex main0_out main0(' + ', '.join(f'constant _16& vp_c{3 + i} [[buffer({i})]]' for i in range(15)) + ')\n{ y = vp_c3._m0[int(k)]; }\n'
        self.assertIsNone(build.size_guard(build.guard_cbuf_indices(full), 'vs')[1])   # vertex stages: no free index below 15
    def test_support_buffer_parameters(self):
        # Ryujinx's support buffer is the struct carrying render_scale (float[73]); its entry parameter needs no reflection.
        msl = ("struct _10\n{\n    uint _m0;\n    uint _m1[8];\n    float4 _m2;\n    float4 _m3;\n    int _m4;\n    float _m5[73];\n    int4 _m6;\n    int _m7;\n};\n"
               "struct _16\n{\n    float4 _m0[4096];\n};\n"
               "fragment main0_out main0(constant _10& support_buffer [[buffer(0)]], constant _16& fp_c3 [[buffer(1)]])\n")
        self.assertEqual(build.support_bindings(msl), {0})
        self.assertEqual(build.support_bindings(msl.replace('constant _10& support_buffer [[buffer(0)]], ', '')), set())
        used = msl.replace('    float _m5[73];', '    float4 _m5[73];').replace('    uint _m1[8];', '    uint4 _m1[8];')   # as emitted when read (round77)
        self.assertEqual(build.support_bindings(used), {0})
    def test_unwritten_varyings_become_zero_outputs(self):
        # Round78: vs 4d338cef88ef2962 writes only its position; fs 84a6cd3c9abd22a8 reads user(locn0) -> Metal rejects the
        # pipeline. Maxwell leaves unwritten varyings undefined; the vertex shader declares them (zeroed by `out = {}`).
        vs = ("struct main0_out\n{\n    float4 m_9 [[user(locn2)]];\n    float4 gl_Position [[position]];\n};\n"
              "vertex main0_out main0()\n{\n    main0_out out = {};\n    return out;\n}\n")
        out = build.pad_vertex_outputs(vs, {9: 'int4'})
        self.assertIn('    float4 wonder_unwritten_locn0 [[user(locn0)]];', out)
        self.assertIn('    float4 wonder_unwritten_locn7 [[user(locn7)]];', out)
        self.assertNotIn('wonder_unwritten_locn2', out)                          # written by the shader itself
        self.assertIn('    int4 wonder_unwritten_locn9 [[user(locn9)]];', out)     # a paired fragment reads locn9 as int4
        self.assertIn('main0_out out = {};', out)
        self.assertEqual(build.fragment_inputs("struct main0_in\n{\n    float4 m_31 [[user(locn0)]];\n    uint4 m_3 [[user(locn4)]];\n};\n"), {0: 'float4', 4: 'uint4'})
    def test_ryujinx_sampler_types(self):
        # Ryujinx SamplerType: 1D 1, buffer 2, 2D 3, 3D 4, cube 5; Array 0x100, Multisample 0x200 (round86 bindless: 3).
        self.assertEqual(build.sampler_type_name(3), '2d'); self.assertEqual(build.sampler_type_name(5 | 0x100), 'cubearray')
        self.assertEqual(build.sampler_type_name(3 | 0x100), '2darray'); self.assertEqual(build.sampler_type_name(3 | 0x200), '2dms')
        with self.assertRaises(ValueError): build.sampler_type_name(9)
    def test_storage_buffer_slots(self):
        # Wonder SDK BindStorageBuffer (sdk+0x468030, table sdk+0xac558c): address of slot r at c[0][base + 16 r],
        # base vertex 0x110, fragment 0x510; the translator reports the word offset (round89: vs 88bfcbf96f575ff3 -> 68).
        self.assertEqual(build.storage_slot(68, 'vs'), 0); self.assertEqual(build.storage_slot(0x520 // 4, 'ps'), 1)
        with self.assertRaises(ValueError): build.storage_slot(70, 'vs')
        with self.assertRaises(ValueError): build.storage_slot(0x100 // 4, 'vs')
    def test_compute_slots(self):
        # Compute (round135): storage table at c[0][0x310 + 16 r]; uniform slot r read by address at c[0][0x210 + 16 r]
        # (D2R dmsh_build); image slot r at c[2][0x120 + 8 r] (BindImage sdk+0x46a280).
        self.assertEqual(build.storage_slot(0x320 // 4, 'cs'), 1)
        self.assertEqual(build.uniform_pointer_slot(0x230 // 4, 'cs'), 2); self.assertIsNone(build.uniform_pointer_slot(0x310 // 4, 'cs'))
        self.assertIsNone(build.uniform_pointer_slot(0x230 // 4, 'ps'))
        self.assertEqual(build.image_slot(0x120 // 4, 2), 0); self.assertEqual(build.image_slot(0x138 // 4, -1), 3)
        for bad in (0x118 // 4, 0x160 // 4):
            with self.assertRaises(ValueError): build.image_slot(bad, -1)
    def test_driver_c0_reads(self):
        # round136 cs-1fb9d970e6bbceb8 reads c[0] at 0x290 / 0x2e0 (uniform address table) and 0x310 / 0x350 (storage table).
        msl = 'kernel void main0(constant _16& cp_c0 [[buffer(19)]])\n x = cp_c0._m0[41].x + cp_c0._m0[49].y + cp_c0._m0[53].x;'
        build.check_driver_c0(msl, 19, 'cs')
        with self.assertRaises(ValueError): build.check_driver_c0(msl.replace('[41]', '[20]'), 19, 'cs')   # 0x140: not a table
        with self.assertRaises(ValueError): build.check_driver_c0(msl, 19, 'ps')                           # fragment table is 0x510
        with self.assertRaises(ValueError): build.check_driver_c0(msl + ' wonder_cb(cp_c0._m0, i)', 19, 'cs')
    def test_image_formats(self):
        self.assertEqual(build.image_format(0x25), 'R8G8B8A8Unorm'); self.assertEqual(build.image_format(0x29), 'R16G16B16A16Float')
        self.assertEqual(build.image_format(0xa), 'R32Float'); self.assertEqual(build.image_format(0x3f), 'R11G11B10Float')
        self.assertEqual(build.image_format(0x17), 'R32G32Uint'); self.assertEqual(build.image_format(0x2), 'R8Snorm')
        with self.assertRaises(ValueError): build.image_format(0x38)   # RGBA8_SRGB has no image format
    def test_texture_target_names(self):
        self.assertEqual(build.texture_type(1), '2d'); self.assertEqual(build.texture_type(8), 'cube'); self.assertEqual(build.texture_type(4), '2darray')

class StageKey(unittest.TestCase):
    def test_key_covers_code(self):
        """Round191: one control section with two code variants (as SecredUber's 1395 shared controls) gives two keys."""
        control = bytearray(0x880); control[0x704:0x708] = (8).to_bytes(4, 'little')
        a, b = build.stage_key(bytes(control), b'\x78\x56\x34\x12AAAA'), build.stage_key(bytes(control), b'\x78\x56\x34\x12BBBB')
        self.assertNotEqual(a, b)
        self.assertEqual(a, hashlib.sha256(bytes(control) + b'\x78\x56\x34\x12AAAA').hexdigest()[:16])
        self.assertEqual(build.stage_key(bytes(control), b'\x78\x56\x34\x12AAAAtail'), a)   # bytes past the length do not count
        with self.assertRaises(ValueError): build.stage_key(bytes(control), b'short')

@unittest.skipUnless(TOOLS and (CENSUS / 'draw.json').exists(), 'needs work/ translator, spirv-cross and a draw census')
class CensusBuild(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(); cls.out = Path(cls.tmp.name)
        build.build([CENSUS], cls.out)
        cls.census = json.loads((CENSUS / 'draw.json').read_text()); build.rekey(CENSUS, cls.census)   # round191 keys cover the code
        cls.manifest = json.loads((cls.out / 'manifest.json').read_text())
        cls.keys = {s['name']: s['key'] for s in cls.census['program']['stages']}
    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()
    def refl(self, stage): return (self.out / f"{self.keys[stage]}.refl").read_text().splitlines()
    def test_cached_translation_equals_full_build(self):
        # Round197: translator outputs are cached by job content; a second build from the cache writes the same files.
        with tempfile.TemporaryDirectory() as cache, tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            old = os.environ.get('WONDER_XLATE_CACHE'); os.environ['WONDER_XLATE_CACHE'] = cache
            try:
                build.build([CENSUS], a); self.assertTrue(any(Path(cache).glob('*.spv')))
                build.build([CENSUS], b)
            finally:
                if old is None: os.environ.pop('WONDER_XLATE_CACHE', None)
                else: os.environ['WONDER_XLATE_CACHE'] = old
            self.assertEqual(json.loads((Path(a) / 'manifest.json').read_text())['keys'], json.loads((Path(b) / 'manifest.json').read_text())['keys'])
    def test_keys_match_census(self):
        self.assertEqual(set(self.manifest['keys']), set(self.keys.values()))
        for name, key in self.keys.items():
            self.assertTrue((self.out / f'{key}.metal').exists())
            self.assertEqual(self.manifest['keys'][key]['stage'], {'vs': 'vs', 'fs': 'ps'}[name])
    def test_texture_slots_within_census(self):
        bound = {(t['stage'], t['index']) for t in self.census['textures'] if t['texture']}
        for name, stage in (('vs', 0), ('fs', 1)):
            for line in self.refl(name):
                if line.startswith('tex '): self.assertIn((stage, int(line.split()[1])), bound)
        self.assertIn('tex 0 1 1 2d', self.refl('fs'))
    def test_buffer_slots_disjoint_below_16(self):
        for name in self.keys:
            used = [int(l.split()[2]) for l in self.refl(name) if l.split()[0] in ('cbuf', 'c1')] + \
                   [int(l.split()[1]) for l in self.refl(name) if l.startswith('support ')]
            self.assertEqual(len(used), len(set(used))); self.assertTrue(all(0 <= b < 16 for b in used))
        self.assertIn('cbuf 0 1', self.refl('vs'))
    def test_vertex_attr_types_from_census(self):
        jobs = json.loads((self.out / 'jobs.json').read_text())
        vs = next(j for j in jobs if j['key'] == self.keys['vs'])
        self.assertEqual(vs['attr'], {'0': 'float'}); self.assertTrue(vs['depth_minus_one'])
        self.assertIn('attr 0 0 float', self.refl('vs')); self.assertIn('fout 0 float', self.refl('fs'))
    def test_depth_line_follows_census(self):
        self.assertIn('depth minus_one', self.refl('vs')); self.assertFalse(any(l.startswith('depth') for l in self.refl('fs')))
    def test_msl_21_flags(self):
        for flag in ('--msl-ios', '--msl-version', '30200', '--msl-decoration-binding'): self.assertIn(flag, self.manifest['spirv_cross_command'])
        # The NEAR_IS_MINUS_W remap z' = fma(z, 0.5, w * 0.5): assigned directly, or through a temporary in DebugMode.
        import re
        text = (self.out / f"{self.keys['vs']}.metal").read_text()
        direct = 'gl_Position.z = fma(' in text
        via = re.search(r'float (_\d+) = fma\(\w+, 0\.5, \w+\);\s*out\.gl_Position\.z = \1;', text)
        self.assertTrue(direct or via, 'vertex depth remap missing')
    def test_manifest_hashes(self):
        import hashlib
        for key, entry in self.manifest['keys'].items():
            self.assertEqual(entry['msl_sha256'], hashlib.sha256((self.out / f'{key}.metal').read_bytes()).hexdigest())
            self.assertEqual(entry['refl_sha256'], hashlib.sha256((self.out / f'{key}.refl').read_bytes()).hexdigest())

if __name__ == '__main__': unittest.main()

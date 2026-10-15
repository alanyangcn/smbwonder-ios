"""Offline shader build from a draw census: keys, facts, reflection lines, MSL 2.1 flags.
Pure helpers always run; the end-to-end build needs work/ tools and a census (WONDER_CENSUS or round53)."""
from pathlib import Path
import importlib.util, json, os, tempfile, unittest
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
        # The load itself is clamped into range (the compiler may turn the ternary into load + select), then zeroed.
        self.assertIn('static inline __attribute__((unused)) float4 wonder_cb(constant float4 *a, int i) { float4 v = a[min(uint(i), 4095u)]; return uint(i) < 4096u ? v : float4(0.0); }', out)
        self.assertEqual(out.index('static inline __attribute__((unused)) float4 wonder_cb('), out.index('using namespace metal;\n') + len('using namespace metal;\n'))
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
    def test_texture_target_names(self):
        self.assertEqual(build.texture_type(1), '2d'); self.assertEqual(build.texture_type(8), 'cube'); self.assertEqual(build.texture_type(4), '2darray')

@unittest.skipUnless(TOOLS and (CENSUS / 'draw.json').exists(), 'needs work/ translator, spirv-cross and a draw census')
class CensusBuild(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(); cls.out = Path(cls.tmp.name)
        build.build([CENSUS], cls.out)
        cls.census = json.loads((CENSUS / 'draw.json').read_text())
        cls.manifest = json.loads((cls.out / 'manifest.json').read_text())
        cls.keys = {s['name']: s['key'] for s in cls.census['program']['stages']}
    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()
    def refl(self, stage): return (self.out / f"{self.keys[stage]}.refl").read_text().splitlines()
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
        for flag in ('--msl-ios', '--msl-version', '20100', '--msl-decoration-binding'): self.assertIn(flag, self.manifest['spirv_cross_command'])
        self.assertIn('gl_Position.z = fma(', (self.out / f"{self.keys['vs']}.metal").read_text())
    def test_manifest_hashes(self):
        import hashlib
        for key, entry in self.manifest['keys'].items():
            self.assertEqual(entry['msl_sha256'], hashlib.sha256((self.out / f'{key}.metal').read_bytes()).hexdigest())
            self.assertEqual(entry['refl_sha256'], hashlib.sha256((self.out / f'{key}.refl').read_bytes()).hexdigest())

if __name__ == '__main__': unittest.main()

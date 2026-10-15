from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class Metal2GpuTests(unittest.TestCase):
    def test_actual_gpu_texture_binding_and_readback_when_host_has_metal(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'gpu'
            subprocess.run(['clang','-fobjc-arc','-Wall','-Wextra',str(ROOT/'tests/metal2_probe_test.m'),str(ROOT/'runtime/metal2_probe.m'),str(ROOT/'runtime/metal2_pixels.c'),'-framework','Foundation','-framework','Metal','-framework','CoreGraphics','-framework','ImageIO','-o',str(exe)],check=True)
            p=subprocess.run([str(exe),directory])
            if p.returncode==77:self.skipTest('Host has no Metal device; physical iPhone test required')
            self.assertEqual(p.returncode,0)

class Metal2ShaderLibraryTests(unittest.TestCase):
    def test_shader_library_compiles_verified_msl_and_stops_by_name(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'lib'; scratch=Path(directory)/'shaders'; scratch.mkdir()
            subprocess.run(['clang','-fobjc-arc','-Wall','-Wextra','-I'+str(ROOT/'runtime'),'-I'+str(ROOT/'vendor/d2r-mobile/dm/gfx/nvn'),'-I'+str(ROOT/'vendor/d2r-mobile/dm/gfx/tex'),str(ROOT/'tests/metal2_draw_test.m'),str(ROOT/'runtime/metal2_draw.m'),str(ROOT/'runtime/shader_library.c'),str(ROOT/'vendor/d2r-mobile/dm/gfx/tex/nvn_fmt_mtl.c'),str(ROOT/'vendor/d2r-mobile/dm/gfx/tex/nvn_bcdec.c'),str(ROOT/'vendor/d2r-mobile/dm/gfx/tex/nvn_deswizzle.c'),str(ROOT/'vendor/d2r-mobile/dm/gfx/nvn/nvn_tex.c'),'-framework','Foundation','-framework','Metal','-framework','CoreGraphics','-framework','ImageIO','-o',str(exe)],check=True)
            built=ROOT/'work/build/wonder-shaders'
            p=subprocess.run([str(exe),str(scratch),'ok']+([str(built)] if (built/'manifest.json').exists() else []),capture_output=True,text=True)
            if p.returncode==77:self.skipTest('Host has no Metal device')
            self.assertEqual(p.returncode,0,p.stderr[-2000:])
            for case,text in (('missing','NVN_SHADER_MISSING key=0000000000000000'),('broken','NVN_SHADER_COMPILE_FAILED key=bbbbbbbbbbbbbbbb'),
                              ('tampered','NVN_SHADER_MANIFEST key=cccccccccccccccc'),('wrong_stage','NVN_SHADER_STAGE key=aaaaaaaaaaaaaaaa')):
                q=subprocess.run([str(exe),str(scratch),case],capture_output=True,text=True)
                self.assertEqual(q.returncode,3,(case,q.stderr[-500:])); self.assertIn(text,q.stderr)
                if case=='broken': self.assertIn('error:',q.stderr)   # the compiler's own error line, not just its headline
            self.assertIn('metal2_draw shader library OK',p.stdout)

class Metal2EncodeTests(unittest.TestCase):
    def run_case(self,case):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'encode'
            subprocess.run(['clang','-fobjc-arc','-Wall','-Wextra','-I'+str(ROOT/'runtime'),'-I'+str(ROOT/'vendor/d2r-mobile/dm/gfx/nvn'),'-I'+str(ROOT/'vendor/d2r-mobile/dm/gfx/tex'),
                str(ROOT/'tests/metal2_encode_test.m'),str(ROOT/'runtime/metal2_draw.m'),str(ROOT/'runtime/shader_library.c'),
                str(ROOT/'vendor/d2r-mobile/dm/gfx/tex/nvn_fmt_mtl.c'),str(ROOT/'vendor/d2r-mobile/dm/gfx/tex/nvn_bcdec.c'),str(ROOT/'vendor/d2r-mobile/dm/gfx/tex/nvn_deswizzle.c'),
                str(ROOT/'vendor/d2r-mobile/dm/gfx/nvn/nvn_tex.c'),'-framework','Foundation','-framework','Metal','-framework','CoreGraphics','-framework','ImageIO','-o',str(exe)],check=True)
            p=subprocess.run([str(exe),case,directory],capture_output=True,text=True)
            if p.returncode==77:self.skipTest('Host has no Metal device')
            return p
    def test_draw_textured_triangle_with_depth(self):
        p=self.run_case('draw'); self.assertEqual(p.returncode,0,p.stdout+p.stderr[-2000:]); self.assertIn('metal2 encode draw OK',p.stdout)
    def test_back_facing_triangle_is_culled(self):
        p=self.run_case('back_facing'); self.assertEqual(p.returncode,0,p.stdout+p.stderr[-2000:]); self.assertIn('back face culled OK',p.stdout)
    def test_bound_zero_channel_mask_gets_its_own_pipeline(self):
        p=self.run_case('mask'); self.assertEqual(p.returncode,0,p.stdout+p.stderr[-2000:]); self.assertIn('channel mask OK',p.stdout)
    def test_full_texture_view_with_swizzle(self):
        p=self.run_case('view'); self.assertEqual(p.returncode,0,p.stdout+p.stderr[-2000:]); self.assertIn('texture view OK',p.stdout)
    def test_texture_view_reinterpreting_the_format_stops(self):
        p=self.run_case('bad_view'); self.assertEqual(p.returncode,3,p.stdout+p.stderr[-2000:]); self.assertIn('texture view format',p.stderr)
    def test_bc_texture_decoded_when_the_gpu_lacks_bc(self):
        p=self.run_case('bc'); self.assertEqual(p.returncode,0,p.stdout+p.stderr[-2000:]); self.assertIn('BC decode OK',p.stdout)
        self.assertIn('HOST_TEXTURE_BC_DECODE serial=79',p.stderr)
    def test_constant_blend_colour(self):
        p=self.run_case('blend_color'); self.assertEqual(p.returncode,0,p.stdout+p.stderr[-2000:]); self.assertIn('blend colour OK',p.stdout)
    def test_vertex_format_without_metal_equivalent_stops(self):
        p=self.run_case('bad_vertex_format'); self.assertEqual(p.returncode,3,p.stdout+p.stderr[-2000:]); self.assertIn('vertex format',p.stderr)

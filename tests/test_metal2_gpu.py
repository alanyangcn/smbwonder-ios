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
            subprocess.run(['clang','-fobjc-arc','-Wall','-Wextra','-I'+str(ROOT/'runtime'),str(ROOT/'tests/metal2_draw_test.m'),str(ROOT/'runtime/metal2_draw.m'),str(ROOT/'runtime/shader_library.c'),'-framework','Foundation','-framework','Metal','-o',str(exe)],check=True)
            built=ROOT/'work/build/wonder-shaders'
            p=subprocess.run([str(exe),str(scratch),'ok']+([str(built)] if (built/'manifest.json').exists() else []),capture_output=True,text=True)
            if p.returncode==77:self.skipTest('Host has no Metal device')
            self.assertEqual(p.returncode,0,p.stderr[-2000:])
            for case,text in (('missing','NVN_SHADER_MISSING key=0000000000000000'),('broken','NVN_SHADER_COMPILE_FAILED key=bbbbbbbbbbbbbbbb'),
                              ('tampered','NVN_SHADER_MANIFEST key=cccccccccccccccc'),('wrong_stage','NVN_SHADER_STAGE key=aaaaaaaaaaaaaaaa')):
                q=subprocess.run([str(exe),str(scratch),case],capture_output=True,text=True)
                self.assertEqual(q.returncode,3,(case,q.stderr[-500:])); self.assertIn(text,q.stderr)
            self.assertIn('metal2_draw shader library OK',p.stdout)

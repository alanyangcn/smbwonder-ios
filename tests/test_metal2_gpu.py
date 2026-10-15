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

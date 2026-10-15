from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class Metal2SourceTests(unittest.TestCase):
    def test_actual_compilation_and_corrupted_input_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'sources'
            subprocess.run(['clang','-fobjc-arc','-Wall','-Wextra','-Wno-deprecated-declarations',str(ROOT/'tests/metal2_shader_check_test.m'),str(ROOT/'runtime/metal2_shader_check.m'),'-framework','Foundation','-framework','Metal','-o',str(exe)],check=True)
            p=subprocess.run([str(exe),directory])
            if p.returncode==77:self.skipTest('Host has no Metal device')
            self.assertEqual(p.returncode,0)

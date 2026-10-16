from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class GpuGateTests(unittest.TestCase):
    def test_gate_blocks_until_active(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'test'
            subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(ROOT/'tests/gpu_gate_test.c'),str(ROOT/'runtime/gpu_gate.c'),'-o',str(exe)],check=True)
            out=subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=30).stdout
            self.assertIn('gpu_gate OK',out)
if __name__=='__main__':unittest.main()

from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class RtDeclareTests(unittest.TestCase):
    def test_computation_follows_the_p90_of_per_frame_cpu(self):
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/'rt_declare'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(ROOT/'tests/rt_declare_test.c'),str(ROOT/'runtime/rt_declare.c'),'-o',str(exe)],check=True)
            self.assertIn('RT_DECLARE_OK',subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout)
if __name__=='__main__':unittest.main()

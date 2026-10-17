from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class FpsMeterTests(unittest.TestCase):
    def test_windows_rates_and_gaps(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'test'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(ROOT/'tests/fps_meter_test.c'),str(ROOT/'runtime/fps_meter.c'),'-o',str(exe)],check=True)
            self.assertIn('fps_meter OK',subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout)
if __name__=='__main__':unittest.main()

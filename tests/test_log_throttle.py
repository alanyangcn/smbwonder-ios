from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class LogThrottleTests(unittest.TestCase):
    def test_first_lines_in_full_then_every_thousandth_per_key(self):
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/'throttle'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'runtime'),str(ROOT/'tests/log_throttle_test.c'),str(ROOT/'runtime/log_throttle.c'),'-o',str(exe)],check=True)
            self.assertIn('LOG_THROTTLE_OK',subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout)
if __name__=='__main__':unittest.main()

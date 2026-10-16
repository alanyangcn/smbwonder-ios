from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class InputScriptTests(unittest.TestCase):
    def test_timed_and_periodic_presses(self):
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/'script'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(ROOT/'tests/input_script_test.c'),str(ROOT/'runtime/input_script.c'),'-o',str(exe)],check=True)
            self.assertIn('INPUT_SCRIPT_OK',subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout)
if __name__=='__main__':unittest.main()

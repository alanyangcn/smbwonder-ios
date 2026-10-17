from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class WidescreenTests(unittest.TestCase):
    def test_aspect_stretch_and_patch(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'test'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(ROOT/'tests/widescreen_test.c'),str(ROOT/'runtime/widescreen.c'),'-o',str(exe)],check=True)
            self.assertIn('widescreen OK',subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout)
if __name__=='__main__':unittest.main()

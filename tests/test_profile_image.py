from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class ProfileImageTests(unittest.TestCase):
    def test_offline_profile_image(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'test'
            subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(ROOT/'tests/profile_image_test.c'),str(ROOT/'runtime/profile_image.c'),
                            '-framework','CoreGraphics','-framework','ImageIO','-framework','CoreFoundation','-o',str(exe)],check=True)
            out=subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout
            self.assertIn('profile_image OK',out)
if __name__=='__main__':unittest.main()

from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class TextureStorageTests(unittest.TestCase):
    def test_textures_identified_by_storage(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'test'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),'-I'+str(ROOT/'vendor/d2r-mobile/dm/gfx/nvn'),
                            str(ROOT/'tests/texture_storage_test.c'),str(ROOT/'runtime/texture_storage.c'),'-o',str(exe)],check=True)
            out=subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout
            self.assertIn('texture_storage OK',out)
if __name__=='__main__':unittest.main()

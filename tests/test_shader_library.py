from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class ShaderLibraryTests(unittest.TestCase):
    def test_offline_shader_library(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'test'
            subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(ROOT/'tests/shader_library_test.c'),str(ROOT/'runtime/shader_library.c'),'-o',str(exe)],check=True)
            out=subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout
            self.assertIn('shader_library OK',out)
if __name__=='__main__':unittest.main()

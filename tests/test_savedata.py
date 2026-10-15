from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class SaveDataTests(unittest.TestCase):
    def test_journaled_save_data(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'test'
            subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(ROOT/'tests/savedata_test.c'),str(ROOT/'runtime/savedata.c'),'-o',str(exe)],check=True)
            root=Path(directory)/'root';root.mkdir()
            out=subprocess.run([str(exe),str(root)],check=True,capture_output=True,text=True).stdout
            self.assertIn('savedata OK',out)
if __name__=='__main__':unittest.main()

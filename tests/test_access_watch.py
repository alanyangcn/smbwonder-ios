from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class AccessWatchTests(unittest.TestCase):
    def test_first_access_is_reported_once(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'test'
            subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(ROOT/'tests/access_watch_test.c'),str(ROOT/'runtime/access_watch.c'),'-o',str(exe)],check=True)
            out=subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=30).stdout
            self.assertIn('access_watch OK',out)
if __name__=='__main__':unittest.main()

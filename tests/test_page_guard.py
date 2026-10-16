from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class PageGuardTests(unittest.TestCase):
    def test_write_watch_dirty_rearm_release_forget(self):
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/'guard'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(ROOT/'tests/page_guard_test.c'),str(ROOT/'runtime/page_guard.c'),'-o',str(exe)],check=True)
            self.assertIn('PAGE_GUARD_OK',subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout)
if __name__=='__main__':unittest.main()

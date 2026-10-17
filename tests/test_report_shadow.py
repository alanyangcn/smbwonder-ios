from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class ReportShadowTests(unittest.TestCase):
    def test_pending_report_values(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'test'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(ROOT/'tests/report_shadow_test.c'),str(ROOT/'runtime/report_shadow.c'),'-o',str(exe)],check=True)
            self.assertIn('report_shadow OK',subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout)
if __name__=='__main__':unittest.main()

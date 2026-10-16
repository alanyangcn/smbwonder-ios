from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class TbdrPlanTests(unittest.TestCase):
    def test_clear_folding_and_dontcare_stores_only_when_proven(self):
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/'tbdr_plan'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(ROOT/'tests/tbdr_plan_test.c'),str(ROOT/'runtime/tbdr_plan.c'),'-o',str(exe)],check=True)
            self.assertIn('TBDR_PLAN_OK',subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout)
if __name__=='__main__':unittest.main()

from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class AccountStateTests(unittest.TestCase):
    def test_single_offline_user_persists_and_validates_handles(self):
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/'account'; root=Path(d)/'sdk'; root.mkdir()
            subprocess.run(['clang','-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-pthread','-I'+str(ROOT/'runtime'),str(ROOT/'tests/account_state_test.c'),str(ROOT/'runtime/account_state.c'),'-o',str(exe)],check=True)
            first=subprocess.run([str(exe),str(root)],check=True,capture_output=True,text=True).stdout
            second=subprocess.run([str(exe),str(root)],check=True,capture_output=True,text=True).stdout
            self.assertIn('ACCOUNT_STATE_OK',first)
            uid=lambda s:[l for l in s.splitlines() if l.startswith('ACCOUNT_UID')][0]
            self.assertEqual(uid(first),uid(second))   # stable across launches
if __name__=='__main__':unittest.main()

from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
def build(d):
    exe=Path(d)/'account'
    subprocess.run(['clang','-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'runtime'),str(ROOT/'tests/account_state_test.c'),str(ROOT/'runtime/account_state.c'),'-o',str(exe)],check=True)
    return exe
users=lambda s:[l for l in s.splitlines() if l.startswith('USER ')]
class AccountStateTests(unittest.TestCase):
    def test_eight_default_users_management_and_persistence(self):
        with tempfile.TemporaryDirectory() as d:
            exe=build(d); root=Path(d)/'sdk'; root.mkdir()
            first=subprocess.run([str(exe),str(root),'fresh'],check=True,capture_output=True,text=True).stdout
            second=subprocess.run([str(exe),str(root),'check'],check=True,capture_output=True,text=True).stdout
            self.assertIn('ACCOUNT_STATE_OK',first); self.assertIn('ACCOUNT_CHECK_OK',second)
            self.assertEqual(len(users(first)),2)
            self.assertEqual(users(first),users(second))   # stable across launches
            self.assertTrue(users(first)[0].endswith(' 马力欧 Mario'))
    def test_existing_single_user_is_kept_first(self):
        with tempfile.TemporaryDirectory() as d:
            exe=build(d); root=Path(d)/'sdk'; root.mkdir()
            out=subprocess.run([str(exe),str(root),'legacy'],check=True,capture_output=True,text=True).stdout
            self.assertIn('ACCOUNT_LEGACY_OK',out)
            self.assertEqual(users(out)[0],'USER 00000000000022220000000000001111 玩家3')
            again=subprocess.run([str(exe),str(root),'check'],check=True,capture_output=True,text=True).stdout
            self.assertEqual(users(out),users(again))
if __name__=='__main__':unittest.main()

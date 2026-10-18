from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class UserAdminTests(unittest.TestCase):
    def test_deletions_are_queued_then_applied_at_next_launch(self):
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/'admin'; root=Path(d)/'sdk'; root.mkdir()
            subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'runtime'),str(ROOT/'tests/user_admin_test.c'),
                            str(ROOT/'runtime/user_admin.c'),str(ROOT/'runtime/account_state.c'),str(ROOT/'runtime/savedata.c'),'-o',str(exe)],check=True)
            first=subprocess.run([str(exe),str(root),'queue'],check=True,capture_output=True,text=True)
            self.assertIn('USER_ADMIN_QUEUE_OK',first.stdout)
            second=subprocess.run([str(exe),str(root),'apply'],check=True,capture_output=True,text=True)
            self.assertIn('USER_ADMIN_APPLY_OK',second.stdout)
            self.assertEqual(second.stderr.count('HOST_USERS deleted user'),2)
if __name__=='__main__':unittest.main()

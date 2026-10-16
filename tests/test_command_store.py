from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class CommandStoreTests(unittest.TestCase):
    def test_records_by_stub_address_replace_and_forget(self):
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/'store'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'runtime'),str(ROOT/'tests/command_store_test.c'),str(ROOT/'runtime/command_store.c'),'-o',str(exe)],check=True)
            self.assertIn('COMMAND_STORE_OK',subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout)
if __name__=='__main__':unittest.main()

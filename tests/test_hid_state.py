from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class HidStateTests(unittest.TestCase):
    def test_handheld_npad_connection_history_and_publish(self):
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/'hid'
            subprocess.run(['clang','-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-pthread','-I'+str(ROOT/'runtime'),str(ROOT/'tests/hid_state_test.c'),str(ROOT/'runtime/hid_state.c'),'-o',str(exe)],check=True)
            self.assertIn('HID_STATE_OK',subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout)
if __name__=='__main__':unittest.main()

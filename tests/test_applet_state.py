from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class AppletStateTests(unittest.TestCase):
    def test_notifications_follow_host_state_and_enable_flags(self):
        source=ROOT/'runtime/applet_state.c'
        self.assertTrue(source.exists(),'Applet notification state missing')
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'applet'
            subprocess.run(['clang','-Wall','-Wextra',str(ROOT/'tests/applet_state_test.c'),str(source),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

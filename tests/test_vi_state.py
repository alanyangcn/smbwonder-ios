from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class ViStateTests(unittest.TestCase):
 def test_valid_display_layer_and_native_window_only(self):
  with tempfile.TemporaryDirectory() as d:
   exe=Path(d)/'vi';subprocess.run(['clang','-Wall','-Wextra','-I'+str(ROOT/'runtime'),str(ROOT/'tests/vi_state_test.c'),str(ROOT/'runtime/vi_state.c'),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)

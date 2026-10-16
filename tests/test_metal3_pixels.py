from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class Metal3PixelTests(unittest.TestCase):
    def test_readback_oracle_rejects_corruption_and_clear_only_output(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'pixels'
            subprocess.run(['clang','-Wall','-Wextra','-I'+str(ROOT/'runtime'),str(ROOT/'tests/metal3_pixels_test.c'),str(ROOT/'runtime/metal3_pixels.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

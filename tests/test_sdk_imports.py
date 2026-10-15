from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class StrictImportsTests(unittest.TestCase):
    def test_weak_symbols_and_strong_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'imports'
            subprocess.run(['clang','-DSMBW_STRICT_IMPORTS=1','-I'+str(ROOT/'vendor/d2r-mobile/dm/host'),
                str(ROOT/'tests/sdk_imports_test.c'),str(ROOT/'vendor/d2r-mobile/dm/host/hle.c'),
                '-Wl,-dead_strip','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

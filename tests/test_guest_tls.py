from pathlib import Path
import subprocess, tempfile, unittest
ROOT = Path(__file__).resolve().parents[1]
class GuestTlsTests(unittest.TestCase):
    def test_frees_original_allocation_after_sdk_rewrites_keys(self):
        with tempfile.TemporaryDirectory() as d:
            exe = Path(d) / 'tls'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-pthread',
                '-I'+str(ROOT/'runtime'), str(ROOT/'tests/guest_tls_test.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=10)

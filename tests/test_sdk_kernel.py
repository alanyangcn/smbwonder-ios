"""Compile and execute the actual portable startup kernel on the Intel host."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
class SdkKernelTests(unittest.TestCase):
    def test_real_startup_kernel(self):
        source = ROOT / 'runtime/sdk_kernel.c'
        self.assertTrue(source.exists(), 'Target SDK startup kernel missing')
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'kernel-test'
            subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-I'+str(ROOT/'vendor/d2r-mobile/dm/host'),
                str(ROOT/'tests/sdk_kernel_test.c'),str(source),'-o',str(binary)],check=True)
            subprocess.run([str(binary)], check=True)

    def test_concurrent_host_page_reset(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'kernel-race-test'
            subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-pthread','-I'+str(ROOT/'vendor/d2r-mobile/dm/host'),
                str(ROOT/'tests/sdk_kernel_race_test.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=15,capture_output=True,text=True)

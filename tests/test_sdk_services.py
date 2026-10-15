from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class SdkServiceTests(unittest.TestCase):
    def test_real_service_contexts_and_persistent_log(self):
        source=ROOT/'runtime/sdk_services.c'
        self.assertTrue(source.exists(),'Host service contexts are missing')
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'services'
            subprocess.run(['clang','-Wall','-Wextra',str(ROOT/'tests/sdk_services_test.c'),str(source),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

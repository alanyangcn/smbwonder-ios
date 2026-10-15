from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class ConstructorTests(unittest.TestCase):
    def test_dt_init_order_and_bounds(self):
        source=ROOT/'runtime/module_inits.c'
        self.assertTrue(source.exists(),'Bounded DT_INIT runner missing')
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'constructors'
            subprocess.run(['clang','-Wall','-Wextra','-I'+str(ROOT/'vendor/d2r-mobile/dm/host'),str(ROOT/'tests/module_inits_test.c'),str(source),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

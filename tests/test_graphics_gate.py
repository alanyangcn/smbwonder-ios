from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class GraphicsBoundaryTests(unittest.TestCase):
    def test_driver_boundary_stops_without_faking_renderer_success(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'graphics-gate'
            subprocess.run(['clang','-Wall','-Wextra','-I'+str(ROOT/'vendor/d2r-mobile/dm/host'),str(ROOT/'tests/graphics_gate_test.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

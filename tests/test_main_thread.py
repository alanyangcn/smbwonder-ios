from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class MainThreadTests(unittest.TestCase):
    def test_actual_dedicated_thread_has_npdm_stack_and_is_singleton(self):
        source=ROOT/'runtime/game_thread.c'
        self.assertTrue(source.exists(),'Dedicated game thread not implemented')
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'thread'
            subprocess.run(['clang','-Wall','-Wextra',str(ROOT/'tests/game_thread_test.c'),str(source),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_missing_service_stops_guest_without_resuming_it(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'stop'
            subprocess.run(['clang','-Wall','-Wextra',str(ROOT/'tests/game_thread_stop_test.c'),str(ROOT/'runtime/game_thread.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class KernelThreadTests(unittest.TestCase):
    def test_threads_events_locks_condvars_and_address_arbiter(self):
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/'threads'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-pthread','-I'+str(ROOT/'runtime'),str(ROOT/'tests/kernel_threads_test.c'),str(ROOT/'runtime/kernel_threads.c'),'-o',str(exe)],check=True)
            out=subprocess.run([str(exe)],check=True,timeout=30,capture_output=True,text=True).stdout
            self.assertIn('KERNEL_THREADS_SYNC_OK',out); self.assertIn('HOST_THREAD_CREATE handle=',out)
    def test_exit_restores_host_stack(self):
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/'stack'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Wno-deprecated-declarations','-pthread','-I'+str(ROOT/'runtime'),str(ROOT/'tests/kernel_thread_stack_test.c'),str(ROOT/'runtime/kernel_threads.c'),'-o',str(exe)],check=True)
            out=subprocess.run([str(exe)],check=True,timeout=15,capture_output=True,text=True).stdout
            self.assertIn('KERNEL_THREAD_EXIT_HOST_STACK_OK',out)
if __name__=='__main__':unittest.main()

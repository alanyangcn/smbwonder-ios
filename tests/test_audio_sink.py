from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class AudioSinkTests(unittest.TestCase):
    def test_device_sink_extraction_and_ring(self):
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/'sink'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(ROOT/'tests/audio_sink_test.c'),str(ROOT/'runtime/audio_sink.c'),'-o',str(exe)],check=True)
            self.assertIn('AUDIO_SINK_OK',subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout)
if __name__=='__main__':unittest.main()

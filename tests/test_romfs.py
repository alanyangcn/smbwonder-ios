from pathlib import Path
import importlib.util,subprocess,tempfile,unittest,struct
ROOT=Path(__file__).resolve().parents[1]
class RomfsTests(unittest.TestCase):
    def test_pack_and_real_reads_boundaries_and_stale_handles(self):
        tool=ROOT/'tools/pack_romfs.py'
        self.assertTrue(tool.exists(),'RomFS packer missing')
        with tempfile.TemporaryDirectory() as directory:
            d=Path(directory);rom=d/'source';(rom/'Folder').mkdir(parents=True)
            (rom/'Folder/hello.bin').write_bytes(b'0123456789');(rom/'empty').write_bytes(b'')
            pack=d/'romfs.wrom'
            subprocess.run(['python3',str(tool),str(rom),str(pack)],check=True)
            exe=d/'test'
            subprocess.run(['clang','-Wall','-Wextra','-I'+str(ROOT/'runtime'),str(ROOT/'tests/romfs_test.c'),str(ROOT/'runtime/romfs.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe),str(pack)],check=True)
            # Same archive truncated: validation must fail before mount.
            pack.write_bytes(pack.read_bytes()[:-1]);subprocess.run([str(exe),str(pack),'invalid'],check=True)
    def test_packer_rejects_symlink(self):
        tool=ROOT/'tools/pack_romfs.py';self.assertTrue(tool.exists())
        with tempfile.TemporaryDirectory() as directory:
            d=Path(directory);(d/'rom').mkdir();(d/'outside').write_bytes(b'private');(d/'rom/link').symlink_to(d/'outside')
            p=subprocess.run(['python3',str(tool),str(d/'rom'),str(d/'out')],capture_output=True)
            self.assertNotEqual(p.returncode,0);self.assertFalse((d/'out').exists())

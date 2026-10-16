from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class NvnTexLayoutTests(unittest.TestCase):
    def test_linear_render_target_is_pitch_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'test'; nvn=ROOT/'vendor/d2r-mobile/dm/gfx/nvn'; tex=ROOT/'vendor/d2r-mobile/dm/gfx/tex'
            subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(nvn),'-I'+str(tex),str(ROOT/'tests/nvn_tex_layout_test.c'),str(nvn/'nvn_tex.c'),str(tex/'nvn_deswizzle.c'),str(tex/'nvn_fmt_mtl.c'),'-o',str(exe)],check=True)
            out=subprocess.run([str(exe)],check=True,capture_output=True,text=True).stdout
            self.assertIn('nvn_tex_layout OK',out)
if __name__=='__main__':unittest.main()

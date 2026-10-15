from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
RUNTIME=ROOT/'runtime'
class NvnDeviceTests(unittest.TestCase):
    def test_strict_bootstrap_follows_wonder_call_order(self):
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/'nvn'
            subprocess.run(['clang','-std=c11','-pthread','-Wall','-Wextra','-Werror','-I'+str(RUNTIME),str(ROOT/'tests/nvn_device_test.c'),str(RUNTIME/'nvn_device.c'),str(RUNTIME/'nvn_swapchain.c'),str(RUNTIME/'nvn_buffer.c'),str(RUNTIME/'nvn_texture.c'),str(RUNTIME/'nvn_state.c'),str(ROOT/'vendor/d2r-mobile/dm/gfx/tex/nvn_fmt_mtl.c'),str(ROOT/'vendor/d2r-mobile/dm/gfx/tex/nvn_bcdec.c'),str(ROOT/'vendor/d2r-mobile/dm/gfx/tex/nvn_deswizzle.c'),'-I'+str(ROOT/'vendor/d2r-mobile/dm/gfx/tex'),str(ROOT/'vendor/d2r-mobile/dm/gfx/nvn/nvn_tex.c'),'-I'+str(ROOT/'vendor/d2r-mobile/dm/gfx/nvn'),'-o',str(exe)],check=True)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr + result.stdout[-1500:])
            out=result.stdout
            self.assertIn('NVN_STRICT_BOOTSTRAP_OK',out)
            self.assertRegex(out,r'NVN_PROC_TABLE pass=1 device=\S+ implemented=137 stop_thunks=397')
            self.assertRegex(out,r'NVN_PROC_TABLE pass=2 device=0x[0-9a-f]+ implemented=137 stop_thunks=397')
    def test_build_script_and_xcode_project_compile_same_runtime(self):
        import importlib.util,re
        spec=importlib.util.spec_from_file_location('b',ROOT/'tools/build_loader_probe.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        project=(ROOT/'ios-probe/project.yml').read_text()
        for path in m.RUNTIME_GRAPHICS:
            self.assertTrue((ROOT/path).exists(),path); self.assertIn('${SMBW_REFERENCE}/'+path,project)
    def test_header_matches_inventory_tool_count(self):
        header=(RUNTIME/'nvn_names.h').read_text()
        self.assertEqual(header.count('{"nvn'),534)
        self.assertIn('#define WONDER_NVN_API_MAJOR 55',header); self.assertIn('#define WONDER_NVN_API_MINOR_MIN 15',header)
if __name__=='__main__':unittest.main()

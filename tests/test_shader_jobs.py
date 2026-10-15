from pathlib import Path
import importlib.util,struct,unittest
class ShaderJobTests(unittest.TestCase):
    def module(self):
        p=Path(__file__).resolve().parents[1]/'tools/prepare_shader_jobs.py'
        spec=importlib.util.spec_from_file_location('wonder_jobs',p);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
    def test_strips_only_validated_nvn_header_and_checks_stage(self):
        b=bytearray(256);struct.pack_into('<I',b,0,0x12345678);struct.pack_into('<I',b,0x30,1<<10)
        self.assertEqual(self.module().graphics_payload(bytes(b),'vertex'),bytes(b[0x30:]))
        with self.assertRaises(ValueError):self.module().graphics_payload(bytes(b),'fragment')
    def test_rejects_truncated_header_and_unknown_marker(self):
        for b in [b'',bytes(80),bytes(256)]:
            with self.assertRaises(ValueError):self.module().graphics_payload(b,'vertex')

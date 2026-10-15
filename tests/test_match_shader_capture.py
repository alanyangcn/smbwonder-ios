from pathlib import Path
import importlib.util, unittest
spec=importlib.util.spec_from_file_location('capture',Path(__file__).resolve().parents[1]/'tools/match_shader_capture.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class ShaderCaptureTests(unittest.TestCase):
    def test_matches_control_and_code_at_exact_runtime_offset(self):
        c={'control':b'c'*2176+b'tail','code':b'p'*48+b'x'*256,'stage':'vertex','origin':'original'}
        match=m.match_stage(b'c'*2176,b'x'*256,[c])
        self.assertEqual(match['runtime_code_offset'],48)
        with self.assertRaises(ValueError):m.match_stage(b'd'*2176,b'x'*256,[c])
        with self.assertRaises(ValueError):m.match_stage(b'c'*2176,b'z'*256,[c])
    def test_rejects_ambiguous_payloads_beyond_prefix(self):
        a={'control':b'c'*2176,'code':b'x'*256+b'a','stage':'fragment'}
        b={**a,'code':b'x'*256+b'b'}
        with self.assertRaisesRegex(ValueError,'Ambiguous'):m.match_stage(a['control'],b'x'*256,[a,b])
    def test_identical_variations_are_one_binary_identity(self):
        a={'control':b'c'*2176,'code':b'x'*256,'stage':'fragment','origin':'a'}
        b={**a,'origin':'b'}
        self.assertEqual(m.match_stage(a['control'],b'x'*256,[a,b])['origins'],['a','b'])

    def test_short_original_shader_does_not_compare_adjacent_pool_padding(self):
        c={'control':b'c'*2176,'code':b'p'*48+b'x'*208,'stage':'fragment'}
        m=self.module if False else globals()['m']
        match=m.match_stage(b'c'*2176,b'x'*208+bytes(48),[c])
        self.assertEqual(match['runtime_code_offset'],48)
        self.assertEqual(match['captured_code_bytes_verified'],208)

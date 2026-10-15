"""验证无 llvm-objcopy 时仍以独立 LLVM 解码核对指令。 | Check independent LLVM decoding without llvm-objcopy."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OBJDUMP = Path('/Applications/Xcode-26.6.0.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-objdump')


class DisassemblyTests(unittest.TestCase):
    def decode(self, blob, base=0x1000):
        source = ROOT / 'vendor/d2r-mobile/dm/verify/aarch64_disasm.py'
        if not source.exists():
            self.fail('Independent disassembler adapter is not implemented')
        spec = importlib.util.spec_from_file_location('aarch64_disasm', source)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            # 目录刻意不含 objcopy，真实运行 Mach-O 回退。 | Deliberately omit objcopy to exercise the Mach-O fallback.
            (Path(directory) / 'llvm-objdump').symlink_to(OBJDUMP)
            return module.relevant_instructions(blob, base, directory)

    @unittest.skipUnless(OBJDUMP.is_file(), 'Requires Xcode LLVM objdump')
    def test_fallback_preserves_addresses_and_filters_unrelated_instructions(self):
        result = self.decode(bytes.fromhex('f20300aa1f2003d5'))
        self.assertEqual(set(result), {0x1000})
        self.assertIn('x18', result[0x1000])
        self.assertIn('x0', result[0x1000])

    @unittest.skipUnless(OBJDUMP.is_file(), 'Requires Xcode LLVM objdump')
    def test_system_register_and_svc_forms_are_not_lost(self):
        result = self.decode(bytes.fromhex('40d03bd5210000d420e03bd540e03bd5'))
        self.assertEqual(set(result), {0x1000, 0x1004, 0x1008, 0x100c})
        self.assertIn('tpidr_el0', result[0x1000])
        self.assertTrue(result[0x1004].startswith('svc'))
        self.assertIn('cntpct_el0', result[0x1008])
        self.assertIn('cntvct_el0', result[0x100c])

    def test_rejects_partial_aarch64_instruction(self):
        with self.assertRaises(ValueError):
            self.decode(b'\x00')

    def test_empty_text_is_empty(self):
        self.assertEqual(self.decode(b''), {})


if __name__ == '__main__':
    unittest.main()

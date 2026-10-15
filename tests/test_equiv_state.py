"""防止差分测试吞掉恰好位于 TSD 映射内的客体写入。 | Do not swallow guest writes inside the test TSD mapping."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VisibleWritesTests(unittest.TestCase):
    def filter(self, writes, patched=True):
        source = ROOT / 'vendor/d2r-mobile/dm/verify/equiv_state.py'
        if not source.exists():
            self.fail('Precise TSD write classification is not implemented')
        spec = importlib.util.spec_from_file_location('equiv_state', source)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.guest_writes(writes, patched, 0x2000000, [300, 301, 302, 303])

    def test_reproduced_guest_store_inside_tsd_mapping_remains_visible(self):
        # 实际失败样本：原版与补丁最终字节相同，但旧比较器仅吞掉补丁侧记录。
        # Real failing sample: final bytes agree, but the old comparator dropped only the patched record.
        guest = (0x2007751, 4, 10729717)
        internal = (0x2000000 + 300 * 8, 8, 7145934)
        self.assertEqual(self.filter([internal, guest]), [guest])

    def test_original_writes_are_never_filtered(self):
        write = (0x2000000 + 300 * 8, 8, 42)
        self.assertEqual(self.filter([write], patched=False), [write])

    def test_only_full_slot_writes_are_classified_as_internal(self):
        partial = (0x2000000 + 300 * 8, 4, 42)
        adjacent = (0x2000000 + 304 * 8, 8, 42)
        self.assertEqual(self.filter([partial, adjacent]), [partial, adjacent])

    def test_external_and_stack_writes_are_preserved_in_order(self):
        writes = [(0x100, 4, 1), (0x7ffff0, 8, 2), (0x108, 4, 3)]
        self.assertEqual(self.filter(writes), writes)


if __name__ == '__main__':
    unittest.main()

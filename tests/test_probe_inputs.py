"""诊断包只接受与验证清单相同的程序。 | The probe accepts only the exact verified program inputs."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProbeInputsTests(unittest.TestCase):
    def validate(self, root, profile):
        source = ROOT / 'tools/build_loader_probe.py'
        if not source.exists():
            self.fail('Verified-input probe builder is not implemented')
        spec = importlib.util.spec_from_file_location('build_loader_probe', source)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.validate_inputs(root, profile)

    def fixture(self, root):
        blob = bytearray(0x100)
        blob[:4] = b'NSO0'
        blob[0x40:0x60] = bytes(range(32))
        (root / 'input').write_bytes(blob)
        (root / 'patch').write_bytes(b'verified-patch')
        (root / 'fixups').write_bytes(b'verified-layout')
        (root / 'evidence').write_text('EQUIV main OK\n')
        def record(path):
            return {'path':path, 'sha256':hashlib.sha256((root / path).read_bytes()).hexdigest()}
        return {'modules':{'main':{'nso':record('input'), 'module_id':blob[0x40:0x60].hex(), 'patched':record('patch'), 'auxiliary':[record('fixups')]}},
                'checks':[dict(record('evidence'), marker='EQUIV main OK')]}

    def test_accepts_exact_verified_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertIsNone(self.validate(root, self.fixture(root)))

    def test_rejects_changed_guest_before_compilation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); profile = self.fixture(root)
            (root / 'patch').write_bytes(b'changed')
            with self.assertRaises(ValueError):self.validate(root, profile)

    def test_rejects_wrong_npdm_stack(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);profile=self.fixture(root)
            import struct
            blob=bytearray(0x80);blob[:4]=b'META';struct.pack_into('<I',blob,0x1c,0x200000)
            (root/'npdm').write_bytes(blob)
            profile['npdm']={'path':'npdm','sha256':hashlib.sha256(blob).hexdigest(),'main_thread_stack_size':0x100000}
            with self.assertRaises(ValueError):self.validate(root,profile)

    def test_rejects_changed_fixups(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); profile = self.fixture(root)
            (root / 'fixups').write_bytes(b'changed-layout')
            with self.assertRaises(ValueError):self.validate(root, profile)

    def test_rejects_changed_evidence_even_if_marker_still_exists(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); profile = self.fixture(root)
            (root / 'evidence').write_text('EQUIV main OK\nmodified\n')
            with self.assertRaises(ValueError):self.validate(root, profile)

    def test_rejects_a_different_module_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); profile = self.fixture(root)
            profile['modules']['main']['module_id'] = '00' * 32
            with self.assertRaises(ValueError):self.validate(root, profile)


if __name__ == '__main__':unittest.main()

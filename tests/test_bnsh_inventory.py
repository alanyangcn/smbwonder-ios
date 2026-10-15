"""Malformed shader containers must fail before their offsets are dereferenced."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import unittest

class BnshInventoryTests(unittest.TestCase):
    def parse(self, blob):
        path = Path(__file__).resolve().parents[1] / 'tools/inspect_bnsh.py'
        self.assertTrue(path.exists(), 'BNSH bounded parser missing')
        spec = importlib.util.spec_from_file_location('inspect_bnsh', path)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        return module.inspect_bnsh(blob)

    def fixture(self):
        b = bytearray(0x400); b[:4] = b'BNSH'
        struct.pack_into('<H', b, 0x16, 0x60); struct.pack_into('<I', b, 0x1c, len(b))
        b[0x60:0x64] = b'grsc'
        struct.pack_into('<IQ', b, 0x7c, 1, 0xc0)
        struct.pack_into('<Q', b, 0xd0, 0x100)
        struct.pack_into('<Q', b, 0x108, 0x200)
        struct.pack_into('<QQII', b, 0x208, 0x280, 0x300, 8, 16)
        b[0x300:0x308] = b'bytecode'
        return b

    def test_one_vertex_shader_and_hash(self):
        d = self.parse(self.fixture())
        self.assertEqual(d['variation_count'], 1)
        self.assertEqual(d['variations'][0]['stages'][0]['stage'], 'vertex')
        self.assertEqual(d['variations'][0]['stages'][0]['bytecode_size'], 8)
        self.assertEqual(d['variations'][0]['stages'][0]['bytecode_sha256'], hashlib.sha256(b'bytecode').hexdigest())

    def test_rejects_out_of_container_shader(self):
        b = self.fixture(); struct.pack_into('<Q', b, 0x210, 0x3fc)
        with self.assertRaises(ValueError):self.parse(b)

    def test_rejects_huge_variation_array(self):
        b = self.fixture(); struct.pack_into('<I', b, 0x7c, 0xffffffff)
        with self.assertRaises(ValueError):self.parse(b)

    def test_rejects_truncated_container(self):
        with self.assertRaises(ValueError):self.parse(self.fixture()[:-1])

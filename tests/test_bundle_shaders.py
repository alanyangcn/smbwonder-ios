"""App bundling of the translated shader build: only manifest-verified files are copied into dm/shaders."""
from pathlib import Path
import hashlib, importlib.util, json, tempfile, unittest
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_loader_probe', ROOT / 'tools/build_loader_probe.py')
probe = importlib.util.module_from_spec(spec); spec.loader.exec_module(probe)

def shader_build(directory, edit=None):
    keys = {}
    for key, stage in (('b6ef435899b15f38', 'vs'), ('b56073e27e6d9488', 'ps')):
        msl, refl = f'// {key}\n'.encode(), f'stage {stage}\n'.encode()
        (directory / f'{key}.metal').write_bytes(msl); (directory / f'{key}.refl').write_bytes(refl)
        keys[key] = {'stage': stage, 'msl_sha256': hashlib.sha256(msl).hexdigest(), 'refl_sha256': hashlib.sha256(refl).hexdigest()}
    (directory / 'manifest.json').write_text(json.dumps({'keys': keys}))
    (directory / 'jobs.json').write_text('[]')   # build input record, not shipped
    if edit: (directory / edit).write_text('// edited')

class BundleShaders(unittest.TestCase):
    def test_copies_manifest_and_verified_stages(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            src, dst = Path(a), Path(b) / 'shaders'; shader_build(src)
            self.assertEqual(probe.copy_shaders(src, dst), 2)
            self.assertEqual(sorted(p.name for p in dst.iterdir()), sorted(['manifest.json', 'b6ef435899b15f38.metal', 'b6ef435899b15f38.refl', 'b56073e27e6d9488.metal', 'b56073e27e6d9488.refl']))
    def test_refuses_edited_stage(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            src = Path(a); shader_build(src, edit='b56073e27e6d9488.refl')
            with self.assertRaisesRegex(ValueError, 'b56073e27e6d9488.refl'): probe.copy_shaders(src, Path(b) / 'shaders')
    def test_missing_build_ships_no_shaders(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            dst = Path(b) / 'shaders'
            self.assertEqual(probe.copy_shaders(Path(a) / 'absent', dst), 0)
            self.assertFalse(dst.exists())

if __name__ == '__main__': unittest.main()

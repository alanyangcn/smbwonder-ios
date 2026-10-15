"""Wonder translator: NEAR_IS_MINUS_W depth remap in vertex stages only (game data in work/, skipped without it)."""
from pathlib import Path
import json, os, subprocess, tempfile, unittest
ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work'
XLATE = WORK / 'tools/wonder_xlate/wonder_xlate'
CROSS = WORK / 'tools/spirv-cross/spirv-cross'
SAMPLES = WORK / 'build/metal2-shaders/exploratory'
ENV = {**os.environ, 'DOTNET_ROOT': str(WORK / 'tools/dotnet')}
VS, FS = SAMPLES / '78cc4a70148d800cfb0a6c69.bin', SAMPLES / 'a241834ba9fed232042bcf38.bin'

def translate(directory, code, stage, depth_minus_one):
    key = f"{stage}{int(depth_minus_one)}"
    job = [{'key': key, 'stage': stage, 'code': str(code), 'tex': {}, 'img': {}, 'attr': {}, 'fout': {},
            'debug': False, 'reduced': False, 'depth_minus_one': depth_minus_one}]
    jobs = Path(directory) / f'{key}.json'; jobs.write_text(json.dumps(job))
    subprocess.run([str(XLATE), str(jobs), str(directory)], check=True, capture_output=True, env=ENV)
    spv = Path(directory) / f'{key}.spv'
    msl = subprocess.run([str(CROSS), str(spv), '--msl', '--msl-ios', '--msl-version', '20100', '--msl-decoration-binding'],
                         check=True, capture_output=True, text=True).stdout
    return spv.read_bytes(), msl

@unittest.skipUnless(XLATE.exists() and CROSS.exists() and VS.exists() and FS.exists(), 'needs work/ tools and sample shaders')
class WonderXlateTests(unittest.TestCase):
    def test_vertex_remaps_minus_one_depth(self):
        with tempfile.TemporaryDirectory() as d:
            _, remapped = translate(d, VS, 'vs', True)
            _, plain = translate(d, VS, 'vs', False)
        # Ryujinx EmitterContext: z' = fma(z, 0.5, w * 0.5) when DepthMode and no host depth clip control.
        self.assertIn('gl_Position.z = fma(', remapped)
        self.assertNotIn('gl_Position.z = fma(', plain)

    def test_fragment_unchanged(self):
        with tempfile.TemporaryDirectory() as d:
            a, _ = translate(d, FS, 'ps', True)
            b, _ = translate(d, FS, 'ps', False)
        self.assertEqual(a, b)

    def test_source_sha_reports_wonder_sources(self):
        out = subprocess.run([str(XLATE), '--source-sha'], check=True, capture_output=True, text=True, env=ENV).stdout.strip()
        self.assertRegex(out, r'^[0-9a-f]{16}$')

if __name__ == '__main__': unittest.main()

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

CENSUS_FS = WORK / 'analysis/first-frame-round53/Documents/DrawCensus/stage-1.code.bin'   # samples c[2] word 8

def translate(directory, code, stage, depth_minus_one, tex=None):
    key = f"{stage}{int(depth_minus_one)}"
    job = [{'key': key, 'stage': stage, 'code': str(code), 'tex': tex or {}, 'img': {}, 'attr': {}, 'fout': {},
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

    @unittest.skipUnless(CENSUS_FS.exists(), 'needs the round53 draw census')
    def test_plain_tex_dimension_comes_from_the_instruction(self):
        # Ryujinx asks QuerySamplerType only for buffer textures, TXQ and bindless elimination; a plain TEX keeps its own
        # dimension, so a wrong census fact cannot silently change this shader.
        with tempfile.TemporaryDirectory() as d:
            sph = Path(d) / 'fs.sph'; sph.write_bytes(CENSUS_FS.read_bytes()[0x30:])
            _, msl = translate(d, sph, 'ps', True, {'0': '2darray'})
        self.assertIn('texture2d<float>', msl)

    def test_source_sha_reports_wonder_sources(self):
        out = subprocess.run([str(XLATE), '--source-sha'], check=True, capture_output=True, text=True, env=ENV).stdout.strip()
        self.assertRegex(out, r'^[0-9a-f]{16}$')


class TextureTableTests(unittest.TestCase):
    def test_translator_texture_table_matches_sdk(self):
        # Wonder SDK BindTexture (sdk+0x468840) writes slot r at c[2][0x20 + 8r]; translator and build tool must agree.
        import importlib.util, re
        spec = importlib.util.spec_from_file_location('wonder_shader_build', ROOT / 'tools/wonder_shader_build.py')
        build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)
        source = (ROOT / 'tools/wonder_xlate/Program.cs').read_text()
        base, end = (int(x, 16) for x in re.search(r'const int TexBase = (0x[0-9a-fA-F]+), SampBase = (0x[0-9a-fA-F]+);', source).groups())
        self.assertEqual(base, build.TEX_TABLE); self.assertEqual(end, build.TEX_TABLE + 8 * build.TEX_SLOTS)

if __name__ == '__main__': unittest.main()

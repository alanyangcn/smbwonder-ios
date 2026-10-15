from pathlib import Path
import hashlib,importlib.util,json,tempfile,unittest
TOOL=Path(__file__).resolve().parents[1]/'tools/compile_metal2_shaders.py'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
class ShaderProvenanceTests(unittest.TestCase):
    def module(self):
        spec=importlib.util.spec_from_file_location('compile_wonder',TOOL);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
    def fixture(self,d):
        key='a'*24;(d/'spv').mkdir();code=d/'input.bin';code.write_bytes(b'original');spv=d/'spv'/(key+'.spv');spv.write_bytes(b'translated')
        jobs=[{'key':key,'stage':'vs','code':str(code)}];manifest={'runtime_eligible':False,'pairs':[{'stages':[{'key':key,'stage':'vertex','sph_code_sha256':sha(code)}]}]}
        (d/'jobs.json').write_text(json.dumps(jobs));(d/'manifest.json').write_text(json.dumps(manifest))
        proof={'jobs_sha256':sha(d/'jobs.json'),'source_manifest_sha256':sha(d/'manifest.json'),'stages':[{**jobs[0],'input_sha256':sha(code),'spirv_sha256':sha(spv)}]}
        (d/'spv/translation-manifest.json').write_text(json.dumps(proof));return key,jobs
    def test_bound_translation_and_replaced_spirv(self):
        with tempfile.TemporaryDirectory() as directory:
            d=Path(directory);key,jobs=self.fixture(d);self.module().verify_translation(d)
            (d/'spv'/(key+'.spv')).write_bytes(b'replaced')
            with self.assertRaises(ValueError):self.module().verify_translation(d)
    def test_empty_missing_duplicate_and_wrong_stage_jobs(self):
        for mode in ['empty','missing','duplicate','wrong_stage']:
            with self.subTest(mode=mode),tempfile.TemporaryDirectory() as directory:
                d=Path(directory);key,jobs=self.fixture(d)
                if mode=='empty':jobs=[]
                elif mode=='missing':
                    path=d/'manifest.json';m=json.loads(path.read_text());m['pairs'][0]['stages'].append({'key':'b'*24,'stage':'fragment','sph_code_sha256':sha(d/'input.bin')});path.write_text(json.dumps(m))
                elif mode=='duplicate':jobs*=2
                else:jobs[0]['stage']='ps'
                (d/'jobs.json').write_text(json.dumps(jobs))
                # Rebind file hashes: collection/stage checks must still reject this.
                p=d/'spv/translation-manifest.json';proof=json.loads(p.read_text());proof['jobs_sha256']=sha(d/'jobs.json');proof['source_manifest_sha256']=sha(d/'manifest.json');p.write_text(json.dumps(proof))
                with self.assertRaises(ValueError):self.module().verify_translation(d)

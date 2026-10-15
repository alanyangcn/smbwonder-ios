#!/usr/bin/env python3
"""Run the local Maxwell translator and bind exact inputs, tool DLLs and SPIR-V outputs."""
import argparse,hashlib,json,os,re,subprocess,tempfile
from pathlib import Path

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def translate(directory,dotnet,translator):
    jobs_path=directory/'jobs.json';manifest_path=directory/'manifest.json'
    jobs=json.loads(jobs_path.read_text());manifest=json.loads(manifest_path.read_text());expected={s['key']:s for p in manifest['pairs'] for s in p['stages']}
    if manifest.get('runtime_eligible') is not False or not jobs or len(jobs)>256:raise ValueError('Expected nonempty exploratory jobs')
    keys=[j['key'] for j in jobs]
    if len(set(keys))!=len(keys) or set(keys)!=set(expected):raise ValueError('Incomplete or duplicate job list')
    for job in jobs:
        key=job['key']
        if not re.fullmatch(r'[0-9a-f]{24}',key) or job['stage']!={'vertex':'vs','fragment':'ps'}[expected[key]['stage']]:raise ValueError('Invalid key or stage')
        if sha(Path(job['code']))!=expected[key]['sph_code_sha256']:raise ValueError('Input shader identity mismatch')
    output=directory/'spv'
    if output.exists():raise FileExistsError('Use a fresh output directory; preserve old outputs before translating')
    inputs={'jobs_sha256':sha(jobs_path),'source_manifest_sha256':sha(manifest_path)}
    tools=[{'name':p.name,'sha256':sha(p)} for p in sorted(translator.parent.glob('*.dll'))]
    with tempfile.TemporaryDirectory(prefix='.xlate-',dir=directory) as temporary:
        target=Path(temporary)/'spv';command=[str(dotnet),str(translator),str(jobs_path.resolve()),str(target)]
        subprocess.run(command,check=True)
        if sha(jobs_path)!=inputs['jobs_sha256'] or sha(manifest_path)!=inputs['source_manifest_sha256']:raise ValueError('Inputs changed during translation')
        records=[]
        for job in jobs:
            key=job['key'];actual=sha(Path(job['code']))
            if actual!=expected[key]['sph_code_sha256']:raise ValueError('Shader changed during translation')
            res=target/(key+'.res.json');metadata=json.loads(res.read_text())
            if metadata['key']!=key or metadata['stage']!=job['stage']:raise ValueError('Translator result mismatch')
            records.append({'key':key,'stage':job['stage'],'input_sha256':actual,'spirv_sha256':sha(target/(key+'.spv')),'reflection_sha256':sha(res),'notes':metadata['notes']})
        proof={**inputs,'scope':'exploratory translation, reflection facts not validated','runtime_eligible':False,'tools':tools,'command':command,'stages':records}
        (target/'translation-manifest.json').write_text(json.dumps(proof,indent=2)+'\n');os.replace(target,output)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--dotnet',type=Path,required=True);p.add_argument('--translator',type=Path,required=True);a=p.parse_args();translate(a.directory,a.dotnet,a.translator)

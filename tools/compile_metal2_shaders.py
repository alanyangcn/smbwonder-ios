#!/usr/bin/env python3
"""Compile exploratory original-game stages with discrete MSL 2.1 resource bindings.
Successful compilation is not reflection validation or permission to use an artifact at runtime.
"""
import argparse,hashlib,json,re,subprocess
from pathlib import Path

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def verify_translation(directory):
    manifest=json.loads((directory/'manifest.json').read_text());jobs=json.loads((directory/'jobs.json').read_text())
    if manifest.get('runtime_eligible') is not False:raise ValueError('Expected explicitly exploratory manifest')
    expected={s['key']:s for p in manifest['pairs'] for s in p['stages']}
    if not isinstance(jobs,list) or not jobs or len(jobs)>256:raise ValueError('Empty or oversized job list')
    keys=[j['key'] for j in jobs]
    if len(set(keys))!=len(keys) or set(keys)!=set(expected):raise ValueError('Duplicate or incomplete job set')
    proof=json.loads((directory/'spv/translation-manifest.json').read_text())
    if proof['jobs_sha256']!=sha(directory/'jobs.json') or proof['source_manifest_sha256']!=sha(directory/'manifest.json'):raise ValueError('Translation inputs changed')
    stages={s['key']:s for s in proof['stages']}
    if len(stages)!=len(proof['stages']) or set(stages)!=set(expected):raise ValueError('Incomplete translation proof')
    for job in jobs:
        key=job['key'];original=expected[key];stage=stages[key]
        if not re.fullmatch(r'[0-9a-f]{24}',key):raise ValueError('Invalid shader identity')
        if job['stage']!={'vertex':'vs','fragment':'ps'}[original['stage']] or stage['stage']!=job['stage']:raise ValueError('Shader stage mismatch')
        if sha(Path(job['code']))!=original['sph_code_sha256'] or stage['input_sha256']!=original['sph_code_sha256']:raise ValueError('Shader bytecode changed')
        if sha(directory/'spv'/(key+'.spv'))!=stage['spirv_sha256']:raise ValueError('SPIR-V differs from recorded translation')
        if 'reflection_sha256' in stage and sha(directory/'spv'/(key+'.res.json'))!=stage['reflection_sha256']:raise ValueError('Translation reflection changed')
    return manifest,jobs

def compile_stages(directory,cross,output,source_only=False):
    manifest,jobs=verify_translation(directory)
    expected={s['key']:s for p in manifest['pairs'] for s in p['stages']}
    output.mkdir(parents=True,exist_ok=True);records=[]
    for job in jobs:
        key=job['key']
        if not re.fullmatch(r'[0-9a-f]{24}',key) or key not in expected:raise ValueError('Invalid shader identity')
        code=Path(job['code']);spv=directory/'spv'/(key+'.spv')
        if sha(code)!=expected[key]['sph_code_sha256']:raise ValueError('Shader input changed after extraction')
        msl=output/(key+'.metal');air=output/(key+'.air');lib=output/(key+'.metallib');reflection=output/(key+'.spirv-reflect.json')
        commands=[
            [str(cross),str(spv),'--msl','--msl-ios','--msl-version','20100','--msl-decoration-binding','--output',str(msl)],
            [str(cross),str(spv),'--reflect','--output',str(reflection)],
            ['xcrun','--sdk','iphoneos','metal','-std=ios-metal2.1','-mios-version-min=16.0','-c',str(msl),'-o',str(air)],
            ['xcrun','--sdk','iphoneos','metallib',str(air),'-o',str(lib)]]
        record={'key':key,'stage':job['stage'],'input_sha256':sha(code),'spirv_sha256':sha(spv),'commands':commands[:2] if source_only else commands,'generated':False,'compiled':False,'runtime_eligible':False}
        for command in record['commands']:
            result=subprocess.run(command,capture_output=True,text=True)
            if result.returncode:
                record['failure']={'exit_code':result.returncode,'command':command,'stderr':result.stderr};break
        else:
            record.update(generated=True,compiled=not source_only,msl_sha256=sha(msl),reflection_sha256=sha(reflection))
            if not source_only:record['metallib_sha256']=sha(lib)
        records.append(record);print(key,job['stage'],'MSL_GENERATED' if source_only and record['generated'] else 'COMPILED' if record['compiled'] else 'FAILED')
    result={'scope':'exploratory syntax compilation only; not game-frame validation','backend':'metal2','msl':'2.1','ios_min':'16.0','discrete_binding':True,'spirv_cross_sha256':sha(cross),'source_manifest_sha256':sha(directory/'manifest.json'),'translation_manifest_sha256':sha(directory/'spv/translation-manifest.json'),'offline_compile_requested':not source_only,'generated':sum(r['generated'] for r in records),'compiled':sum(r['compiled'] for r in records),'total':len(records),'reflection_validated':False,'runtime_eligible':False,'stages':records}
    (output/'compile-result.json').write_text(json.dumps(result,indent=2)+'\n')
    if result['generated']==result['total']:
        sources={'scope':'original-game MSL compilation probe; not runtime qualified','runtime_eligible':False,'source_manifest_sha256':result['source_manifest_sha256'],'translation_manifest_sha256':result['translation_manifest_sha256'],'compile_result_sha256':sha(output/'compile-result.json'),'stages':[{k:r[k] for k in ['key','stage','input_sha256','spirv_sha256','msl_sha256']} for r in records]}
        (output/'sources.json').write_text(json.dumps(sources,indent=2)+'\n')
    return 0 if (result['generated'] if source_only else result['compiled'])==result['total'] else 1
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--spirv-cross',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--source-only',action='store_true',help='Generate MSL for device compilation without the optional offline Metal Toolchain');a=p.parse_args();raise SystemExit(compile_stages(a.directory,a.spirv_cross,a.output,a.source_only))

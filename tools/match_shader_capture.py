#!/usr/bin/env python3
"""Match live NVN control/code prefixes to original BNSH payloads; no runtime qualification implied."""
import argparse, hashlib, json
from pathlib import Path

def digest(data):return hashlib.sha256(data).hexdigest()
def match_stage(control,code,candidates):
    matches={}
    for c in candidates:
        if not c['control'].startswith(control):continue
        for offset in (0,0x30):
            if c['code'][offset:offset+len(code)]!=code:continue
            identity=(digest(c['code']),digest(c['control']),offset,c['stage'])
            if identity not in matches:matches[identity]={**c,'runtime_code_offset':offset,'origins':[]}
            matches[identity]['origins'].append(c.get('origin'))
    if len(matches)!=1:raise ValueError(f'Ambiguous or unknown live shader: {len(matches)} binary identities')
    return next(iter(matches.values()))

def match_capture(capture,inputs,output):
    from inspect_bnsh import inspect_file
    manifest=json.loads((capture/'manifest.json').read_text())
    if manifest.get('complete') is not True or not 1<=manifest['count']<=6 or len(manifest['stages'])!=manifest['count']:
        raise ValueError('Incomplete shader capture')
    candidates=[]
    for path in inputs:
        record=inspect_file(path);raw=path.read_bytes()
        if path.suffix=='.zs':
            import zstandard
            raw=zstandard.ZstdDecompressor().decompress(raw,max_output_size=256*1024*1024)
        for ci,c in enumerate(record['containers']):
            base=c['container_offset']
            for v in c['variations']:
                for s in v['stages']:
                    code=raw[base+s['bytecode_offset']:base+s['bytecode_offset']+s['bytecode_size']]
                    control=raw[base+s['control_offset']:base+s['control_offset']+s['control_size']]
                    if digest(code)!=s['bytecode_sha256'] or digest(control)!=s['control_sha256']:raise ValueError('Source identity mismatch')
                    candidates.append({'code':code,'control':control,'stage':s['stage'],'origin':{'source':record['source_name'],'source_sha256':record['source_sha256'],'container':ci,'variation':v['index']}})
    if output.exists():raise FileExistsError('Preserve existing capture match output')
    matches=[]
    for i in range(manifest['count']):
        control=(capture/f'stage-{i}.control.bin').read_bytes();code=(capture/f'stage-{i}.code.bin').read_bytes()
        if len(control)!=0x880 or len(code)!=256:raise ValueError('Truncated captured prefixes')
        matches.append(match_stage(control,code,candidates))
    output.mkdir(parents=True);records=[]
    from prepare_shader_jobs import graphics_payload
    for i,m in enumerate(matches):
        key=digest(m['code'])[:24];(output/(key+'.bin')).write_bytes(graphics_payload(m['code'],m['stage']))
        (output/(key+'.control.bin')).write_bytes(m['control'])
        records.append({k:v for k,v in m.items() if k not in ('code','control')})
        records[-1].update(index=i,key=key,code_sha256=digest(m['code']),control_sha256=digest(m['control']),code_size=len(m['code']),control_size=len(m['control']))
    proof={'capture':manifest,'capture_manifest_sha256':digest((capture/'manifest.json').read_bytes()),'runtime_eligible':False,'scope':'Live prefix matched to unique original binary; complete runtime code and bindings still require validation','stages':records}
    (output/'matched.json').write_text(json.dumps(proof,indent=2)+'\n')
    print(json.dumps(proof,indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=Path);p.add_argument('inputs',nargs='+',type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();match_capture(a.capture,a.inputs,a.output)

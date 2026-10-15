#!/usr/bin/env python3
"""Extract paired BNSH stages for exploratory translation, never runtime-qualified shader facts."""
import argparse,hashlib,json,struct
from pathlib import Path

def graphics_payload(code,stage):
    # NVN prefix then a 0x50-byte Maxwell SPH. Stage bits follow Ryujinx ShaderHeader.
    if len(code)<0x80 or struct.unpack_from('<I',code)[0]!=0x12345678:
        raise ValueError('Unknown or truncated NVN graphics header')
    expected={'vertex':1,'fragment':5}.get(stage)
    if expected is None or (struct.unpack_from('<I',code,0x30)[0]>>10)&15!=expected:
        raise ValueError('BNSH and Maxwell SPH stage mismatch')
    return code[0x30:]

def prepare(inputs,output,pair_limit):
    from inspect_bnsh import inspect_file
    candidates=[];sources=[]
    for path in inputs:
        record=inspect_file(path);sources.append(record);raw=path.read_bytes()
        if path.suffix=='.zs':
            import zstandard
            raw=zstandard.ZstdDecompressor().decompress(raw,max_output_size=256*1024*1024)
        for ci,container in enumerate(record['containers']):
            for variation in container['variations']:
                if {s['stage'] for s in variation['stages']}!={'vertex','fragment'}:continue
                candidates.append((sum(s['bytecode_size'] for s in variation['stages']),len(sources)-1,ci,variation['index'],raw))
    output.mkdir(parents=True,exist_ok=True);jobs=[];manifest=[];seen=set();pairs=0
    for total,si,ci,vi,raw in sorted(candidates,key=lambda x:x[:4]):
        container=sources[si]['containers'][ci];variation=container['variations'][vi]
        identity=tuple(s['bytecode_sha256'] for s in variation['stages'])
        if identity in seen:continue
        seen.add(identity)
        if pairs>=pair_limit:break
        pair=[]
        for stage in variation['stages']:
            offset=container['container_offset']+stage['bytecode_offset'];code=raw[offset:offset+stage['bytecode_size']]
            if hashlib.sha256(code).hexdigest()!=stage['bytecode_sha256']:raise ValueError('Payload identity mismatch')
            payload=graphics_payload(code,stage['stage']);key=stage['bytecode_sha256'][:24];target=output/(key+'.bin');target.write_bytes(payload)
            control_offset=container['container_offset']+stage['control_offset'];control=raw[control_offset:control_offset+stage['control_size']]
            (output/(key+'.control.bin')).write_bytes(control)
            if not any(j['key']==key for j in jobs):
                jobs.append({'key':key,'stage':{'vertex':'vs','fragment':'ps'}[stage['stage']],'code':str(target.resolve()),'tex':{},'img':{},'attr':{},'fout':{},'debug':False,'reduced':False})
            pair.append({'key':key,'stage':stage['stage'],'payload_sha256':stage['bytecode_sha256'],'sph_code_sha256':hashlib.sha256(payload).hexdigest(),'control_sha256':stage['control_sha256'],'control_size':stage['control_size']})
        manifest.append({'source':sources[si]['source_name'],'source_sha256':sources[si]['source_sha256'],'container_sha256':container['sha256'],'container_index':ci,'variation':vi,'stages':pair});pairs+=1
    if not jobs:raise ValueError('No supported paired graphics shaders')
    (output/'jobs.json').write_text(json.dumps(jobs,indent=2)+'\n')
    (output/'manifest.json').write_text(json.dumps({'scope':'exploratory original-game shader translation; not runtime-qualified','pairs':manifest,'facts_verified':False,'unverified':['attribute and output numeric types','texture and image types and slots','driver constant layout and c1','coordinate conventions and actual draw state'],'runtime_eligible':False},indent=2)+'\n')
    print(f'Extracted {len(manifest)} pairs / {len(jobs)} unique stages; reflection and draw state remain unverified')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('inputs',type=Path,nargs='+');p.add_argument('--output',type=Path,required=True);p.add_argument('--pairs',type=int,default=2);a=p.parse_args()
    if not 1<=a.pairs<=128:p.error('--pairs must be 1..128')
    prepare(a.inputs,a.output,a.pairs)

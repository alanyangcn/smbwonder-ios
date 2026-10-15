#!/usr/bin/env python3
"""Stream a local, extracted RomFS into an indexed read-only archive; no game data in source."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,struct

def pack(root,output):
    root=root.resolve();files=[]
    for path in root.rglob('*'):
        if path.is_symlink():raise ValueError('Symlinks are not RomFS input')
        if not path.is_file():continue
        name=path.relative_to(root).as_posix().encode('utf-8')
        if len(name)>0x300 or any(c in name for c in (b':',b'\\',b'\0')):raise ValueError('Invalid RomFS path')
        files.append((name,path,path.stat().st_size))
    files.sort();index=b''.join(struct.pack('<HQ',len(n),sz)+n for n,p,sz in files)
    if len(index)>16*1024*1024 or len(files)>100000:raise ValueError('RomFS index exceeds host bounds')
    temp=output.with_name(output.name+'.partial');output.parent.mkdir(parents=True,exist_ok=True)
    try:
        with temp.open('wb') as out:
            out.write(struct.pack('<8sQQQ',b'WONDER01',len(files),len(index),sum(s for n,p,s in files)));out.write(index)
            for name,path,size in files:
                before=out.tell()
                with path.open('rb') as src:shutil.copyfileobj(src,out,1024*1024)
                if out.tell()-before!=size:raise ValueError('Input changed while packing')
        os.replace(temp,output)
    finally:
        if temp.exists():temp.unlink()
    h=hashlib.sha256()
    with output.open('rb') as src:
        for data in iter(lambda:src.read(1024*1024),b''):h.update(data)
    result={'files':len(files),'payload_bytes':sum(s for n,p,s in files),'archive_bytes':output.stat().st_size,'sha256':h.hexdigest(),'index_sha256':hashlib.sha256(index).hexdigest()}
    output.with_suffix(output.suffix+'.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('output',type=Path);a=p.parse_args();pack(a.root,a.output)

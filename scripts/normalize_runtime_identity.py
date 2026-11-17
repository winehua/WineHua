#!/usr/bin/env python3
"""Normalize tracked runtime ELFs before hashing/packaging, matching release strip.
Refuse stale inputs or changes to allocated sections / program headers.
Only known, manifest-paired libraries are rewritten; source ELF code is intact.
"""
import argparse, copy, hashlib, io, json, re, struct, subprocess, tempfile
from pathlib import Path, PurePosixPath
from zipfile import ZipFile

def sha(data): return hashlib.sha256(data).hexdigest()

def load_identity(data):
    if data[:6] != b'\x7fELF\x02\x01': raise ValueError('expected ELF64 little-endian')
    shoff, = struct.unpack_from('<Q',data,40)
    entsize,count,names = struct.unpack_from('<HHH',data,58)
    if not count or entsize != 64: raise ValueError('unsupported section table')
    sections=[struct.unpack_from('<IIQQQQIIQQ',data,shoff+i*entsize) for i in range(count)]
    stringtable=sections[names];strings=data[stringtable[4]:stringtable[4]+stringtable[5]]
    allocated=[]
    for name,kind,flags,addr,offset,size,*_ in sections:
        if flags & 2:
            label=strings[name:strings.index(0,name)]
            allocated.append((label,kind,flags,addr,size,b'' if kind==8 else data[offset:offset+size]))
    phoff,=struct.unpack_from('<Q',data,32)
    phsize,phcount=struct.unpack_from('<HH',data,54)
    return (data[16:24], data[48:52], data[phoff:phoff+phsize*phcount], allocated)

def normalize(libs,rawfile,strip):
    manifest_path=rawfile/'wine-runtime-manifest.json'
    if not manifest_path.exists(): return False
    manifest=json.loads(manifest_path.read_text())
    version_path=rawfile/'wine_runtime.version';version=version_path.read_text()
    records=[(key,manifest[key]) for key in ('sharedAudio','threadExit') if key in manifest]
    records += [(name,dict(row,library='libs/arm64-v8a/'+name))
        for name,row in manifest.get('graphicsMigration',{}).get('virgl',{}).items()]
    changes=[];nested={}
    with tempfile.TemporaryDirectory(prefix='runtime-normalize-') as d:
        for key,record in records:
            member=PurePosixPath(record['library'])
            if len(member.parts)!=3 or member.parts[0]!='libs' or '..' in member.parts:
                raise ValueError('invalid manifest library path')
            target=libs.joinpath(*member.parts[1:]);old=target.read_bytes()
            if sha(old)!=record['sha256']: raise ValueError('stale manifest library: '+str(member))
            new=Path(d)/member.name;new.write_bytes(old)
            subprocess.run([str(strip),'--strip-all','--remove-section=.comment',str(new)],check=True)
            data=new.read_bytes()
            if load_identity(old)!=load_identity(data): raise ValueError('strip changed loadable ELF: '+str(member))
            if data==old: continue
            changes.append((target,data))
            for path in record.get('nestedMembers',[]):nested[path]=(sha(old),data)
            if key in ('sharedAudio','threadExit'):
                manifest[key]['sha256']=sha(data)
                marker='audio' if key=='sharedAudio' else 'thread-exit'
                version=re.sub(r'(?<![\w-])'+marker+r'=[^;\s]+',marker+'='+sha(data)[:16],version)
            else: manifest['graphicsMigration']['virgl'][key]['sha256']=sha(data)
    if nested:
        payload=rawfile/'wine-data.zip';buffer=io.BytesIO()
        with ZipFile(payload) as source,ZipFile(buffer,'w',allowZip64=True) as out:
            for path,(expected,data) in nested.items():
                if sha(source.read(path))!=expected: raise ValueError('stale nested runtime: '+path)
            for entry in source.infolist():
                out.writestr(copy.copy(entry),nested[entry.filename][1] if entry.filename in nested else source.read(entry.filename))
        data=buffer.getvalue();changes.append((payload,data));manifest['payloadSha256']=sha(data)
        version=re.sub(r'content=[^;\s]+','content='+sha(data)[:16],version)
    if not changes: return False
    changes += [(manifest_path,(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n').encode()),(version_path,version.encode())]
    # Validation above completes before writing any staged input.
    for path,data in changes:
        tmp=path.with_name(path.name+'.normalize.tmp');tmp.write_bytes(data);tmp.replace(path)
    print('runtime identity: normalized release metadata; loadable ELF content unchanged')
    return True

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--libs',type=Path,required=True);p.add_argument('--rawfile',type=Path,required=True)
    p.add_argument('--strip',type=Path,required=True);a=p.parse_args()
    normalize(a.libs,a.rawfile,a.strip)

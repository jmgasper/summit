#!/usr/bin/env python3
"""Compile and run production CRX verification/extraction in the native Haiku VM."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import secrets
import shlex
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
NATIVE = r'''
from pathlib import Path
import hashlib,json,os,shlex,subprocess,sys,time
stage=Path(sys.argv[1]);bundle=Path(sys.argv[2]);build=Path('/SummitExtensions/WebKit/WebKitBuild/Modern')
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
inputs=json.loads((stage/'inputs.json').read_text())
assert all(sha(stage/name)==value for name,value in inputs.items())
temporary=stage/'tmp';temporary.mkdir()
environment=dict(os.environ,TMPDIR=str(temporary),LIBRARY_PATH=str(bundle/'lib')+':/boot/system/lib')
report={'scope':'Production CRX3 verifier, archive extraction and existing ZIP/XPI regressions; no extension/browser execution','stage':str(stage),'bundle':str(bundle),'passed':False,'units':[],'runs':[]}
try:
 target='Source/WebKit/CMakeFiles/WebKit.dir/UIProcess/Extensions/haiku/WebExtensionArchiveHaiku.cpp.o'
 lines=subprocess.check_output(['ninja','-t','commands',target],cwd=build,text=True).splitlines()
 original=next(shlex.split(line) for line in reversed(lines) if ' -c ' in line and '/WebExtensionArchiveHaiku.cpp' in line)
 flags=[];i=1
 while i<len(original):
  flag=original[i]
  if flag in ['-c','-o','-MF','-MT','-MQ']: i+=2;continue
  if flag in ['-MD','-MMD']: i+=1;continue
  if flag=='-include' and 'cmake_pch' in original[i+1]:i+=2;continue
  flags.append(flag);i+=1
 flags=['-I'+str(stage),'-I'+str(stage/'vendor'),*flags,'-fmax-errors=3']
 watched={str(build/'build.ninja'):sha(build/'build.ninja'),str(bundle/'build-manifest.json'):sha(bundle/'build-manifest.json')}
 for path in [bundle/'lib/libJavaScriptCore.so.18.7.4',Path('/boot/system/lib/libcrypto.so.3'),Path('/boot/home/summit-deps/libzip-1.11.4/lib/libzip.so.5.5')]:watched[str(path)]=sha(path)
 names=['WebExtensionCRXHaiku','WebExtensionArchiveHaiku','EngineExtensionCRXTests','EngineExtensionCRXArchiveTests','EngineExtensionArchiveTests']
 for name in names:
  source=stage/(name+'.cpp');obj=stage/(name+'.o');dep=stage/(name+'.d')
  command=[original[0],*flags,'-M','-MF',str(dep),'-MT',name,str(source)]
  result=subprocess.run(command,cwd=build,env=environment,capture_output=True,text=True,timeout=120)
  if result.returncode: raise RuntimeError(result.stdout+result.stderr)
  raw=dep.read_text().replace('\\\n',' ')
  for value in shlex.split(raw.split(':',1)[1]):
   path=Path(value);path=path if path.is_absolute() else build/path;watched[str(path)]=sha(path)
  command=[original[0],*flags,'-c',str(source),'-o',str(obj)]
  result=subprocess.run(command,cwd=build,env=environment,capture_output=True,text=True,timeout=180)
  report['units'].append({'name':name,'command':command,'exit':result.returncode,'output':result.stdout+result.stderr})
  if result.returncode: raise RuntimeError('Compile failed: '+name)
 common=[str(stage/'WebExtensionCRXHaiku.o'),str(stage/'WebExtensionArchiveHaiku.o')]
 libraries=['-Wl,--no-export-dynamic','-Wl,--gc-sections',str(bundle/'lib/libJavaScriptCore.so.18.7.4'),'/boot/home/summit-deps/libzip-1.11.4/lib/libzip.so.5.5','-lcrypto','-lbe','-lnetwork','-Wl,-rpath,'+str(bundle/'lib'),'-Wl,-rpath,/boot/home/summit-deps/libzip-1.11.4/lib']
 for name,args in [('EngineExtensionCRXTests',[str(stage/'crx-fixtures')]),('EngineExtensionCRXArchiveTests',[str(stage/'crx-fixtures'),str(temporary)]),('EngineExtensionArchiveTests',[str(stage/'zip-fixtures')])]:
  executable=stage/name;command=['c++',str(stage/(name+'.o')),*common,*libraries,'-o',str(executable)]
  linked=subprocess.run(command,cwd=build,env=environment,capture_output=True,text=True,timeout=120)
  if linked.returncode:raise RuntimeError(linked.stdout+linked.stderr)
  result=subprocess.run([str(executable),*args],env=environment,capture_output=True,text=True,timeout=120)
  report['runs'].append({'name':name,'exit':result.returncode,'output':result.stdout+result.stderr,'sha256':sha(executable)})
  if result.returncode:raise RuntimeError('Runtime failed: '+name)
 report['native_dependencies_sha256']=watched
 report['native_dependencies_unchanged']=all(sha(Path(path))==value for path,value in watched.items())
 report['staged_inputs_unchanged']=all(sha(stage/name)==value for name,value in inputs.items())
 report['passed']=report['native_dependencies_unchanged'] and report['staged_inputs_unchanged']
except Exception as error:report['error']=str(error)
(stage/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'passed':report['passed'],'stage':str(stage),'error':report.get('error')}),flush=True)
raise SystemExit(0 if report['passed'] else 1)
'''


def digest(data): return hashlib.sha256(data).hexdigest()
def remote(command, **kwargs): return subprocess.run(['bash', str(ROOT/'tools/haiku.sh'), command], cwd=ROOT, **kwargs)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,default=ROOT/'.cache/WebKit')
    parser.add_argument('--fixtures',type=Path,required=True)
    parser.add_argument('--bundle',required=True)
    args=parser.parse_args()
    output=ROOT/'.vm'/('native-crx-'+secrets.token_hex(12));output.mkdir()
    subprocess.run(['python3',str(ROOT/'tools/test-engine-extension-archives-fixtures.py'),str(output/'zip-fixtures')],check=True)
    files={};original={}
    def add(target,path):
        data=path.read_bytes();files[target]=data;original[str(path.resolve())]=digest(data)
    prefix='Source/WebKit/UIProcess/Extensions/haiku/'
    for name in ['WebExtensionCRXHaiku.cpp','WebExtensionCRXHaiku.h','WebExtensionArchiveHaiku.cpp','WebExtensionArchiveHaiku.h']:
        add(name,args.source_root/prefix/name)
    add('WebExtensionResourcePathsHaiku.h',ROOT/'.cache/WebKit'/prefix/'WebExtensionResourcePathsHaiku.h')
    for name in ['EngineExtensionCRXTests.cpp','EngineExtensionCRXArchiveTests.cpp','EngineExtensionArchiveTests.cpp']:add(name,ROOT/'tests'/name)
    add('vendor/nlohmann/json.hpp',ROOT/'vendor/nlohmann/json.hpp')
    for directory,label in [(args.fixtures,'crx-fixtures'),(output/'zip-fixtures','zip-fixtures')]:
        for path in sorted(directory.iterdir()):
            if path.is_file():add(label+'/'+path.name,path)
    files['run.py']=NATIVE.encode();files['inputs.json']=json.dumps({name:digest(data) for name,data in files.items()}).encode()
    archive=io.BytesIO()
    with tarfile.open(fileobj=archive,mode='w:gz') as tar:
        for name,data in files.items():
            item=tarfile.TarInfo(name);item.size=len(data);item.mode=0o600;tar.addfile(item,io.BytesIO(data))
    stage=remote('mktemp -d /SummitExtensions/summit/crx-tests.XXXXXXXX',capture_output=True,text=True,check=True).stdout.strip()
    assert stage.startswith('/SummitExtensions/summit/crx-tests.') and stage.rsplit('.',1)[-1].isalnum()
    remote('tar -xzf - -C '+shlex.quote(stage),input=archive.getvalue(),check=True)
    (output/'stage.json').write_text(json.dumps({'stage':stage,'sources':original},indent=2)+'\n')
    print(json.dumps({'stage':stage,'output':str(output)}),flush=True)
    with (output/'native.log').open('w') as log:result=remote(shlex.join(['python3.10',stage+'/run.py',stage,args.bundle]),stdout=log,stderr=subprocess.STDOUT)
    fetched=remote('cat '+shlex.quote(stage+'/result.json'),capture_output=True,text=True,check=True)
    report={'native':json.loads(fetched.stdout),'source_sha256':original,'sources_unchanged':all(digest(Path(path).read_bytes())==value for path,value in original.items())}
    report['passed']=result.returncode==0 and report['native']['passed'] and report['sources_unchanged']
    (output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'passed':report['passed'],'result':str(output/'result.json'),'error':report['native'].get('error')}),flush=True)
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())

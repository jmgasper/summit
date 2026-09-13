#!/usr/bin/env python3
"""Check native BApplication constructor error reporting without registering an app."""
import hashlib
import io
import json
import pathlib
import re
import shlex
import subprocess
import tarfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tests/NativeApplicationInitTests.cpp'


def native(command, **kwargs):
    return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command],
                          cwd=ROOT, check=True, **kwargs)


def main():
    stage = native('mktemp -d /boot/home/summit/application-init-inputs.XXXXXXXX',
                   capture_output=True, text=True).stdout.strip()
    if not re.fullmatch(r'/boot/home/summit/application-init-inputs\.[A-Za-z0-9]+', stage):
        raise SystemExit('Unexpected native staging path: ' + stage)
    source_bytes = SOURCE.read_bytes()
    expected = hashlib.sha256(source_bytes).hexdigest()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w') as output:
        entry = tarfile.TarInfo(SOURCE.name)
        entry.size = len(source_bytes)
        output.addfile(entry, io.BytesIO(source_bytes))
    native('tar -xf - -C ' + shlex.quote(stage), input=archive.getvalue(), capture_output=True)
    script = '''import hashlib,json,pathlib,subprocess,sys
stage=pathlib.Path(sys.argv[1]); expected=sys.argv[2]
source=stage/'NativeApplicationInitTests.cpp'
assert hashlib.sha256(source.read_bytes()).hexdigest()==expected
command=['c++','-std=c++23','-Wall','-Wextra',str(source),'-lbe','-o',str(stage/'run')]
compiled=subprocess.run(command,capture_output=True,text=True,timeout=60)
result={'stage':str(stage),'source_sha256':expected,'command':command,'compile_exit':compiled.returncode,'compile_stdout':compiled.stdout,'compile_stderr':compiled.stderr,'passed':False}
if not compiled.returncode:
    for mode in ('baseline','checked'):
        test=subprocess.run([str(stage/'run'),mode],capture_output=True,text=True,timeout=10)
        result[mode]={'exit':test.returncode,'stdout':test.stdout,'stderr':test.stderr}
    baseline=result['baseline']; checked=result['checked']
    result['checks']={
        'baseline_reached_constructor':'BASELINE before constructor' in baseline['stdout'],
        'baseline_exits_without_returning':'BASELINE unexpectedly returned' not in baseline['stdout'] and baseline['exit']==0,
        'checked_returns_native_error':'CHECKED reported native failure' in checked['stdout'],
        'checked_propagates_failure_exit':checked['exit']==1,
        'inputs_unchanged':hashlib.sha256(source.read_bytes()).hexdigest()==expected}
    result['passed']=all(result['checks'].values())
(stage/'result.json').write_text(json.dumps(result,indent=2)+'\\n')
print(json.dumps(result,indent=2))
'''
    command = 'python3.10 -c ' + shlex.quote(script) + ' ' + shlex.quote(stage) + ' ' + expected
    output = native(command, capture_output=True, text=True)
    result = json.loads(output.stdout)
    evidence = ROOT / '.vm' / pathlib.Path(stage).name
    evidence.mkdir(parents=True, exist_ok=True)
    (evidence / SOURCE.name).write_bytes(source_bytes)
    (evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    if not result['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()

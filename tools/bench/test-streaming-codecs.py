#!/usr/bin/env python3
"""Decode parser fixtures with native Media Kit and check exact frames and PCM.

Pass directories produced by test-webm-parser.py/test-mpeg-audio-parser.py.
Directories without a result.json may contain additional valid .webm/.mp4
fixtures. Malformed-input fixtures are deliberately excluded from decoding.
"""
import argparse
import io
import json
import pathlib
import shlex
import subprocess
import tarfile
import time

import guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixtures', type=pathlib.Path, nargs='+', required=True)
    parser.add_argument('--engine', type=pathlib.Path, default=guest.ROOT / '.cache/WebKit')
    parser.add_argument('--output', type=pathlib.Path, default=guest.ROOT / '.vm/streaming-codecs')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixtures = []
    for directory in args.fixtures:
        manifest = directory / 'result.json'
        if manifest.exists():
            fixtures += [directory / item['file'] for item in json.loads(manifest.read_text())['fixtures']]
        else:
            fixtures += sorted(directory.glob('*.webm')) + sorted(directory.glob('*.mp4'))
    if not fixtures:
        raise SystemExit('No media fixtures found')
    remote = guest.GUEST_ROOT + '/streaming-codecs-' + str(time.time_ns())
    port = args.engine / 'Source/WebCore/platform/graphics/haiku'
    third_party = args.engine / 'Source/ThirdParty/libwebrtc/Source/third_party'
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w') as tar:
        tar.add(guest.ROOT / 'tests/ModernStreamingCodecTests.cpp', arcname='probe.cpp')
        tar.add(guest.ROOT / 'vendor/nlohmann/json.hpp', arcname='nlohmann/json.hpp')
        for name in ('WebMParserHaiku', 'MPEGAudioParserHaiku', 'FragmentedMP4ParserHaiku'):
            for extension in ('cpp', 'h'):
                tar.add(port / (name + '.' + extension), arcname=name + '.' + extension)
        tar.add(port / 'CencCryptoHaiku.h', arcname='CencCryptoHaiku.h')
        tar.add(port / 'VideoPresentationTimelineHaiku.h', arcname='VideoPresentationTimelineHaiku.h')
        tar.add(third_party / 'libwebm/webm_parser', arcname='webm')
        tar.add(third_party / 'opus/src/include', arcname='opus')
        for index, fixture in enumerate(fixtures):
            tar.add(fixture, arcname='media/' + str(index) + '-' + fixture.name)
    guest.ssh('mkdir -p ' + shlex.quote(remote) + ' && tar -xf - -C ' + shlex.quote(remote), input_bytes=archive.getvalue())
    compile_script = '''import glob,subprocess,sys
remote=sys.argv[1]
sources=[p for p in sorted(glob.glob(remote+'/webm/src/*.cc')) if not p.endswith('_test.cc')]
command=['g++','-std=c++23','-O1','-DWEBRTC_WEBKIT_BUILD=1','-Wno-multichar',
 '-I'+remote,'-I'+remote+'/webm','-I'+remote+'/webm/include','-I'+remote+'/opus',
 remote+'/probe.cpp',remote+'/WebMParserHaiku.cpp',remote+'/MPEGAudioParserHaiku.cpp',
 remote+'/FragmentedMP4ParserHaiku.cpp',*sources,'-Wl,-l:libopus.so.0','-lbe','-lmedia','-o',remote+'/probe']
with open(remote+'/compile.log','wb') as log:
 result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
sys.exit(result.returncode)
'''
    compilation = guest.ssh('python3.10 - ' + shlex.quote(remote), input_bytes=compile_script.encode(), timeout=600, check=False)
    guest.fetch_file(remote + '/compile.log', args.output / 'compile.log')
    if compilation.returncode:
        raise guest.GuestError('codec probe compilation failed; see compile.log')
    ctl = guest.ensure_ctl()
    watch = guest.CrashWatch()
    launch = '''import glob,subprocess,sys
remote=sys.argv[1]
process=subprocess.Popen([remote+'/probe']+sorted(glob.glob(remote+'/media/*')),stdin=subprocess.DEVNULL,
 stdout=open(remote+'/native.log','wb'),stderr=subprocess.STDOUT,start_new_session=True)
print(process.pid)
'''
    launched = guest.ssh('python3.10 - ' + shlex.quote(remote), input_bytes=launch.encode())
    team = int(launched.stdout.strip().splitlines()[-1])
    result = {'team': team, 'remote': remote, 'fixtures': [str(path) for path in fixtures]}
    (args.output / 'active.json').write_text(json.dumps(result, indent=2) + '\n')
    try:
        deadline = time.monotonic() + 300
        while guest.alive(team) and time.monotonic() < deadline:
            time.sleep(1)
        if guest.alive(team):
            result['timedOut'] = True
    finally:
        if guest.alive(team):
            result['cleanup'] = guest.terminate(ctl, team)
        guest.fetch_file(remote + '/native.log', args.output / 'native.log')
        result['members'] = guest.members(ctl, team)
        result['crashes'] = watch.poll()
    log = (args.output / 'native.log').read_text()
    decodes = []
    for line in log.splitlines():
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        entry.pop('timestamps', None)
        entry.pop('presentationTimes', None)
        decodes.append(entry)
    result['decodes'] = decodes
    result['passed'] = ('StreamingCodec_RESULT PASS' in log and all(item.get('ok') for item in decodes)
                        and bool(decodes) and not result.get('timedOut') and not result['members']
                        and not result['crashes']['newReports'])
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Native streaming codecs:', 'PASS' if result['passed'] else 'FAIL', len(fixtures), 'files,', len(decodes), 'decode passes')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

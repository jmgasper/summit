#!/usr/bin/env python3
"""Check production readback geometry on Haiku using real WebCore rectangle types.

Extracts both pure helpers verbatim from the prepared engine, compiles with the
native build's headers and links a frozen bundle. No GPU or browser is launched;
this checks rectangle coverage and bounds, not driver readback or presentation.
"""
import argparse
import hashlib
import io
import json
import pathlib
import shlex
import sys
import tarfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / 'bench'))
import guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True)
    parser.add_argument('--build', default='/boot/home/summit-webkit-extensions/WebKitBuild/SkiaCGMiPGO')
    parser.add_argument('--source', type=pathlib.Path, default=guest.ROOT / '.cache/WebKit')
    parser.add_argument('--label', default='')
    args = parser.parse_args()
    run_id = time.strftime('readback-rects-%Y%m%d-%H%M%S') + ('-' + args.label if args.label else '')
    output = guest.ROOT / '.vm' / run_id
    output.mkdir(parents=True)
    remote = guest.GUEST_ROOT + '/' + run_id
    source = (args.source / 'Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/AcceleratedSurface.cpp').read_text()
    pieces = []
    for name in ('mergedReadbackRectsHaiku', 'widenedReadbackRectsHaiku'):
        start = source.index('static Vector<IntRect, 1> ' + name + '(')
        end = source.index('\n}\n', start) + 3
        pieces.append(source[start:end])
    files = {
        'ReadbackRectsUnderTest.h': '\n'.join(pieces).encode(),
        'EngineReadbackRectsTests.cpp': (guest.ROOT / 'tests/EngineReadbackRectsTests.cpp').read_bytes(),
    }
    for name, data in files.items():
        (output / name).write_bytes(data)
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w') as tar:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    guest.ssh(f'mkdir -p {shlex.quote(remote)} && tar -xf - -C {shlex.quote(remote)}', input_bytes=archive.getvalue())
    ninja = guest.ssh('cat ' + shlex.quote(args.build + '/build.ninja'), timeout=60).stdout
    marker = 'build Source/WebKit/CMakeFiles/WebKit.dir/WebProcess/WebPage/CoordinatedGraphics/AcceleratedSurface.cpp.o:'
    block = ninja[ninja.index(marker):].split('\n\n', 1)[0]
    fields = dict(line.strip().split(' = ', 1) for line in block.splitlines()[1:] if ' = ' in line)
    flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))
    # A fixture has no production training profile. Keep the native target's
    # configuration/ABI, but remove profile instrumentation and the PCH.
    selected = []
    iterator = iter(flags)
    for flag in iterator:
        if flag == '-include':
            next(iterator)
        elif not flag.startswith(('-fprofile-', '-Werror', '-O')):
            selected.append(flag)
    selected += ['-O2', '-I' + remote]
    libraries = args.bundle + '/lib'
    command = ['c++', *selected, remote + '/EngineReadbackRectsTests.cpp',
               '-L' + libraries, '-lWebKit', '-lJavaScriptCore', '-lbe', '-lnetwork',
               '-Wl,-rpath,' + libraries, '-o', remote + '/run']
    compile_result = guest.ssh('cd ' + shlex.quote(args.build) + ' && ' + shlex.join(command), timeout=300, check=False)
    (output / 'compile.log').write_text(compile_result.stdout + compile_result.stderr)
    result = {'scope': 'native production readback rectangle helpers; no GPU presentation',
              'bundle': args.bundle, 'build': args.build,
              'sourceSHA256': hashlib.sha256(source.encode()).hexdigest(),
              'inputs': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()},
              'command': command, 'compileExit': compile_result.returncode, 'passed': False}
    if compile_result.returncode == 0:
        watch = guest.CrashWatch()
        run = guest.ssh('env LIBRARY_PATH=' + shlex.quote(libraries + ':/boot/system/lib')
                        + ' ' + shlex.quote(remote + '/run'), timeout=120, check=False)
        result.update(exit=run.returncode, output=run.stdout + run.stderr, crashes=watch.poll())
        result['passed'] = (run.returncode == 0 and 'READBACK_RECTS_RESULT PASS' in run.stdout
                            and not result['crashes']['newReports'] and not result['crashes']['syslogEvents'])
        print(run.stdout + run.stderr)
    else:
        print(compile_result.stdout + compile_result.stderr)
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Artifacts:', output)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

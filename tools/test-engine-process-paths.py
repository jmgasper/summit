#!/usr/bin/env python3
"""Compile and exercise Haiku's production auxiliary-executable resolver."""
import os
import pathlib
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = pathlib.Path('/boot/home/summit-webkit/WebKitBuild/Release')
OUTPUT = ROOT / 'build-process-tests'
OBJECT = 'Source/WTF/wtf/CMakeFiles/WTF.dir/haiku/RunLoopHaiku.cpp.o'
fields = {}
with (BUILD / 'build.ninja').open() as stream:
    for line in stream:
        if line.startswith('build ' + OBJECT + ':'):
            for line in stream:
                if not line.strip(): break
                key, _, value = line.strip().partition(' = ')
                fields[key] = value
            break
flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))

with tempfile.TemporaryDirectory(prefix='summit-process-paths-') as folder:
    base = pathlib.Path(folder).resolve()
    app = base / 'application space 雪'
    fallback = base / 'fallback'
    override = base / 'override'
    names = ['SummitWebProcessTest', 'SummitNetworkProcessTest']
    for directory in [app, fallback, override]:
        directory.mkdir()
        for name in names:
            path = directory / name
            path.write_text('#!/bin/sh\nexit 0\n')
            path.chmod(0o700)
    flags += ['-I' + str(OUTPUT), '-DLIBEXECDIR="' + str(fallback) + '"',
              '-DWEBPROCESSNAME="' + names[0] + '"', '-DNETWORKPROCESSNAME="' + names[1] + '"']
    executable = app / 'path-test'
    subprocess.run(['c++', *flags, str(OUTPUT / 'ProcessExecutablePathHaiku.cpp'),
                    str(ROOT / 'tests/EngineProcessPathTests.cpp'), '-L' + str(BUILD / 'lib'),
                    '-lJavaScriptCore', '-lbe', '-lnetwork', '-Wl,-rpath,' + str(BUILD / 'lib'),
                    '-o', str(executable)], cwd=BUILD, check=True)

    def check(label, directory, search=None, network=None):
        env = dict(os.environ)
        env.pop('WEBKIT_EXEC_PATH', None)
        if search is not None: env['WEBKIT_EXEC_PATH'] = search
        expected_network = str(directory / names[1]) if network is None else network
        print(label, flush=True)
        subprocess.run([str(executable), str(directory / names[0]), expected_network],
                       env=env, check=True, timeout=10)

    check('Find helpers beside the actual native executable', app)
    check('Explicit absolute executable directory takes precedence', override, str(override))
    check('Relative executable directory is rejected', app, 'override')
    for name in names: (override / name).chmod(0o600)
    check('Non-executable override files are rejected', app, str(override))
    for name in names: (app / name).unlink()
    check('Use the configured installation directory', fallback)
    (fallback / names[1]).unlink()
    check('Missing helper produces an empty path', fallback, network='')
print('12 native executable-path checks passed.', flush=True)

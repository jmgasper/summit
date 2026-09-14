#!/usr/bin/env python3
"""Prepare the locked libzip package in a private native Haiku prefix."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PREFIX = Path('/boot/home/summit-deps/libzip-1.11.4')
CACHE = ROOT / 'extension-archive-dependencies'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    lock_path = ROOT / 'engine/libzip.lock.json'
    lock = json.loads(lock_path.read_text())
    if lock['version'] != '1.11.4':
        raise SystemExit('Update the private prefix and linker names for a new libzip version.')
    CACHE.mkdir(parents=True, exist_ok=True)
    PREFIX.mkdir(parents=True, exist_ok=True)
    for name, expected in lock['packages'].items():
        package = CACHE / name
        if not package.exists():
            with tempfile.NamedTemporaryFile(dir=CACHE, suffix='.download', delete=False) as stream:
                temporary = Path(stream.name)
            try:
                urllib.request.urlretrieve(lock['package_url_base'] + name, temporary)
                if digest(temporary) != expected:
                    raise RuntimeError('Downloaded package hash mismatch: ' + name)
                temporary.replace(package)
            finally:
                temporary.unlink(missing_ok=True)
        if digest(package) != expected:
            raise RuntimeError('Cached package hash mismatch: ' + name)

    with tempfile.TemporaryDirectory(prefix='libzip-extract-', dir=PREFIX.parent) as directory:
        stage = Path(directory)
        for name in lock['packages']:
            subprocess.run(['package', 'extract', '-C', str(stage), str(CACHE / name)], check=True)
        for name, expected in lock['files'].items():
            if digest(stage / name) != expected:
                raise RuntimeError('Extracted dependency hash mismatch: ' + name)
        files = {'lib/libzip.so.5.5': 'lib/libzip.so.5.5',
                 'develop/headers/zip.h': 'include/zip.h',
                 'develop/headers/zipconf.h': 'include/zipconf.h'}
        for source, target in files.items():
            destination = PREFIX / target
            destination.parent.mkdir(parents=True, exist_ok=True)
            if not destination.exists() or digest(destination) != lock['files'][source]:
                shutil.copy2(stage / source, destination)
        licenses = stage / 'data/licenses'
        if licenses.is_dir():
            shutil.copytree(licenses, PREFIX / 'share/licenses', dirs_exist_ok=True)

    for name in ('libzip.so', 'libzip.so.5'):
        link = PREFIX / 'lib' / name
        if link.is_symlink() and link.readlink() == Path('libzip.so.5.5'):
            continue
        if link.exists() or link.is_symlink():
            raise RuntimeError('Unexpected existing dependency link: ' + str(link))
        link.symlink_to('libzip.so.5.5')
    pkgconfig = PREFIX / 'lib/pkgconfig'
    pkgconfig.mkdir(exist_ok=True)
    (pkgconfig / 'libzip.pc').write_text('''prefix=${pcfiledir}/../..
libdir=${prefix}/lib
includedir=${prefix}/include

Name: libzip
Description: Library for handling zip archives
Version: 1.11.4
Libs: -L${libdir} -lzip
Libs.private: -lssl -lcrypto -lz
Cflags: -I${includedir}
''')
    report = {'lock_sha256': digest(lock_path), 'prefix': str(PREFIX),
              'packages': lock['packages'],
              'files': {target: digest(PREFIX / target) for target in files.values()}}
    (PREFIX / 'summit-dependency-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Verified private libzip ' + lock['version'] + ': ' + str(PREFIX))


if __name__ == '__main__':
    main()

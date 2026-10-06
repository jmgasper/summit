#!/usr/bin/env python3
"""Build checksum-pinned image codecs in a private prefix, without OS changes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, **kwargs)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=ROOT / '.cache/image-codecs')
    parser.add_argument('--prefix', type=Path)
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--download-only', action='store_true')
    args = parser.parse_args()
    lockfile = ROOT / 'engine/image-codecs.lock.json'
    lock = json.loads(lockfile.read_text())
    prefix = args.prefix or Path('/boot/home/summit-deps/image-codecs-' + lock['revision'])
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    for name, item in lock['sources'].items():
        archive = cache / item['archive']
        if not archive.exists():
            temporary = archive.with_suffix('.download')
            try:
                urllib.request.urlretrieve(item['url'], temporary)
                if digest(temporary) != item['sha256']:
                    raise RuntimeError('Downloaded checksum mismatch: ' + name)
                temporary.replace(archive)
            finally:
                temporary.unlink(missing_ok=True)
        if digest(archive) != item['sha256']:
            raise RuntimeError('Cached checksum mismatch: ' + name)
    for source in lock['sources'].values():
        for patch in source.get('patches', []):
            if digest(ROOT / patch['path']) != patch['sha256']:
                raise RuntimeError('Local dependency patch checksum mismatch: ' + patch['path'])
    if args.download_only:
        print('Verified all image codec sources and patches.')
        return
    if not prefix.is_absolute():
        raise SystemExit('--prefix must be absolute')
    work = prefix.parent / (prefix.name + '-build')
    sources = work / 'sources'
    sources.mkdir(parents=True, exist_ok=True)
    directories = {}
    for name, item in lock['sources'].items():
        target = sources / (name + '-' + item['version'])
        if not target.exists():
            # Verified archives still must not write outside the source tree.
            with tarfile.open(cache / item['archive']) as archive:
                members = archive.getmembers()
                roots = {Path(m.name).parts[0] for m in members if m.name}
                if len(roots) != 1:
                    raise RuntimeError('Archive needs one root: ' + name)
                for member in members:
                    if member.name.startswith('/') or '..' in Path(member.name).parts or member.isdev():
                        raise RuntimeError('Unsafe archive member: ' + member.name)
                    if (member.issym() or member.islnk()) and (member.linkname.startswith('/') or '..' in Path(member.linkname).parts):
                        raise RuntimeError('Unsafe archive link: ' + member.name)
                archive.extractall(sources)
                extracted = sources / next(iter(roots))
                if extracted != target:
                    extracted.rename(target)
        directories[name] = target
    # Debian maintains portability/UB fixes and the CMake build for the
    # Microsoft reference codec. Pin that complete patch set as well.
    patches = directories['jxrlib-patches'] / 'patches'
    for name in (patches / 'series').read_text().splitlines():
        if not name or name.startswith('#'):
            continue
        patch = patches / name
        stamp = directories['jxrlib'] / ('.summit-patch-' + digest(patch))
        if not stamp.exists():
            run(['patch', '-p1', '-i', patch], cwd=directories['jxrlib'])
            stamp.touch()
    for name, item in lock['sources'].items():
        for item in item.get('patches', []):
            patch = ROOT / item['path']
            if digest(patch) != item['sha256']:
                raise RuntimeError('Local dependency patch checksum mismatch: ' + str(patch))
            stamp = directories[name] / ('.summit-patch-' + item['sha256'])
            if not stamp.exists():
                run(['patch', '-p1', '-i', patch], cwd=directories[name])
                stamp.touch()
    for name in ('highway', 'brotli'):
        target = directories['libjxl'] / 'third_party' / name
        if target.is_dir() and not any(target.iterdir()):
            target.rmdir()
        if not target.exists():
            target.symlink_to(directories[name], target_is_directory=True)
    env = dict(os.environ)
    env['PKG_CONFIG_PATH'] = str(prefix / 'lib/pkgconfig') + (':' + env['PKG_CONFIG_PATH'] if env.get('PKG_CONFIG_PATH') else '')
    common = ['-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=ON', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
              '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_PREFIX_PATH=' + str(prefix),
              '-DBUILD_TESTING=OFF']
    options = {
        'jxrlib': [],
        'libde265': ['-DENABLE_DECODER=OFF', '-DENABLE_ENCODER=OFF', '-DENABLE_SDL=OFF'],
        'libheif': ['-DENABLE_PLUGIN_LOADING=OFF', '-DWITH_LIBDE265=ON', '-DWITH_LIBDE265_PLUGIN=OFF',
                    '-DWITH_EXAMPLES=OFF', '-DWITH_GDK_PIXBUF=OFF', '-DBUILD_DOCUMENTATION=OFF', '-DWITH_LIBSHARPYUV=OFF']
            + ['-DWITH_' + codec + '=OFF' for codec in ('X265', 'KVAZAAR', 'UVG266', 'VVDEC', 'VVENC', 'X264',
                'OpenH264_DECODER', 'DAV1D', 'AOM_DECODER', 'AOM_ENCODER', 'SvtEnc', 'RAV1E', 'JPEG_DECODER',
                'JPEG_ENCODER', 'OpenJPEG_ENCODER', 'OpenJPEG_DECODER', 'FFMPEG_DECODER', 'OPENJPH_ENCODER')],
        'libjxl': ['-DJPEGXL_ENABLE_' + feature + '=OFF' for feature in ('TOOLS', 'DEVTOOLS', 'DOXYGEN', 'MANPAGES',
            'BENCHMARK', 'EXAMPLES', 'JNI', 'SJPEG', 'OPENEXR', 'SKCMS', 'VIEWERS', 'TCMALLOC', 'PLUGINS')]
            + ['-DJPEGXL_FORCE_SYSTEM_LCMS2=ON'],
    }
    prefix.mkdir(parents=True, exist_ok=True)
    for name in ('libde265', 'libheif', 'libjxl', 'jxrlib'):
        build = work / (name + '-build')
        run(['cmake', '-G', 'Ninja', '-S', directories[name], '-B', build, *common, *options[name]], env=env)
        run(['cmake', '--build', build, '-j', max(1, min(args.jobs, 32))], env=env)
        run(['cmake', '--install', build], env=env)
        license_dir = prefix / 'share/licenses' / name
        license_dir.mkdir(parents=True, exist_ok=True)
        for source in directories[name].glob('*'):
            if source.is_file() and source.name.startswith(('LICENSE', 'COPYING', 'PATENTS', 'AUTHORS')):
                shutil.copy2(source, license_dir / source.name)
    shutil.copy2(directories['jxrlib-patches'] / 'copyright', prefix / 'share/licenses/jxrlib/Debian-copyright')
    for name in ('highway', 'brotli'):
        license_dir = prefix / 'share/licenses' / name
        license_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(directories[name] / 'LICENSE', license_dir / 'LICENSE')
    report = {'lock_sha256': digest(lockfile), 'sources': lock['sources'],
              'files': {str(p.relative_to(prefix)): digest(p) for p in sorted(prefix.rglob('*'))
                        if p.is_file() and not p.is_symlink() and p.name != 'summit-dependency-manifest.json'}}
    (prefix / 'summit-dependency-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Verified private image codecs: ' + str(prefix), flush=True)

if __name__ == '__main__':
    main()

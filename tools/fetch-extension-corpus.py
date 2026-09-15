#!/usr/bin/env python3
"""Fetch pinned upstream extension packages and inspect them without executing code."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.cache/extension-corpus/packages'
LOCK = ROOT / 'compatibility/extensions.lock.json'


def inspect(path):
    with zipfile.ZipFile(path) as archive:
        files = archive.infolist()
        if len(files) > 20000 or sum(item.file_size for item in files) > 256 * 1024 * 1024:
            raise RuntimeError('Package exceeds the corpus inspection bounds: ' + path.name)
        names = [item.filename for item in files]
        if len(names) != len(set(names)):
            raise RuntimeError('Ambiguous duplicate archive members: ' + path.name)
        for item in files:
            name = PurePosixPath(item.filename)
            if name.is_absolute() or '..' in name.parts or '\\' in item.filename or item.file_size > 32 * 1024 * 1024:
                raise RuntimeError('Unexpected archive member: ' + item.filename)
        manifests = sorted((name for name in names if PurePosixPath(name).name == 'manifest.json'),
                           key=lambda name: (len(PurePosixPath(name).parts), name))
        if not manifests or (len(manifests) > 1 and len(PurePosixPath(manifests[0]).parts) == len(PurePosixPath(manifests[1]).parts)):
            raise RuntimeError('No unique package manifest: ' + path.name)
        manifest_path = manifests[0]
        manifest = json.loads(archive.read(manifest_path))
        prefix = str(PurePosixPath(manifest_path).parent)
        prefix = '' if prefix == '.' else prefix + '/'
        # These are textual mentions, not an AST/call-graph analysis or proof
        # that an API is required or supported at runtime.
        references = {}
        for item in files:
            if not item.filename.startswith(prefix) or not item.filename.endswith('.js'):
                continue
            script = archive.read(item).decode('utf-8', errors='replace')
            for namespace, member in set(re.findall(r'\b(?:browser|chrome)\.([A-Za-z_$][\w$]*)\.([A-Za-z_$][\w$]*)', script)):
                references.setdefault(namespace + '.' + member, []).append(item.filename)
        licenses = [name for name in names if re.search(r'(^|/)(license|copying)(\.[^/]*)?$', name, re.I)]
        return {
            'manifest_path': manifest_path,
            'manifest': manifest,
            'archive_files': len(files),
            'uncompressed_bytes': sum(item.file_size for item in files),
            'license_files': {name: hashlib.sha256(archive.read(name)).hexdigest() for name in licenses},
            'textual_api_references': {key: sorted(value) for key, value in sorted(references.items())},
            'signature_verified': False,
            'native_runtime_tested': False,
        }


def fetch(package):
    name = package['name']
    if Path(name).name != name or not re.fullmatch(r'[a-zA-Z0-9_.-]+', name):
        raise RuntimeError('Invalid pinned package filename')
    if not package['url'].startswith('https://github.com/'):
        raise RuntimeError('Corpus package must have an official HTTPS release URL')
    if not re.fullmatch(r'[0-9a-f]{64}', package['sha256']) or not 0 < package['size'] < 32 * 1024 * 1024:
        raise RuntimeError('Invalid pinned package size or digest')
    path = CACHE / package['sha256'] / name
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        request = urllib.request.Request(package['url'], headers={'User-Agent': 'Summit-compatibility-audit'})
        with urllib.request.urlopen(request, timeout=45) as response:
            data = response.read(package['size'] + 1)
        if len(data) != package['size'] or hashlib.sha256(data).hexdigest() != package['sha256']:
            raise RuntimeError('Downloaded package does not match its pin: ' + name)
        # Never overwrite an existing corpus artifact.
        with path.open('xb') as output:
            output.write(data)
    data = path.read_bytes()
    if len(data) != package['size'] or hashlib.sha256(data).hexdigest() != package['sha256']:
        raise RuntimeError('Cached package does not match its pin: ' + name)
    result = {**package, 'path': str(path), **inspect(path)}
    print(json.dumps({'package': name, 'sha256': package['sha256'],
                      'manifest_version': result['manifest']['manifest_version'],
                      'manifest_path': result['manifest_path'], 'files': result['archive_files']}), flush=True)
    return result


def main():
    CACHE.mkdir(parents=True, exist_ok=True)
    original = LOCK.read_bytes()
    lock = json.loads(original)
    if lock['schema'] != 1 or not lock['packages']:
        raise RuntimeError('Invalid corpus lock')
    if len({package['name'] for package in lock['packages']}) != len(lock['packages']):
        raise RuntimeError('Duplicate pinned package filename')
    with ThreadPoolExecutor(max_workers=2) as workers:
        packages = list(workers.map(fetch, lock['packages']))
    if LOCK.read_bytes() != original:
        raise RuntimeError('Corpus pins changed during download')
    report = {'scope': 'hash-verified upstream packages and read-only archive inspection; no signature authentication or native execution',
              'lock_sha256': hashlib.sha256(original).hexdigest(), 'packages': packages}
    output = CACHE.parent / 'inspection.json'
    temporary = output.with_suffix('.tmp')
    temporary.write_text(json.dumps(report, indent=2) + '\n')
    temporary.replace(output)
    print(json.dumps({'inspection': str(output), 'packages': len(packages), 'native_runtime_tested': False}), flush=True)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Copy a completed frozen native bundle and its matching WebKit sources to the host.

This tool never prepares, synchronizes, configures, builds, or launches the engine.
It publishes a new artifact directory only after all validation has passed.
"""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
import pathlib
import posixpath
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ('CMakeLists.txt', 'Configurations', 'Source', 'Tools')
HASH = re.compile(r'[0-9a-f]{64}\Z')
LIBRARY = re.compile(r'lib/(libWebKit|libJavaScriptCore|libicudata|libicui18n|libicuuc|libzip)\.so(?:\.[0-9]+)*\Z')
LIBRARIES = {'libWebKit', 'libJavaScriptCore', 'libicudata', 'libicui18n', 'libicuuc'}
HEADERS = {
    'WebKitView.h': 'UIProcess/API/haiku/WebKitView.h',
    'WebKitContext.h': 'UIProcess/API/haiku/WebKitContext.h',
    'WebKitExtensionPermission.h': 'UIProcess/API/haiku/WebKitExtensionPermission.h',
    'WebKitInfo.h': 'UIProcess/API/haiku/WebKitInfo.h',
    'WKBase.h': 'Shared/API/c/WKBase.h',
    'WKDeclarationSpecifiers.h': 'Shared/API/c/WKDeclarationSpecifiers.h',
    'WKBaseHaiku.h': 'Shared/API/c/haiku/WKBaseHaiku.h',
}
SUPPORT = (
    'engine/sources.lock.json', 'engine/icu.lock.json', 'engine/README.md',
    'tools/haiku.sh', 'tools/prepare-webkit.py', 'tools/build-webkit-in-vm.sh',
    'tools/sync-webkit-sources.py', 'tools/build-icu-in-vm.sh',
    'tools/prepare-extension-deps.py', 'engine/libzip.lock.json',
    'tools/build-modern-browser-in-vm.sh', 'tools/copy-modern-browser-bundle.py',
    'docs/modern-bundle-copy.md',
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def file_hash(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()


def json_bytes(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'Duplicate JSON key: ' + key)
            result[key] = value
        return result
    require(len(data) <= 16 * 1024 * 1024, 'Manifest exceeds the size limit')
    return json.loads(data, object_pairs_hook=unique)


def relative(name):
    require(isinstance(name, str) and name and '\\' not in name
            and not any(ord(character) < 32 or ord(character) == 127 for character in name),
            f'Invalid relative path: {name!r}')
    path = pathlib.PurePosixPath(name)
    require(not path.is_absolute() and str(path) == name
            and all(part not in ('.', '..') for part in path.parts),
            f'Unsafe relative path: {name!r}')
    return name


def hashes(value, label, absolute=False, extra_roots=()):
    require(isinstance(value, dict) and value, f'Missing {label} hash map')
    for name, expected in value.items():
        if absolute:
            require(isinstance(name, str) and name.startswith(('/boot/home/', *extra_roots))
                    and str(pathlib.PurePosixPath(name)) == name
                    and '..' not in pathlib.PurePosixPath(name).parts,
                    f'Invalid original input path: {name!r}')
        else:
            relative(name)
        require(isinstance(expected, str) and HASH.fullmatch(expected),
                f'Invalid {label} digest for {name!r}')
    return value


def validate_report(report, target):
    require(isinstance(report, dict), 'Bundle report must be an object')
    expected_keys = {'bundle', 'created_at', 'kind', 'inputs', 'configuration', 'compiler',
                     'compile_commands', 'original_sha256', 'bundled_sha256', 'needed',
                     'runtime_search_paths', 'symlinks'}
    require(set(report) == expected_keys, 'Unknown or incomplete bundle manifest format')
    require(report['kind'] == 'modern-native-' + target, 'Manifest has the wrong bundle kind')
    native = report['bundle']
    require(isinstance(native, str) and re.fullmatch(
        '/boot/home/summit/build-modern-' + target + r'/bundle-[A-Za-z0-9_-]+', native),
        'Unexpected native bundle path')
    inputs = report['inputs']
    require(isinstance(inputs, dict), 'Staged inputs must be an object')
    legacy = 'engine_variant' not in inputs
    variant = inputs.get('engine_variant', 'modern')
    require(variant in ('modern', 'modern-extensions'), 'Unknown engine variant')
    extensions = variant == 'modern-extensions'
    input_keys = {'engine', 'icu', 'public_headers', 'target', 'sha256'}
    if not legacy:
        input_keys.add('engine_variant')
    if extensions:
        input_keys.add('libzip')
    require(set(inputs) == input_keys,
            'Unknown or incomplete staged input format')
    headers = inputs['public_headers']
    legacy_headers = {name: path for name, path in HEADERS.items() if name != 'WebKitExtensionPermission.h'}
    require(inputs['target'] == target and (headers == HEADERS or (legacy and headers == legacy_headers)),
            'Staged target or public header mapping differs from this copier')
    source_hashes = hashes(inputs['sha256'], 'app source')
    required = {'tests/ModernBrowser.cpp', 'tools/build-modern-browser.py', 'LICENSE-Summit'}
    required.update('include/WebKit/' + name for name in headers)
    if target == 'browser':
        required.update({'src/main.cpp', 'src/core/Address.cpp', 'src/core/Profile.cpp',
                         'src/ui/BrowserWindow.cpp', 'src/ui/Chrome.cpp', 'resources/start.html'})
    require(required <= source_hashes.keys(), 'The frozen app source is incomplete')
    for name in source_hashes:
        require(name in required or (target == 'browser' and name.split('/')[0] in {'src', 'vendor', 'resources'}),
                'Unexpected staged app source: ' + name)
    configuration = {
        'PORT': 'Haiku', 'ENABLE_WEBKIT': 'ON', 'ENABLE_WEBKIT_LEGACY': 'OFF',
        'CMAKE_HOME_DIRECTORY': '/boot/home/summit-webkit' + ('-extensions' if extensions else ''),
        'ICU_ROOT': '/boot/home/summit-deps/icu78'}
    if not legacy:
        configuration.update(ENABLE_WK_WEB_EXTENSIONS='ON' if extensions else 'OFF',
                             ENABLE_CONTENT_EXTENSIONS='ON' if extensions else 'OFF')
    require(report['configuration'] == configuration,
            'Manifest is not the expected modern native/private ICU configuration')
    require(isinstance(report['compiler'], str) and report['compiler'], 'Missing native compiler provenance')
    commands = report['compile_commands']
    require(isinstance(commands, list) and commands and all(
        isinstance(command, list) and command and all(isinstance(arg, str) for arg in command)
        for command in commands), 'Missing native compile commands')
    hashes(report['original_sha256'], 'original native', absolute=True,
           extra_roots=('/SummitExtensions/WebKit/',) if extensions else ())
    bundled = hashes(report['bundled_sha256'], 'bundled')
    executable, launcher = ('Summit', 'run-browser.sh') if target == 'browser' else ('SummitModernPreview', 'run-preview.sh')
    regular = {executable, launcher, 'WebProcess', 'NetworkProcess'}
    if target == 'browser':
        regular.add('resources/start.html')
    libraries = LIBRARIES | ({'libzip'} if extensions else set())
    if extensions:
        regular.add('licenses/libzip/zip.h')
        lock = inputs['libzip']
        require(isinstance(lock, dict) and lock.get('version') == '1.11.4', 'Unexpected private libzip version')
        locked_files = hashes(lock.get('files'), 'private libzip')
        require({'lib/libzip.so.5.5', 'develop/headers/zip.h'} <= locked_files.keys(),
                'Incomplete private libzip lock')
        require(bundled.get('licenses/libzip/zip.h') == locked_files['develop/headers/zip.h']
                and bundled.get('lib/libzip.so.5.5') == locked_files['lib/libzip.so.5.5']
                and report['original_sha256'].get('/boot/home/summit-deps/libzip-1.11.4/lib/libzip.so.5.5')
                == locked_files['lib/libzip.so.5.5'],
                'The bundled libzip or its license notice differs from the lock')
    library_files = set(bundled) - regular
    require(regular <= bundled.keys() and len(library_files) == len(libraries)
            and all(LIBRARY.fullmatch(name) for name in library_files)
            and {LIBRARY.fullmatch(name)[1] for name in library_files} == libraries,
            'Incomplete or unexpected bundled executable/library list')
    symlinks = report['symlinks']
    require(isinstance(symlinks, dict), 'Invalid library symlink map')
    for name, link in symlinks.items():
        relative(name)
        relative(link)
        source_match, target_match = LIBRARY.fullmatch(name), LIBRARY.fullmatch('lib/' + link)
        require(source_match and target_match and source_match[1] == target_match[1]
                and 'lib/' + link in library_files and name not in bundled,
                'Library link must name its declared regular library in the same directory: ' + name)
    require(all('lib/' + name + '.so' in library_files | symlinks.keys() for name in libraries),
            'Missing unversioned private library names')
    elf_files = library_files | {executable, 'WebProcess', 'NetworkProcess'}
    require(isinstance(report['needed'], dict) and isinstance(report['runtime_search_paths'], dict)
            and set(report['needed']) == elf_files and set(report['runtime_search_paths']) == elf_files,
            'Incomplete native dependency provenance')
    for name in elf_files:
        needed = report['needed'][name]
        require(isinstance(needed, list) and all(isinstance(item, str) and '/' not in item
                and item not in ('', '.', '..') for item in needed), 'Invalid dependency names: ' + name)
        for dependency in needed:
            if dependency.startswith(('libWebKit', 'libJavaScriptCore', 'libicu', 'libzip')):
                require('lib/' + dependency in library_files | symlinks.keys(),
                        'Unbundled private dependency: ' + dependency)
        paths = report['runtime_search_paths'][name]
        expected = ['$ORIGIN'] if name.startswith('lib/') else ['$ORIGIN/lib']
        require(paths == expected or (name.startswith(('lib/libicu', 'lib/libzip')) and paths == []),
                'Unexpected runtime library search path: ' + name)
    return executable, launcher


def command(arguments, **options):
    return subprocess.check_output([str(arg) for arg in arguments], **options)


def git(source, *arguments):
    # Avoid locks, external diff programs and lazy object fetches during a read-only copy.
    environment = dict(os.environ, GIT_OPTIONAL_LOCKS='0', GIT_NO_LAZY_FETCH='1')
    return command(['git', '-C', source, *arguments], env=environment)


def included(name):
    return name.split('/')[0] in SOURCE_ROOTS


def excluded(name):
    # This is the same explicit exclusion used by the native source synchronization.
    return '__pycache__' in pathlib.PurePosixPath(name).parts


def support_snapshot(root, report):
    lock_data = (root / 'engine/sources.lock.json').read_bytes()
    icu_data = (root / 'engine/icu.lock.json').read_bytes()
    lock, icu = json_bytes(lock_data), json_bytes(icu_data)
    require(lock == report['inputs']['engine'] and icu == report['inputs']['icu'],
            'The frozen bundle and current engine/ICU locks differ; do not attach newer sources')
    if report['inputs'].get('engine_variant') == 'modern-extensions':
        require(json_bytes((root / 'engine/libzip.lock.json').read_bytes()) == report['inputs']['libzip'],
                'The frozen bundle and current private libzip lock differ')
    patch = relative(lock['patch']['path'])
    require(patch.startswith('engine/patches/') and HASH.fullmatch(lock['patch']['sha256']),
            'Invalid locked engine patch')
    files = {}
    for name in (*SUPPORT, patch):
        path = root / name
        require(path.is_file() and not path.is_symlink() and path.resolve().is_relative_to(root.resolve()),
                'Missing or indirect build support input: ' + name)
        files[name] = {'sha256': file_hash(path), 'mode': stat.S_IMODE(path.stat().st_mode)}
    require(files[patch]['sha256'] == lock['patch']['sha256'], 'Engine patch digest differs from its lock')
    return {'files': files, 'host_git_head': git(root, 'rev-parse', 'HEAD').decode().strip()}, lock


def source_snapshot(source, lock):
    require(source.is_dir() and not source.is_symlink(), 'Prepared cached WebKit source is missing or indirect')
    head = git(source, 'rev-parse', 'HEAD').decode().strip()
    require(head == lock['upstream']['commit'], 'Cached upstream HEAD differs from the bundled lock')
    require(git(source, 'rev-parse', '--show-object-format').strip() == b'sha1',
            'Unexpected WebKit Git object format')
    index_data = git(source, 'ls-files', '--stage', '-z')
    index = {}
    for entry in index_data.split(b'\0'):
        if not entry:
            continue
        metadata, raw_name = entry.split(b'\t', 1)
        mode, oid, stage = metadata.decode().split()
        name = os.fsdecode(raw_name)
        require(stage == '0' and mode in ('100644', '100755', '120000'),
                'Conflicted index or unsupported tracked object: ' + name)
        index[name] = (mode, oid)
    for entry in git(source, 'ls-files', '-v', '-z').split(b'\0'):
        if entry:
            name = os.fsdecode(entry[2:])
            require(not chr(entry[0]).islower() and (not included(name) or entry[:1] == b'H'),
                    'Git may hide source modifications (assume-unchanged/skip-worktree): ' + name)
    extras = [os.fsdecode(name) for name in git(source, 'ls-files', '--others', '-z').split(b'\0')
              if name and not excluded(os.fsdecode(name))]
    require(not extras, 'Untracked or ignored source extras are not allowed: ' + ', '.join(extras[:5]))
    patch_digest = sha256(git(source, 'diff', '--no-ext-diff', '--no-textconv', '--binary', '--full-index', 'HEAD'))
    require(patch_digest == lock['patch']['sha256'],
            'Exact git diff --binary --full-index HEAD does not match the locked engine patch')
    unstaged = {os.fsdecode(name) for name in git(source, 'diff', '--no-ext-diff', '--no-textconv', '--name-only', '-z').split(b'\0') if name}
    changed = {os.fsdecode(name) for name in git(source, 'diff', '--no-ext-diff', '--no-textconv', '--name-only', '-z', 'HEAD').split(b'\0') if name}
    require(all(included(name) for name in changed), 'The patch changes paths outside the archived native source roots')
    records = {}

    def scan(path, name):
        relative(name)
        if excluded(name):
            return
        metadata = path.lstat()
        mode = stat.S_IMODE(metadata.st_mode)
        require(not mode & 0o7000, 'Special mode bits on source input: ' + name)
        if stat.S_ISDIR(metadata.st_mode):
            records[name] = {'type': 'directory', 'mode': mode}
            for child in sorted(path.iterdir()):
                scan(child, name + '/' + child.name)
            return
        require(name in index, 'Untracked source input: ' + name)
        if stat.S_ISLNK(metadata.st_mode):
            target = os.readlink(path)
            require(not target.startswith('/') and '\\' not in target,
                    'Absolute or nonportable source link: ' + name)
            resolved_name = posixpath.normpath(posixpath.join(posixpath.dirname(name), target))
            require(included(resolved_name) and path.resolve().is_relative_to(source.resolve()),
                    'Source link escapes archived roots: ' + name)
            data = os.fsencode(target)
            value = {'type': 'symlink', 'mode': mode, 'target': target, 'sha256': sha256(data)}
            blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
            git_mode = '120000'
        else:
            require(stat.S_ISREG(metadata.st_mode), 'Special source file: ' + name)
            digest, blob_digest = hashlib.sha256(), hashlib.sha1()
            blob_digest.update(b'blob ' + str(metadata.st_size).encode() + b'\0')
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(block)
                    blob_digest.update(block)
            value = {'type': 'file', 'mode': mode, 'size': metadata.st_size, 'sha256': digest.hexdigest()}
            blob, git_mode = blob_digest.hexdigest(), '100755' if mode & 0o111 else '100644'
        if blob != index[name][1] and name not in unstaged and git_mode != '120000':
            # Git deliberately checks Windows scripts out with CRLF while the
            # index stores LF. Validate the cleaned blob without accepting an
            # arbitrary external clean filter or changing the archived bytes.
            attributes = git(source, 'check-attr', '-z', 'text', 'eol', 'filter', '--', name).split(b'\0')
            attributes = {attributes[i + 1]: attributes[i + 2] for i in range(0, len(attributes) - 1, 3)}
            if (attributes.get(b'eol') == b'crlf' and attributes.get(b'text') in (b'auto', b'set')
                    and attributes.get(b'filter') in (b'unspecified', b'unset')):
                blob = git(source, 'hash-object', '--path=' + name, name).decode().strip()
                value['git_content_sha1'] = blob
                value['working_tree_eol'] = 'crlf'
        require((git_mode, blob) == index[name] or name in unstaged,
                'Git stat cache hides changed source content or mode: ' + name)
        records[name] = value

    for name in SOURCE_ROOTS:
        scan(source / name, name)
    require(all(name in records for name in index if included(name) and not excluded(name) and name not in unstaged),
            'Tracked source is absent from the archive inventory')
    return {'upstream_head': head, 'patch_sha256': patch_digest, 'index_sha256': sha256(index_data),
            'roots': list(SOURCE_ROOTS), 'excluded_components': ['__pycache__'], 'entries': records}


def remote(root, native, action, output=None):
    # No code or filenames from the bundle are executed. The path has already passed
    # the fixed native-directory whitelist and is still shell-quoted explicitly.
    script = (
        'import pathlib,stat,os,sys\n'
        f'p=pathlib.Path({native!r})\n'
        'if not p.is_dir() or p.resolve() != p: raise SystemExit("Indirect native bundle path")\n'
        'm=p/"build-manifest.json"\n'
        'if not stat.S_ISREG(m.lstat().st_mode) or m.stat().st_size > 16777216: raise SystemExit("Invalid native manifest")\n'
    )
    if action == 'manifest':
        script += 'sys.stdout.buffer.write(m.read_bytes())\n'
    else:
        require(action == 'archive', 'Invalid remote operation')
        script += 'os.execvp("tar", ["tar", "-C", str(p), "-czf", "-", "."])\n'
    arguments = ['bash', root / 'tools/haiku.sh', 'python3.10 -c ' + shlex.quote(script)]
    if output is None:
        return command(arguments)
    subprocess.run([str(arg) for arg in arguments], check=True, stdout=output)


def extract_bundle(archive, destination, report):
    """Reject all links except declared leaf library aliases, before writing anything."""
    with tarfile.open(archive, 'r:gz') as stream:
        members = {}
        size = 0
        for member in stream:
            name = member.name
            if name == '.':
                require(member.isdir() and name not in members, 'Invalid archive root')
                members[name] = member
                continue
            if name.startswith('./'):
                name = name[2:]
            relative(name)
            require(name not in members, 'Duplicate archive member: ' + name)
            require(member.isdir() or member.isfile() or member.issym(), 'Special or hard-linked bundle member: ' + name)
            require(not member.mode & 0o7000, 'Special mode bits in bundle: ' + name)
            if member.issym():
                require(report['symlinks'].get(name) == member.linkname, 'Undeclared or changed bundle link: ' + name)
            else:
                require(name not in report['symlinks'], 'Declared bundle link changed type: ' + name)
            size += member.size
            require(0 <= member.size <= 8 * 1024**3 and size <= 32 * 1024**3 and len(members) < 200000,
                    'Bundle archive exceeds bounded extraction limits')
            members[name] = member
        for name in members:
            if name == '.':
                continue
            for parent in pathlib.PurePosixPath(name).parents:
                require(str(parent) == '.' or (str(parent) in members and members[str(parent)].isdir()),
                        'Missing or non-directory archive ancestor: ' + name)
        for name, member in sorted(members.items()):
            if name == '.':
                continue
            path = destination / name
            if member.isdir():
                path.mkdir(mode=0o755)
            elif member.isfile():
                with stream.extractfile(member) as source, path.open('xb') as output:
                    shutil.copyfileobj(source, output, 1024 * 1024)
                path.chmod(member.mode & 0o777)
            else:
                path.symlink_to(member.linkname)


def inventory(directory):
    result = {}
    for path in sorted(directory.rglob('*')):
        name = path.relative_to(directory).as_posix()
        metadata = path.lstat()
        if stat.S_ISDIR(metadata.st_mode):
            result[name] = {'type': 'directory'}
        elif stat.S_ISLNK(metadata.st_mode):
            result[name] = {'type': 'symlink', 'target': os.readlink(path)}
        else:
            require(stat.S_ISREG(metadata.st_mode), 'Special artifact file: ' + name)
            result[name] = {'type': 'file', 'size': metadata.st_size,
                            'mode': stat.S_IMODE(metadata.st_mode), 'sha256': file_hash(path)}
    return result


def validate_bundle(bundle, report, source, native_manifest):
    files = inventory(bundle)
    require((bundle / 'build-manifest.json').read_bytes() == native_manifest,
            'Native manifest changed during bundle transfer')
    require(json_bytes((bundle / 'source/inputs.json').read_bytes()) == report['inputs'],
            'Frozen app inputs differ from the bundle manifest')
    expected = dict(report['bundled_sha256'])
    expected.update({'source/' + name: digest for name, digest in report['inputs']['sha256'].items()})
    expected['LICENSE-Summit'] = report['inputs']['sha256']['LICENSE-Summit']
    require('licenses/ICU/LICENSE' in files and files['licenses/ICU/LICENSE']['type'] == 'file'
            and files['licenses/ICU/LICENSE']['size'] > 0, 'Missing private ICU license')
    for name, entry in source['entries'].items():
        if name.startswith('Source/') and pathlib.PurePosixPath(name).name.upper().startswith(('LICENSE', 'COPYING')):
            require(entry['type'] == 'file', 'Unexpected indirect WebKit license: ' + name)
            expected['licenses/WebKit/' + name.removeprefix('Source/')] = entry['sha256']
    for name, digest in expected.items():
        require(files.get(name, {}).get('type') == 'file' and files[name]['sha256'] == digest,
                'Missing or changed bundle file: ' + name)
    for name, link in report['symlinks'].items():
        require(files.get(name) == {'type': 'symlink', 'target': link}, 'Missing or changed library alias: ' + name)
    metadata = {'build-manifest.json', 'source/inputs.json', 'README.txt', 'licenses/ICU/LICENSE'}
    allowed = set(expected) | report['symlinks'].keys() | metadata
    parents = {str(parent) for name in allowed for parent in pathlib.PurePosixPath(name).parents}
    for name, entry in files.items():
        require(name in (parents if entry['type'] == 'directory' else allowed), 'Unexpected bundle input: ' + name)
    for name in metadata:
        require(files.get(name, {}).get('type') == 'file', 'Missing bundle metadata: ' + name)
    executable, launcher = ('Summit', 'run-browser.sh') if report['inputs']['target'] == 'browser' else ('SummitModernPreview', 'run-preview.sh')
    for name in (executable, launcher, 'WebProcess', 'NetworkProcess'):
        require(files[name]['mode'] & 0o111, 'Bundle executable lost its executable mode: ' + name)
    if report['inputs']['target'] == 'browser':
        require(expected['resources/start.html'] == expected['source/resources/start.html'],
                'Bundled start page differs from its frozen app source')
    for name, path in report['inputs']['public_headers'].items():
        require(source['entries']['Source/WebKit/' + path]['sha256'] == report['inputs']['sha256']['include/WebKit/' + name],
                'Frozen public header differs from the exact locked engine source: ' + name)
    return files


def source_archive(source, output, snapshot):
    # Add only the validated inventory; never recurse into a later untracked file.
    with tarfile.open(output, 'w:xz', preset=3, format=tarfile.PAX_FORMAT) as archive:
        for name, entry in sorted(snapshot['entries'].items()):
            metadata = tarfile.TarInfo(name)
            metadata.mode, metadata.mtime = entry['mode'], 0
            if entry['type'] == 'directory':
                metadata.type = tarfile.DIRTYPE
                archive.addfile(metadata)
            elif entry['type'] == 'symlink':
                metadata.type, metadata.linkname = tarfile.SYMTYPE, entry['target']
                require(os.readlink(source / name) == entry['target'], 'Source link changed during archiving: ' + name)
                archive.addfile(metadata)
            else:
                metadata.size = entry['size']
                with (source / name).open('rb') as stream:
                    # Hash exactly the bytes handed to tar, including files changed
                    # and restored between the two whole-tree snapshots.
                    class HashedReader:
                        def __init__(self):
                            self.digest = hashlib.sha256()

                        def read(self, count):
                            data = stream.read(count)
                            self.digest.update(data)
                            return data
                    reader = HashedReader()
                    archive.addfile(metadata, reader)
                    require(reader.digest.hexdigest() == entry['sha256'] and not stream.read(1),
                            'Source file changed during archiving: ' + name)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def copy_bundle(root, manifest_path, target):
    manifest_bytes = manifest_path.read_bytes()
    report = json_bytes(manifest_bytes)
    validate_report(report, target)
    native = report['bundle']
    output_root = root / 'artifacts/modern-browser'
    output_root.mkdir(parents=True, exist_ok=True)
    require(not output_root.is_symlink() and output_root.resolve() == root.resolve() / 'artifacts/modern-browser',
            'Artifact root must not be an indirect path')
    final = output_root / pathlib.PurePosixPath(native).name
    require(not os.path.lexists(final), 'Artifact already exists; no bundles are replaced: ' + str(final))
    with (output_root / '.copy.lock').open('a') as lock_file:
        fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        require(not os.path.lexists(final), 'Artifact already exists: ' + str(final))
        print('Verifying frozen manifest and exact locked source...', file=sys.stderr, flush=True)
        support_before, lock = support_snapshot(root, report)
        source = root / '.cache/WebKit'
        source_before = source_snapshot(source, lock)
        native_manifest = remote(root, native, 'manifest')
        require(json_bytes(native_manifest) == {key: value for key, value in report.items() if key != 'bundle'},
                'Native manifest differs from the selected host report')
        with tempfile.TemporaryDirectory(prefix='.import-', dir=output_root) as temporary:
            stage = pathlib.Path(temporary)
            archive = stage / 'native-bundle.tar.gz'
            payload = stage / final.name
            payload.mkdir()
            print('Copying and validating the frozen native bundle...', file=sys.stderr, flush=True)
            with archive.open('xb') as stream:
                remote(root, native, 'archive', stream)
            extract_bundle(archive, payload, report)
            native_files = validate_bundle(payload, report, source_before, native_manifest)
            require(remote(root, native, 'manifest') == native_manifest, 'Native manifest changed while copying')
            support_directory = payload / 'rebuild'
            for name, entry in support_before['files'].items():
                destination = support_directory / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(root / name, destination)
                destination.chmod(entry['mode'])
                require(file_hash(destination) == entry['sha256'], 'Build support changed while copying: ' + name)
            # The producer itself is a frozen app input. Never substitute today's
            # host builder for the exact source used to compile this bundle.
            shutil.copyfile(payload / 'source/tools/build-modern-browser.py', support_directory / 'tools/build-modern-browser.py')
            print('Archiving the complete native WebKit source trees...', file=sys.stderr, flush=True)
            engine_archive = payload / 'WebKit-sources.tar.xz'
            source_archive(source, engine_archive, source_before)
            write_json(payload / 'WebKit-source-manifest.json', source_before)
            (payload / 'host-bundle-report.json').write_bytes(manifest_bytes)
            shutil.copyfile(root / 'docs/modern-bundle-copy.md', payload / 'BUILDING.md')
            print('Rechecking source and lock fingerprints before publishing...', file=sys.stderr, flush=True)
            source_after = source_snapshot(source, lock)
            support_after, _ = support_snapshot(root, report)
            require(source_before == source_after, 'Engine source changed during the copy; artifact discarded')
            require(support_before == support_after, 'Locks or build support changed during the copy; artifact discarded')
            require(manifest_path.read_bytes() == manifest_bytes, 'Selected host bundle report changed during the copy')
            provenance = {
                'format': 1, 'copied_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'kind': report['kind'], 'native_bundle': native,
                'host_report_sha256': sha256(manifest_bytes), 'native_manifest_sha256': sha256(native_manifest),
                'transport_archive_sha256': file_hash(archive),
                'WebKit_source_archive_sha256': file_hash(engine_archive),
                'source_fingerprint_before': sha256(canonical(source_before)),
                'source_fingerprint_after': sha256(canonical(source_after)),
                'support_fingerprint_before': sha256(canonical(support_before)),
                'support_fingerprint_after': sha256(canonical(support_after)),
                'copy_host_support': support_before,
                'native_bundle_entries': native_files,
                'artifact_entries': inventory(payload),
                'notes': [
                    'The source archive contains the full native build trees listed in WebKit-source-manifest.json, not a Git clone.',
                    'Frozen app source is in source/. Host build support is in rebuild/; its Git HEAD identifies the copy host, not the original app build.',
                    'copy-provenance.json excludes itself from artifact_entries. Original build-manifest.json and host report are preserved verbatim.',
                    'Hash verification records byte identity and provenance; this copy does not perform a browser runtime test.',
                ],
            }
            write_json(payload / 'copy-provenance.json', provenance)
            require(not os.path.lexists(final), 'Artifact appeared during the copy; refusing to replace it')
            payload.rename(final)
    print(json.dumps({'artifact': str(final), 'provenance_sha256': file_hash(final / 'copy-provenance.json')}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', nargs='?', type=pathlib.Path,
                        help='Host frozen-bundle report (default: .vm/modern-browser-bundle.json)')
    parser.add_argument('--preview', action='store_true', help='Accept a preview bundle; defaults to .vm/modern-preview-bundle.json')
    arguments = parser.parse_args()
    target = 'preview' if arguments.preview else 'browser'
    manifest = arguments.manifest or ROOT / f'.vm/modern-{target}-bundle.json'
    try:
        copy_bundle(ROOT, manifest.resolve(), target)
    except (ValueError, OSError, subprocess.SubprocessError, tarfile.TarError, KeyError, TypeError) as error:
        print('Modern bundle copy refused: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

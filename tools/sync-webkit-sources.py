#!/usr/bin/env python3
"""Synchronize an engine source archive without retaining stale build timestamps."""
import argparse
import json
import os
import pathlib
import re
import stat
import sys
import tarfile
import tempfile

COMPARE_BUFFER_LIMIT = 8 * 1024 * 1024


def relative_path(name):
    path = pathlib.PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or not path.parts:
        raise ValueError('Invalid engine archive path: ' + name)
    if path.parts[0] not in ('CMakeLists.txt', 'Configurations', 'Source', 'Tools'):
        raise ValueError('Unexpected engine archive member: ' + name)
    return path


def identical(first, second):
    if second.is_symlink() or not second.is_file() or first.stat().st_size != second.stat().st_size:
        return False
    with first.open('rb') as incoming, second.open('rb') as current:
        while True:
            chunk = incoming.read(1024 * 1024)
            if chunk != current.read(len(chunk)):
                return False
            if not chunk:
                return True


def synchronize(destination, patch, stream):
    destination.mkdir(parents=True, exist_ok=True)
    manifest_path = destination / '.summit-source-manifest.json'
    previous = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    members = set()
    changed = unchanged = removed = 0
    with tarfile.open(fileobj=stream, mode='r|gz') as archive:
        for member in archive:
            relative = relative_path(member.name)
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if member.isdir():
                target.mkdir(exist_ok=True)
                continue
            members.add(str(relative))
            if member.issym():
                if target.is_symlink() and os.readlink(target) == member.linkname:
                    unchanged += 1
                    continue
                if target.exists() or target.is_symlink():
                    target.unlink()
                target.symlink_to(member.linkname)
                changed += 1
                continue
            if not member.isfile():
                raise ValueError('Unsupported engine archive member: ' + member.name)
            # Most repeated uploads are unchanged. Avoid creating, writing and
            # unlinking a temporary BFS inode (and changing permissions) for
            # every one of WebKit's tens of thousands of small source files.
            data = None
            if member.size <= COMPARE_BUFFER_LIMIT:
                with archive.extractfile(member) as source:
                    data = source.read()
                if not target.is_symlink() and target.is_file() and target.stat().st_size == len(data):
                    if target.read_bytes() == data:
                        unchanged += 1
                        if stat.S_IMODE(target.stat().st_mode) != member.mode & 0o777:
                            target.chmod(member.mode & 0o777)
                        continue
            with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as output:
                temporary = pathlib.Path(output.name)
                try:
                    if data is not None:
                        output.write(data)
                    else:
                        with archive.extractfile(member) as source:
                            while chunk := source.read(1024 * 1024):
                                output.write(chunk)
                    output.close()
                    if identical(temporary, target):
                        unchanged += 1
                        if stat.S_IMODE(target.stat().st_mode) != member.mode & 0o777:
                            target.chmod(member.mode & 0o777)
                    else:
                        temporary.chmod(member.mode & 0o777)
                        # A changed source must be newer than generated files
                        # made from the old content, even when its archived
                        # modification time predates the previous build.
                        os.utime(temporary, None)
                        os.replace(temporary, target)
                        changed += 1
                finally:
                    temporary.unlink(missing_ok=True)
    for name in previous.get('members', []):
        if name in members:
            continue
        target = destination / relative_path(name)
        if target.is_file() or target.is_symlink():
            target.unlink()
            removed += 1
    manifest = {'patch_sha256': patch, 'members': sorted(members)}
    temporary_manifest = manifest_path.with_suffix('.tmp')
    temporary_manifest.write_text(json.dumps(manifest, separators=(',', ':')) + '\n')
    temporary_manifest.replace(manifest_path)
    print(f'Engine sources: {changed} changed, {unchanged} unchanged, {removed} removed; patch {patch}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('destination', type=pathlib.Path)
    parser.add_argument('patch_sha256')
    arguments = parser.parse_args()
    if not re.fullmatch('[0-9a-f]{64}', arguments.patch_sha256):
        parser.error('patch_sha256 must be a SHA-256 digest')
    synchronize(arguments.destination.resolve(), arguments.patch_sha256, sys.stdin.buffer)

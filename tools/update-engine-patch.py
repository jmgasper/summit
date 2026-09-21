#!/usr/bin/env python3
"""Regenerate engine/patches/0001-haiku-port.patch from the host WebKit tree.

The host checkout (.cache/WebKit) is the pinned upstream commit with the port
applied as working-tree changes. This stages everything, writes the complete
diff against the pin and records its digest in engine/sources.lock.json.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]


def git(directory, *args):
    return subprocess.run(['git', '-C', str(directory), *args], check=True,
                          stdout=subprocess.PIPE).stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=pathlib.Path, default=ROOT / '.cache/WebKit')
    parser.add_argument('--check', action='store_true', help='Only report whether the patch is current.')
    args = parser.parse_args()
    lock_path = ROOT / 'engine/sources.lock.json'
    lock = json.loads(lock_path.read_text())
    if git(args.source, 'rev-parse', 'HEAD').decode().strip() != lock['upstream']['commit']:
        raise SystemExit('Checkout is on a different upstream revision.')
    git(args.source, 'add', '-A', '--', 'CMakeLists.txt', 'Configurations', 'Source', 'Tools')
    patch = git(args.source, 'diff', '--cached', '--no-ext-diff', '--no-textconv', '--binary',
                '--full-index', 'HEAD')
    digest = hashlib.sha256(patch).hexdigest()
    if digest == lock['patch']['sha256']:
        print('Engine patch is current:', digest)
        return
    if args.check:
        raise SystemExit('Engine patch is stale; tree digest ' + digest)
    (ROOT / lock['patch']['path']).write_bytes(patch)
    lock['patch']['sha256'] = digest
    lock_path.write_text(json.dumps(lock, indent=2) + '\n')
    print('Updated engine patch:', digest)


if __name__ == '__main__':
    main()

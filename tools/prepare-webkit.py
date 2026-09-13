#!/usr/bin/env python3
"""Materialize the pinned upstream WebKit tree and the native Haiku port patch."""
import argparse
import hashlib
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]


def git(directory, *args, capture=False):
    return subprocess.run(['git', '-C', str(directory), *args], check=True,
                          text=True, stdout=subprocess.PIPE if capture else None).stdout


def prepare(destination):
    lock = json.loads((ROOT / 'engine/sources.lock.json').read_text())
    patch = ROOT / lock['patch']['path']
    if hashlib.sha256(patch.read_bytes()).hexdigest() != lock['patch']['sha256']:
        raise SystemExit('Engine patch differs from sources.lock.json. Update the lock after reviewing port changes.')
    pin = lock['upstream']['commit']
    if not destination.exists():
        destination.mkdir(parents=True)
        git(destination, 'init')
        git(destination, 'remote', 'add', 'origin', lock['upstream']['url'])
        git(destination, 'fetch', '--depth=1', '--filter=blob:none', 'origin', pin)
        git(destination, 'sparse-checkout', 'set', 'Source', 'Tools', 'Configurations')
        git(destination, 'checkout', '--detach', 'FETCH_HEAD')
    if git(destination, 'rev-parse', 'HEAD', capture=True).strip() != pin:
        raise SystemExit('Checkout is on a different upstream revision; use a new --destination.')
    if not (destination / 'Configurations/Version.xcconfig').is_file():
        sparse = subprocess.run(['git', '-C', str(destination), 'config', '--bool', 'core.sparseCheckout'],
                                text=True, stdout=subprocess.PIPE)
        if sparse.stdout.strip() != 'true':
            raise SystemExit('Configurations/Version.xcconfig is missing from the checkout; review local changes.')
        git(destination, 'sparse-checkout', 'add', 'Configurations')
    reverse = subprocess.run(['git', '-C', str(destination), 'apply', '--reverse', '--check', str(patch)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if reverse.returncode == 0:
        print('Pinned Haiku port is already applied:', destination)
        return
    if git(destination, 'status', '--porcelain', capture=True).strip():
        raise SystemExit('Checkout contains local changes that do not match the port patch; preserve or review them first.')
    git(destination, 'apply', '--check', str(patch))
    git(destination, 'apply', str(patch))
    print('Prepared upstream', pin, 'with Haiku patch', lock['patch']['sha256'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--destination', type=pathlib.Path, default=ROOT / '.cache/WebKit')
    args = parser.parse_args()
    prepare(args.destination.resolve())

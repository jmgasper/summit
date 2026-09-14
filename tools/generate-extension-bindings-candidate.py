#!/usr/bin/env python3
"""Generate isolated extension bindings with the locked upstream Perl generator."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / '.cache/WebKit'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def generate(overlay):
    overlay = Path(overlay).resolve()
    def effective(path):
        candidate = overlay / path.relative_to(ENGINE)
        return candidate if candidate.is_file() else path
    cmake = effective(ENGINE / 'Source/WebKit/CMakeLists.txt')
    text = cmake.read_text()
    names = []
    for variable in ('WebKit_BINDINGS_IN_FILES', 'WebKit_CPP_BINDINGS_IN_FILES'):
        match = re.search(r'set\(' + variable + r'\s+(.*?)\)', text, re.S)
        if not match:
            raise RuntimeError('Missing configured IDL list: ' + variable)
        entries = match.group(1).split()
        if any(not re.fullmatch(r'WebProcess/Extensions/Interfaces/\w+', name) for name in entries):
            raise RuntimeError('Unexpected extension IDL list')
        names.extend(entries)
    if len(names) != len(set(names)):
        raise RuntimeError('Duplicate extension IDL')
    idls = [effective(ENGINE / 'Source/WebKit' / (name + '.idl')) for name in names]
    scripts = ENGINE / 'Source/WebKit/WebProcess/Extensions/Bindings/Scripts'
    core_scripts = ENGINE / 'Source/WebCore/bindings/scripts'
    watched = [cmake, *idls]
    watched += [effective(path) for directory in (scripts, core_scripts) for path in sorted(directory.iterdir())
                if path.suffix in ('.pl', '.pm', '.json')]
    snapshot = {str(path): digest(path) for path in watched}
    output = Path(tempfile.mkdtemp(prefix='extension-bindings-generated-', dir=ROOT / '.vm'))
    # Stage the effective generator too, so candidate attributes and generator
    # changes can be validated before promoting the engine patch.
    staged_scripts = output / 'generator'
    staged_scripts.mkdir()
    for path in sorted(scripts.iterdir()):
        if path.is_file():
            shutil.copyfile(effective(path), staged_scripts / path.name)
    scripts = staged_scripts
    listing = output / 'WebExtensionIDLFileNamesList.txt'
    listing.write_text('\n'.join(map(str, idls)) + '\n')
    command = [shutil.which('perl'), '-I', str(scripts), str(core_scripts / 'generate-bindings.pl'),
               '--outputDir', str(output), '--generator', 'Extensions',
               '--idlAttributesFile', str(scripts / 'IDLAttributes.json'),
               '--idlFileNamesList', str(listing), *map(str, idls)]
    with (output / 'generate.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    unchanged = all(digest(Path(path)) == expected for path, expected in snapshot.items())
    files = {path.name: digest(path) for path in sorted(output.iterdir()) if path.suffix in ('.cpp', '.mm', '.h')}
    report = {'scope': 'upstream host binding generation only; no compile or runtime',
              'engine_patch_sha256': json.loads((ROOT / 'engine/sources.lock.json').read_text())['patch']['sha256'],
              'overlay': str(overlay), 'command': command, 'inputs': snapshot,
              'inputs_unchanged': unchanged, 'exit': result.returncode, 'files': files,
              'passed': result.returncode == 0 and unchanged and len(files) == 2 * len(idls)}
    (output / 'binding-generation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(output, flush=True)
    if not report['passed']:
        raise SystemExit('Binding generation failed: ' + str(output / 'binding-generation.json'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--overlay', required=True, help='Candidate Source/ tree; production files remain unchanged')
    args = parser.parse_args()
    generate(args.overlay)

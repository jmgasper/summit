#!/usr/bin/env python3
"""Verify separated animated regions in the actual browser screenshot.

Uses run-probe.py for launch, fixture serving, crash detection and teardown.
Checks screen pixels separately from the DOM to catch omitted GPU readbacks.
Additional arguments are forwarded to run-probe.py (bundle, environment, label).
"""
import hashlib
import json
import subprocess
import sys

from PIL import Image, ImageChops

import guest


def validate_pixels(directory, probe, frame=(4, 1, 1917, 1077)):
    image = Image.open(directory / 'final.png').convert('RGB')
    # The native screenshot is in logical pixels even on the DPR=2 desktop.
    # Find the fixture's background within our requested window, so chrome's
    # height need not be guessed. Each patch has ample interior for sampling.
    if image.width < frame[2] or image.height < frame[3]:
        raise ValueError(f'Screenshot is too small: {image.size}')
    crop = image.crop(frame)
    background = Image.new('RGB', crop.size, (24, 32, 44))
    difference = ImageChops.difference(crop, background)
    channels = difference.split()
    mask = ImageChops.lighter(ImageChops.lighter(channels[0], channels[1]), channels[2])
    bounds = mask.point(lambda value: 255 if value == 0 else 0).getbbox()
    if bounds is None:
        raise ValueError('Fixture background is absent from screenshot')
    origin = (frame[0] + bounds[0], frame[1] + bounds[1])
    viewport = probe['viewport']
    if abs(bounds[2] - bounds[0] - viewport['width']) > 2 or abs(bounds[3] - bounds[1] - viewport['height']) > 2:
        raise ValueError(f'Background bounds {bounds} do not match viewport {viewport}')
    checks = []
    expected = tuple(probe['expectedRGB'])
    for patch in probe['patches']:
        rect = patch['rect']
        for fx, fy in ((.25, .25), (.5, .5), (.75, .75)):
            x = round(origin[0] + rect['x'] + rect['width'] * fx)
            y = round(origin[1] + rect['y'] + rect['height'] * fy)
            colors = list(image.crop((x - 2, y - 2, x + 3, y + 3)).getdata())
            checks.append({'patch': patch['id'], 'at': [x, y], 'expected': expected,
                           'actual': sorted(set(colors)), 'passed': all(color == expected for color in colors)})
    return {'passed': len(checks) == 9 and all(check['passed'] for check in checks),
            'origin': origin, 'checks': checks}


def main():
    command = [sys.executable, str(guest.BENCH / 'run-probe.py'),
               '--page', 'readback-separated-damage.html', '--window', '4,1,1916,1076',
               '--env', 'SUMMIT_DIRECT_PRESENT=0', *sys.argv[1:]]
    run = subprocess.run(command, text=True, stdout=subprocess.PIPE)
    try:
        report = json.loads(run.stdout)
    except ValueError:
        print(run.stdout)
        return run.returncode or 1
    directory = guest.ROOT / '.vm/bench' / report['id']
    report['fixtureSHA256'] = hashlib.sha256((guest.BENCH / 'pages/readback-separated-damage.html').read_bytes()).hexdigest()
    try:
        if report['page'] != 'readback-separated-damage.html' or report['browser'] != 'summit':
            raise ValueError('This test requires the separated-damage fixture in Summit')
        report['screenValidation'] = validate_pixels(directory, report['probe'])
    except Exception as error:
        report['screenValidation'] = {'passed': False, 'error': str(error)}
    passed = (run.returncode == 0 and report['screenValidation']['passed']
              and report.get('shutdown', {}).get('method') == 'quit-request')
    report['passed'] = passed
    if not passed:
        report['outcome'] = 'screen-validation-failed' if run.returncode == 0 else report['outcome']
    (directory / 'run.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': passed, 'report': str(directory / 'run.json'),
                      'screenValidation': report['screenValidation']}, indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""Send real USB HID pointer input to the workstation through its NanoKVM.

The NanoKVM is plugged into the workstation as a USB keyboard and absolute
pointer, so what it sends reaches Haiku through input_server exactly as a
physical device would. That is the path a person's mouse wheel takes, which
summitctl's synthesized wheel messages and VNC's injected events do not.

Set SUMMIT_NANOKVM_PASSWORD (and optionally SUMMIT_NANOKVM_HOST/USER). The
password and session token are never printed or written to a run artifact.

  nanokvm-input.py move X Y
  nanokvm-input.py click X Y
  nanokvm-input.py wheel X Y [--notches N] [--direction up|down] [--interval-ms MS]
"""
import argparse
import asyncio
import importlib.util
import os
import pathlib
import time

HERE = pathlib.Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location('nanokvm_capture', HERE / 'nanokvm-capture.py')
capture = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(capture)

WIDTH, HEIGHT = 1920, 1080


def report(buttons, x, y, wheel=0):
    # NanoKVM's web client sends [2, buttons, x lo, x hi, y lo, y hi, wheel]
    # for its absolute pointer, with coordinates scaled to 1..32767.
    ax = int(32767 * x / WIDTH) + 1
    ay = int(32767 * y / HEIGHT) + 1
    return bytes([2, buttons, ax & 255, ax >> 8, ay & 255, ay >> 8, wheel & 255])


async def send(host, token, steps):
    import websockets
    async with websockets.connect(f'ws://{host}/api/ws',
                                  extra_headers={'Cookie': 'nano-kvm-token=' + token}) as ws:
        for step in steps:
            if isinstance(step, (int, float)):
                await asyncio.sleep(step)
            else:
                await ws.send(step)
        await asyncio.sleep(0.2)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('move', 'click', 'wheel'):
        command = commands.add_parser(name)
        command.add_argument('x', type=float)
        command.add_argument('y', type=float)
        if name == 'wheel':
            command.add_argument('--notches', type=int, default=1)
            command.add_argument('--direction', choices=('up', 'down'), default='down')
            command.add_argument('--interval-ms', type=int, default=30)
            command.add_argument('--step', type=int, default=1, help='wheel units per report')
    args = parser.parse_args()
    host = os.environ.get('SUMMIT_NANOKVM_HOST', '192.168.1.22')
    password = os.environ.get('SUMMIT_NANOKVM_PASSWORD')
    if not password:
        raise SystemExit('Set SUMMIT_NANOKVM_PASSWORD.')
    token = capture.authenticate('http://' + host, os.environ.get('SUMMIT_NANOKVM_USER', 'admin'), password)
    x, y = args.x, args.y
    steps = [report(0, x, y), 0.1]
    if args.command == 'click':
        steps += [report(1, x, y), 0.08, report(0, x, y)]
    elif args.command == 'wheel':
        # HID wheel is positive away from the user, which scrolls up.
        amount = args.step if args.direction == 'up' else -args.step
        for _ in range(args.notches):
            steps += [report(0, x, y, amount), args.interval_ms / 1000]
    started = time.time()
    asyncio.run(send(host, token, steps))
    print(f'NanoKVM {args.command} sent in {time.time() - started:.2f} s')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Send a pointer click or drag to the workstation's VNC server.

The password is read from SUMMIT_VNC_PASSWORD and is never stored in a run
artifact. This is intended for interactive browser controls that summitctl's
synthetic wheel command cannot exercise, such as media progress bars.
"""
import argparse
import os
import socket
import struct
import sys
import time


def read_exact(connection, length):
    chunks = []
    while length:
        chunk = connection.recv(length)
        if not chunk:
            raise RuntimeError('VNC connection closed')
        chunks.append(chunk)
        length -= len(chunk)
    return b''.join(chunks)


def reverse_bits(byte):
    return int(f'{byte:08b}'[::-1], 2)


def authenticate(connection, version, password):
    if version == b'RFB 003.003\n':
        security = struct.unpack('>I', read_exact(connection, 4))[0]
    else:
        count = read_exact(connection, 1)[0]
        if not count:
            length = struct.unpack('>I', read_exact(connection, 4))[0]
            raise RuntimeError(read_exact(connection, length).decode('utf-8', 'replace'))
        offered = read_exact(connection, count)
        security = 2 if 2 in offered and password else 1 if 1 in offered else 0
        if not security:
            raise RuntimeError(f'No supported VNC security type in {list(offered)}')
        connection.sendall(bytes([security]))
    if security == 2:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

        challenge = read_exact(connection, 16)
        key = bytes(reverse_bits(byte) for byte in password.encode('latin-1')[:8].ljust(8, b'\0'))
        encryptor = Cipher(algorithms.TripleDES(key * 3), modes.ECB()).encryptor()
        connection.sendall(encryptor.update(challenge) + encryptor.finalize())
    elif security != 1:
        raise RuntimeError(f'Unsupported VNC security type {security}')
    if version != b'RFB 003.003\n' or security == 2:
        status = struct.unpack('>I', read_exact(connection, 4))[0]
        if status:
            raise RuntimeError(f'VNC authentication failed (status {status})')


def pointer(connection, x, y, buttons):
    connection.sendall(struct.pack('>BBHH', 5, buttons, x, y))


def key(connection, keysym, pressed):
    connection.sendall(struct.pack('>BBHI', 4, int(pressed), 0, keysym))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--port', type=int, default=5900)
    commands = parser.add_subparsers(dest='command', required=True)
    click = commands.add_parser('click')
    click.add_argument('x', type=int)
    click.add_argument('y', type=int)
    drag = commands.add_parser('drag')
    for name in ('x1', 'y1', 'x2', 'y2'):
        drag.add_argument(name, type=int)
    drag.add_argument('--steps', type=int, default=12)
    drag.add_argument('--interval-ms', type=int, default=30)
    wheel = commands.add_parser('wheel')
    wheel.add_argument('x', type=int)
    wheel.add_argument('y', type=int)
    wheel.add_argument('--notches', type=int, default=1)
    wheel.add_argument('--direction', choices=('up', 'down'), default='down')
    wheel.add_argument('--interval-ms', type=int, default=25)
    keyboard = commands.add_parser('key')
    keyboard.add_argument('name', choices=('pagedown', 'pageup', 'down', 'up', 'space'))
    args = parser.parse_args()
    password = os.environ.get('SUMMIT_VNC_PASSWORD', '')
    with socket.create_connection((args.host, args.port), timeout=10) as connection:
        connection.settimeout(10)
        version = read_exact(connection, 12)
        if not version.startswith(b'RFB 003.'):
            raise RuntimeError(f'Unexpected VNC version {version!r}')
        connection.sendall(version)
        authenticate(connection, version, password)
        connection.sendall(b'\x01')  # Share the desktop with other viewers.
        header = read_exact(connection, 24)
        width, height = struct.unpack('>HH', header[:4])
        name_length = struct.unpack('>I', header[20:24])[0]
        read_exact(connection, name_length)

        def checked_pointer(x, y, buttons):
            if not (0 <= x < width and 0 <= y < height):
                raise ValueError(f'Pointer ({x}, {y}) is outside {width}x{height}')
            pointer(connection, x, y, buttons)

        if args.command == 'click':
            checked_pointer(args.x, args.y, 0)
            checked_pointer(args.x, args.y, 1)
            time.sleep(0.1)
            checked_pointer(args.x, args.y, 0)
        elif args.command == 'drag':
            if args.steps < 1 or args.interval_ms < 0:
                parser.error('--steps must be positive and --interval-ms nonnegative')
            checked_pointer(args.x1, args.y1, 0)
            checked_pointer(args.x1, args.y1, 1)
            for step in range(1, args.steps + 1):
                x = round(args.x1 + (args.x2 - args.x1) * step / args.steps)
                y = round(args.y1 + (args.y2 - args.y1) * step / args.steps)
                time.sleep(args.interval_ms / 1000)
                checked_pointer(x, y, 1)
            checked_pointer(args.x2, args.y2, 0)
        elif args.command == 'wheel':
            if args.notches < 1 or args.interval_ms < 0:
                parser.error('--notches must be positive and --interval-ms nonnegative')
            button = 8 if args.direction == 'up' else 16
            for _ in range(args.notches):
                checked_pointer(args.x, args.y, button)
                checked_pointer(args.x, args.y, 0)
                if args.interval_ms:
                    time.sleep(args.interval_ms / 1000)
        else:
            keysym = {'pagedown': 0xff56, 'pageup': 0xff55,
                      'down': 0xff54, 'up': 0xff52, 'space': 0x20}[args.name]
            key(connection, keysym, True)
            time.sleep(0.08)
            key(connection, keysym, False)
        print(f'VNC {args.command} delivered to {width}x{height} desktop')


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Send a pointer click or drag to the workstation's VNC server, or grab its screen.

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
    move = commands.add_parser('move')
    move.add_argument('x', type=int)
    move.add_argument('y', type=int)
    click = commands.add_parser('click')
    click.add_argument('x', type=int)
    click.add_argument('y', type=int)
    click.add_argument('--button', type=int, choices=(1, 2, 4), default=1,
                       help='1 primary, 2 middle, 4 secondary')
    click.add_argument('--hold-ms', type=int, default=100)
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
    wheel.add_argument('--jitter', type=int, default=0,
                       help='move the pointer this many pixels between notches, as a hand on a mouse does')
    wheel.add_argument('--mod', action='append', default=[], choices=('alt', 'ctrl', 'shift'),
                       help='hold a modifier while turning the wheel; repeatable')
    shot = commands.add_parser('shot', help='save the screen as the VNC server sends it (logical size)')
    shot.add_argument('output')
    shot.add_argument('--crop', type=int, nargs=4, metavar=('X', 'Y', 'W', 'H'))
    keyboard = commands.add_parser('key')
    keyboard.add_argument('name', help='pagedown, pageup, down, up, space, enter, escape or one printable character')
    keyboard.add_argument('--mod', action='append', default=[], choices=('alt', 'ctrl', 'shift'),
                          help='hold a modifier (Alt is the Haiku Command key); repeatable')
    typing = commands.add_parser('type')
    typing.add_argument('text', help='printable ASCII text to type')
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

        if args.command == 'move':
            checked_pointer(args.x, args.y, 0)
        elif args.command == 'click':
            checked_pointer(args.x, args.y, 0)
            time.sleep(0.05)
            checked_pointer(args.x, args.y, args.button)
            time.sleep(args.hold_ms / 1000)
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
            modifiers = [{'alt': 0xffe9, 'ctrl': 0xffe3, 'shift': 0xffe1}[name] for name in args.mod]
            for modifier in modifiers:
                key(connection, modifier, True)
                time.sleep(0.05)
            for notch in range(args.notches):
                x = args.x + (args.jitter if notch % 2 else 0)
                y = args.y + (args.jitter if notch % 4 >= 2 else 0)
                if args.jitter:
                    checked_pointer(x, y, 0)
                checked_pointer(x, y, button)
                checked_pointer(x, y, 0)
                if args.interval_ms:
                    time.sleep(args.interval_ms / 1000)
            for modifier in reversed(modifiers):
                time.sleep(0.05)
                key(connection, modifier, False)
        elif args.command == 'shot':
            from PIL import Image

            x, y, w, h = args.crop or (0, 0, width, height)
            # 32-bit little-endian true colour, raw encoding only.
            connection.sendall(struct.pack('>BxxxBBBBHHHBBBxxx', 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
            connection.sendall(struct.pack('>BxHi', 2, 1, 0))
            connection.sendall(struct.pack('>BBHHHH', 3, 0, x, y, w, h))
            image = Image.new('RGB', (w, h))
            covered = 0
            connection.settimeout(30)
            while covered < w * h:
                kind = read_exact(connection, 1)[0]
                if kind != 0:
                    # Bell (2) or cut text (3) can arrive first.
                    if kind == 3:
                        read_exact(connection, 3)
                        read_exact(connection, struct.unpack('>I', read_exact(connection, 4))[0])
                    elif kind == 1:
                        read_exact(connection, 3)
                        count = struct.unpack('>H', read_exact(connection, 2))[0]
                        read_exact(connection, count * 6)
                    continue
                read_exact(connection, 1)
                rectangles = struct.unpack('>H', read_exact(connection, 2))[0]
                for _ in range(rectangles):
                    rx, ry, rw, rh, encoding = struct.unpack('>HHHHi', read_exact(connection, 12))
                    if encoding != 0:
                        raise RuntimeError(f'Unexpected VNC encoding {encoding}')
                    pixels = read_exact(connection, rw * rh * 4)
                    tile = Image.frombuffer('RGBX', (rw, rh), pixels, 'raw', 'BGRX', 0, 1).convert('RGB')
                    image.paste(tile, (rx - x, ry - y))
                    covered += rw * rh
            image.save(args.output)
        elif args.command == 'type':
            for character in args.text:
                if not ' ' <= character <= '~':
                    parser.error('type only sends printable ASCII')
                key(connection, ord(character), True)
                time.sleep(0.03)
                key(connection, ord(character), False)
                time.sleep(0.03)
        else:
            names = {'pagedown': 0xff56, 'pageup': 0xff55,
                     'down': 0xff54, 'up': 0xff52, 'space': 0x20,
                     'enter': 0xff0d, 'escape': 0xff1b}
            if args.name in names:
                keysym = names[args.name]
            elif len(args.name) == 1 and ' ' <= args.name <= '~':
                keysym = ord(args.name)
            else:
                parser.error('unknown key name')
            modifiers = [{'alt': 0xffe9, 'ctrl': 0xffe3, 'shift': 0xffe1}[name] for name in args.mod]
            for modifier in modifiers:
                key(connection, modifier, True)
                time.sleep(0.03)
            key(connection, keysym, True)
            time.sleep(0.08)
            key(connection, keysym, False)
            for modifier in reversed(modifiers):
                time.sleep(0.03)
                key(connection, modifier, False)
        print(f'VNC {args.command} delivered to {width}x{height} desktop')


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Capture NanoKVM MJPEG frames without touching the workstation display.

Set SUMMIT_NANOKVM_PASSWORD in the environment. The output contains frame
timings and optional JPEGs, never the password or NanoKVM session token.
"""
import argparse
import asyncio
import base64
import hashlib
import io
import json
import os
import pathlib
import time
import urllib.parse
import urllib.request

from cryptography.hazmat.primitives import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


def encrypted_password(password):
    # NanoKVM's web client uses CryptoJS AES passphrase encryption with this
    # device key. OpenSSL's salted MD5 key derivation is the wire format.
    salt = os.urandom(8)
    key_material = b''
    block = b''
    while len(key_material) < 48:
        block = hashlib.md5(block + b'nanokvm-sipeed-2024' + salt).digest()
        key_material += block
    padder = padding.PKCS7(128).padder()
    plaintext = padder.update(password.encode()) + padder.finalize()
    encryptor = Cipher(algorithms.AES(key_material[:32]), modes.CBC(key_material[32:48])).encryptor()
    ciphertext = encryptor.update(plaintext) + encryptor.finalize()
    return urllib.parse.quote(base64.b64encode(b'Salted__' + salt + ciphertext).decode(), safe='')


def authenticate(base, username, password):
    body = json.dumps({'username': username, 'password': encrypted_password(password)}).encode()
    request = urllib.request.Request(base + '/api/auth/login', data=body,
                                     headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=15) as response:
        result = json.load(response)
    if result.get('code') != 0 or not result.get('data', {}).get('token'):
        raise RuntimeError('NanoKVM authentication failed')
    return result['data']['token']


def parse_roi(value):
    if not value:
        return None
    parts = tuple(int(item) for item in value.split(','))
    if len(parts) != 4 or any(item < 0 for item in parts) or not all(parts[2:]):
        raise argparse.ArgumentTypeError('ROI must be x,y,width,height')
    return parts


def image_signature(jpeg, roi):
    if roi is None:
        return None
    from PIL import Image
    with Image.open(io.BytesIO(jpeg)) as image:
        x, y, width, height = roi
        if x + width > image.width or y + height > image.height:
            raise ValueError(f'ROI {roi} exceeds captured image {image.size}')
        return list(image.crop((x, y, x + width, y + height)).convert('L').resize((64, 36)).getdata())


async def capture_status(host, token):
    import websockets
    statuses = []
    async with websockets.connect('ws://' + host + '/api/ws',
                                  extra_headers={'Cookie': 'nano-kvm-token=' + token},
                                  open_timeout=5, close_timeout=1) as socket:
        for _ in range(3):
            event = json.loads(await asyncio.wait_for(socket.recv(), timeout=5))
            if event.get('type') == 'capture-status':
                status = json.loads(event['data'])
                statuses.append({key: status.get(key) for key in ('mode', 'ok', 'result', 'message', 'updatedAt')})
    return statuses


def capture(args):
    password = os.environ.get('SUMMIT_NANOKVM_PASSWORD')
    if not password:
        raise SystemExit('Set SUMMIT_NANOKVM_PASSWORD in the environment.')
    base = 'http://' + args.host
    token = authenticate(base, args.username, password)
    if args.status:
        print(json.dumps(asyncio.run(capture_status(args.host, token)), indent=1))
        return
    request = urllib.request.Request(base + '/api/stream/mjpeg',
                                     headers={'Cookie': 'nano-kvm-token=' + token})
    args.output.mkdir(parents=True, exist_ok=True)
    frames = []
    buffer = bytearray()
    previous_signature = None
    started = time.monotonic()
    try:
        response = urllib.request.urlopen(request, timeout=10)
    except TimeoutError as error:
        raise SystemExit('NanoKVM did not start the MJPEG stream; check --status for capture errors.') from error
    with response:
        content_type = response.headers.get('Content-Type', '')
        if 'multipart' not in content_type.lower():
            raise RuntimeError(f'Unexpected MJPEG content type: {content_type}')
        while time.monotonic() - started < args.duration:
            chunk = response.read(16384)
            if not chunk:
                break
            buffer.extend(chunk)
            while True:
                start = buffer.find(b'\xff\xd8')
                if start < 0:
                    del buffer[:-1]
                    break
                end = buffer.find(b'\xff\xd9', start + 2)
                if end < 0:
                    if start:
                        del buffer[:start]
                    break
                jpeg = bytes(buffer[start:end + 2])
                del buffer[:end + 2]
                signature = image_signature(jpeg, args.roi)
                difference = None
                if signature is not None and previous_signature is not None:
                    difference = round(sum(abs(a - b) for a, b in zip(signature, previous_signature)) / len(signature), 3)
                if signature is not None:
                    previous_signature = signature
                frame = {'atMs': round((time.monotonic() - started) * 1000, 2),
                         'bytes': len(jpeg), 'sha256': hashlib.sha256(jpeg).hexdigest(),
                         'roiMeanDifference': difference}
                frames.append(frame)
                if args.save_every and (len(frames) - 1) % args.save_every == 0:
                    (args.output / f'frame-{len(frames):05d}.jpg').write_bytes(jpeg)
    result = {'host': args.host, 'durationSeconds': round(time.monotonic() - started, 3),
              'roi': args.roi, 'frames': frames}
    (args.output / 'capture.json').write_text(json.dumps(result, indent=1) + '\n')
    print(json.dumps({'frames': len(frames), 'durationSeconds': result['durationSeconds'],
                      'output': str(args.output)}, indent=1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='192.168.1.22')
    parser.add_argument('--username', default='admin')
    parser.add_argument('--duration', type=float, default=20)
    parser.add_argument('--output', type=pathlib.Path, help='Capture output directory')
    parser.add_argument('--status', action='store_true', help='Read the device capture status without streaming')
    parser.add_argument('--roi', type=parse_roi, help='Video region x,y,width,height in capture pixels')
    parser.add_argument('--save-every', type=int, default=0, help='Save every Nth JPEG (0 saves none)')
    args = parser.parse_args()
    if args.duration <= 0 or args.save_every < 0:
        parser.error('Duration must be positive and save-every nonnegative')
    if not args.status and not args.output:
        parser.error('--output is required for a capture')
    capture(args)


if __name__ == '__main__':
    main()

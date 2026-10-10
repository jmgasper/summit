#!/usr/bin/env python3
"""Compare actual AMD RGB captures with independently decoded YUV + color math.

--fallback-after also checks the software suffix against FFmpeg's fast bilinear
conversion. A max channel error of 1 is allowed for hardware fixed-point
rounding; software allows 3 across libswscale versions. Never shift frame indices.
"""
import argparse
import json
from pathlib import Path
import subprocess
import numpy as np

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('capture', type=Path)
parser.add_argument('--fallback-after', type=int)
parser.add_argument('--start-frame', type=int, default=0)
parser.add_argument('--frame-count', type=int,
    help='compare this many source pictures, for a captured paused browser frame')
args = parser.parse_args()
assert args.start_frame >= 0
assert args.frame_count is None or args.frame_count > 0
stream = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
    '-show_streams', '-of', 'json', str(args.source)]))['streams'][0]
w, h = stream['width'], stream['height']
assert w % 2 == h % 2 == 0
assert stream['pix_fmt'] in ('yuv420p', 'yuvj420p')
yuv = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(args.source), '-an',
    '-pix_fmt', stream['pix_fmt'], '-fps_mode', 'passthrough', '-f', 'rawvideo', '-'])
area = w * h
frames = np.frombuffer(yuv, np.uint8).reshape(-1, area * 3 // 2)[args.start_frame:]
if args.frame_count is not None:
    frames = frames[:args.frame_count]
actual = np.fromfile(args.capture, np.uint8).reshape(-1, h, w, 4)
assert len(actual) == len(frames), (len(actual), len(frames))
assert np.all(actual[..., 3] == 255), 'opaque alpha'
full = stream.get('color_range') == 'pc'
matrix = stream.get('color_space', 'unknown')
assert matrix in ('unknown', 'bt709', 'smpte170m', 'bt470bg'), matrix
kr, kb = (0.2126, 0.0722) if matrix == 'bt709' or (matrix == 'unknown' and h > 576) else (0.299, 0.114)
y = frames[:, :area].reshape(-1, h, w).astype(np.float64)
u = frames[:, area:area * 5 // 4].reshape(-1, h // 2, w // 2).repeat(2, 1).repeat(2, 2).astype(np.float64)
v = frames[:, area * 5 // 4:].reshape(-1, h // 2, w // 2).repeat(2, 1).repeat(2, 2).astype(np.float64)
y = (y - (0 if full else 16)) / (255 if full else 219)
u = (u - 128) / (255 if full else 224)
v = (v - 128) / (255 if full else 224)
r, b = y + 2 * (1 - kr) * v, y + 2 * (1 - kb) * u
g = (y - kr * r - kb * b) / (1 - kr - kb)
reference = np.floor(np.clip(np.stack([b, g, r], axis=-1), 0, 1) * 255 + 0.5).astype(np.int16)
split = args.fallback_after if args.fallback_after is not None else len(frames)
assert 0 <= split <= len(frames)
delta = np.abs(actual[:split, ..., :3].astype(np.int16) - reference[:split])
assert delta.max(initial=0) <= 1, ('hardware pixel mismatch', int(delta.max()), np.unravel_index(delta.argmax(), delta.shape))
print(f'Hardware: {split} frames, {matrix}, full={full}, max channel error={delta.max(initial=0)}')
if split < len(frames):
    software = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(args.source), '-an',
        '-vf', 'scale=flags=fast_bilinear', '-pix_fmt', 'bgra', '-fps_mode', 'passthrough', '-f', 'rawvideo', '-'])
    software = np.frombuffer(software, np.uint8).reshape(-1, h, w, 4)[args.start_frame:]
    if args.frame_count is not None:
        software = software[:args.frame_count]
    delta = np.abs(actual[split:, ..., :3].astype(np.int16) - software[split:, ..., :3].astype(np.int16))
    assert delta.max(initial=0) <= 3, ('software pixel/order mismatch', int(delta.max()), np.unravel_index(delta.argmax(), delta.shape))
    print(f'Software: {len(frames) - split} frames, max channel error={delta.max(initial=0)}')
print(f'PASS: all {len(frames)} pictures in their original order, complete RGB payload and alpha')

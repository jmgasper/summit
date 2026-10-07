#!/usr/bin/env python3
"""Generate WebM streaming media and serve actual MSE playback checks."""
import argparse
import http.server
import json
import pathlib
import subprocess
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[2]


def generate(directory):
    directory.mkdir(parents=True, exist_ok=True)
    cases = []
    for video, encoder, codec in [('vp8', 'libvpx', 'vp8'), ('vp9', 'libvpx-vp9', 'vp09.00.10.08'),
                                   ('av1', 'libaom-av1', 'av01.0.04M.08')]:
        for audio, audio_encoder in [('opus', 'libopus'), ('vorbis', 'libvorbis')]:
            filename = video + '-' + audio + '.webm'
            command = ['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                       'testsrc2=size=160x90:rate=24', '-f', 'lavfi', '-i',
                       'sine=frequency=701:sample_rate=48000', '-t', '4', '-af', 'volume=0.15',
                       '-c:v', encoder, '-threads', '2', '-g', '12', '-b:v', '160k',
                       '-c:a', audio_encoder, '-b:a', '64k', '-cluster_time_limit', '250']
            if video == 'av1':
                command += ['-cpu-used', '8']
            subprocess.run(command + [str(directory / filename)], check=True)
            cases.append({'file': filename, 'type': f'video/webm; codecs="{codec},{audio}"', 'video': True})
    for audio, encoder in [('opus', 'libopus'), ('vorbis', 'libvorbis')]:
        filename = audio + '.webm'
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                        'sine=frequency=911:sample_rate=48000', '-t', '4', '-af', 'volume=0.15', '-ac', '2',
                        '-c:a', encoder, '-b:a', '96k', str(directory / filename)], check=True)
        cases.append({'file': filename, 'type': f'audio/webm; codecs="{audio}"', 'video': False})
    cases += [{**cases[0], 'changeType': True}, {**cases[6], 'offset': 5},
              {**cases[2], 'abort': True}, {**cases[7], 'abort': True},
              {**cases[6], 'window': [.4, 3.25]}, {**cases[7], 'window': [.4, 3.25]}]
    for codec, encoder, extension, mime in [('aac', 'aac', 'aac', 'audio/aac'),
                                             ('mp3', 'libmp3lame', 'mp3', 'audio/mpeg'),
                                             ('mp2', 'mp2', 'mp2', 'audio/mpeg')]:
        filename = 'raw-' + codec + '.' + extension
        command = ['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                   'sine=frequency=523:sample_rate=48000', '-t', '4', '-af', 'volume=0.15',
                   '-c:a', encoder, '-b:a', '96k']
        if codec == 'mp3':
            command += ['-write_xing', '0']
        subprocess.run(command + [str(directory / filename)], check=True)
        cases.append({'file': filename, 'type': mime, 'video': False, 'raw': True})
    cases += [{**cases[-3], 'type': 'Audio/AAC', 'abort': True},
              {**cases[-3], 'changeType': True}]
    (directory / 'raw-metadata.mp3').write_bytes(b'ICY 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n'
                                               + (directory / 'raw-mp3.mp3').read_bytes() + b'TAG' + bytes(125))
    cases.append({'file': 'raw-metadata.mp3', 'type': 'audio/mpeg', 'video': False, 'raw': True})
    cases.append({**cases[6], 'after': {'file': 'raw-mp2.mp2', 'type': 'audio/mpeg'}, 'expectedEnd': 8, 'seek': 6})
    for duration in ('2.5', '5', '120'):
        filename = 'opus-' + duration + 'ms.webm'
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                        'sine=frequency=911:sample_rate=48000', '-t', '4', '-af', 'volume=0.15', '-ac', '2',
                        '-c:a', 'libopus', '-frame_duration', duration, '-b:a', '96k',
                        str(directory / filename)], check=True)
        cases.append({'file': filename, 'type': 'audio/webm; codecs="opus"', 'video': False, 'fullPlayback': True})
    tones = '|'.join(f'0.015*sin(2*PI*{frequency}*t)' for frequency in (211, 401, 601, 71, 1009, 1511))
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', f'aevalsrc={tones}:s=48000:c=5.1',
                    '-t', '4', '-c:a', 'libopus', '-mapping_family', '1', '-b:a', '768k', str(directory / 'opus-surround.webm')], check=True)
    cases.append({'file': 'opus-surround.webm', 'type': 'audio/webm; codecs="opus"', 'video': False})
    for video, encoder, codec in [('hevc', 'libx265', 'hvc1.1.6.L30.B0'),
                                   ('vp9', 'libvpx-vp9', 'vp09.00.10.08'), ('av1', 'libaom-av1', 'av01.0.04M.08')]:
        filename = video + '.mp4'
        command = ['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                   'testsrc2=size=160x90:rate=24', '-t', '4', '-c:v', encoder, '-threads', '2',
                   '-g', '12', '-b:v', '160k', '-movflags', 'empty_moov+frag_keyframe+default_base_moof']
        if video == 'hevc':
            command += ['-x265-params', 'pools=2:log-level=error', '-tag:v', 'hvc1']
        if video == 'av1':
            command += ['-cpu-used', '8']
        subprocess.run(command + [str(directory / filename)], check=True)
        cases.append({'file': filename, 'type': f'video/mp4; codecs="{codec}"', 'video': True, 'mp4': True})
    for encoder, codec in [('ac3', 'ac-3'), ('eac3', 'ec-3')]:
        filename = encoder + '.mp4'
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                        'sine=frequency=701:sample_rate=48000', '-t', '4', '-af', 'volume=0.15', '-ac', '2',
                        '-c:a', encoder, '-b:a', '128k', '-movflags', 'delay_moov+empty_moov+frag_keyframe+default_base_moof',
                        str(directory / filename)], check=True)
        cases.append({'file': filename, 'type': f'audio/mp4; codecs="{codec}"', 'video': False, 'mp4': True})
    cases.append({**cases[14], 'deferEnd': True})
    for case in cases:
        if case.get('mp4'):
            case['initLength'] = (directory / case['file']).read_bytes().index(b'moof') - 4
            reference = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_packets', '-of', 'json', str(directory / case['file'])]))
            case['start'] = min(float(packet['pts_time']) for packet in reference['packets'])
            case['expectedEnd'] = max(float(packet['pts_time']) + float(packet.get('duration_time', 1 / 24 if case['video'] else 1536 / 48000)) for packet in reference['packets'])
        else:
            case['initLength'] = 0 if case.get('raw') else (directory / case['file']).read_bytes().index(bytes.fromhex('1f43b675'))
    (directory / 'cases.json').write_text(json.dumps(cases, indent=2) + '\n')
    return cases


def serve(directory, cases, host, port):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            if not self.path.startswith('/result/') or self.path[8:] not in {str(i) for i in range(len(cases))}:
                self.send_error(404); return
            size = int(self.headers.get('Content-Length', 0))
            if size > 100000:
                self.send_error(400); return
            result = json.loads(self.rfile.read(size))
            (directory / ('result-' + self.path[8:] + '.json')).write_text(json.dumps(result, indent=2) + '\n')
            self.send_response(204); self.end_headers()

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            if url.path == '/test.html':
                data, mime = (ROOT / 'tests/fixtures/streaming.html').read_bytes(), 'text/html; charset=utf-8'
            elif url.path == '/cases.json':
                data, mime = json.dumps(cases).encode(), 'application/json'
            elif url.path.startswith('/media/') and url.path[7:] in {case['file'] for case in cases}:
                data, mime = (directory / url.path[7:]).read_bytes(), 'video/webm'
            else:
                self.send_error(404); return
            self.send_response(200)
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass
    http.server.ThreadingHTTPServer((host, port), Handler).serve_forever()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=pathlib.Path, default=pathlib.Path('.vm/streaming-fixture'))
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=8777)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    cases = generate(args.directory)
    if not args.generate_only:
        serve(args.directory, cases, args.host, args.port)

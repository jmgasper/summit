#!/usr/bin/env python3
"""Generate independently packaged Clear Key media; serve on trusted localhost.

Requires FFmpeg and Shaka Packager (pass --packager /path/to/packager).
Run --serve on the Haiku workstation so http://127.0.0.1:8773 is a secure
context without disabling browser security settings or installing a test CA.
"""
import argparse
import http.server
import importlib.util
import pathlib
import shutil
import subprocess
import urllib.parse


def generate(directory, packager, source):
    spec = importlib.util.spec_from_file_location('dash_fixture', pathlib.Path(__file__).with_name('dash-fixture.py'))
    dash = importlib.util.module_from_spec(spec); spec.loader.exec_module(dash)
    dash.generate(source)
    directory.mkdir(parents=True, exist_ok=True)
    for encrypted in [False, True]:
        prefix = '' if encrypted else 'clear-'
        streams = [f'in={source}/range-stream{index}.mp4,stream={track},init_segment={directory}/{prefix}{track}-init.mp4,segment_template={directory}/{prefix}{track}-$Number$.m4s,drm_label={label}'
            for track, index, label in [('video', 0, 'V'), ('audio', 2, 'A')]]
        command = [str(packager), *streams, '--segment_duration', '2', '--generate_static_live_mpd', '--mpd_output', str(directory / ('manifest.mpd' if encrypted else 'clear.mpd'))]
        if encrypted:
            command += ['--enable_raw_key_encryption', '--keys', 'label=V:key_id=00112233445566778899aabbccddeeff:key=000102030405060708090a0b0c0d0e0f,label=A:key_id=ffeeddccbbaa99887766554433221100:key=0f0e0d0c0b0a09080706050403020100', '--protection_systems', 'CommonSystem', '--protection_scheme', 'cenc', '--clear_lead', '0']
        subprocess.run(command, check=True)
    subprocess.run([str(packager),
        f'in={source}/range-stream0.mp4,stream=video,init_segment={directory}/iv16-video-init.mp4,segment_template={directory}/iv16-video-$Number$.m4s',
        '--enable_raw_key_encryption', '--keys', 'key_id=00112233445566778899aabbccddeeff:key=000102030405060708090a0b0c0d0e0f',
        '--protection_systems', 'CommonSystem', '--protection_scheme', 'cenc', '--clear_lead', '0',
        '--iv', '0102030405060708090a0b0c0d0e0f10', '--segment_duration', '2', '--generate_static_live_mpd', '--mpd_output', str(directory / 'iv16.mpd')], check=True)
    shutil.copyfile(pathlib.Path(__file__).parent / 'pages/eme-fixture.html', directory / 'test.html')


def serve(directory, host, port):
    class Handler(http.server.SimpleHTTPRequestHandler):
        extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, '.mpd': 'application/dash+xml', '.m4s': 'video/iso.segment'}
        def __init__(self, *args, **kwargs): super().__init__(*args, directory=str(directory), **kwargs)
        def translate_path(self, path):
            if path.startswith('/cors/'): path = path[5:]
            return super().translate_path(path)
        def do_GET(self):
            if urllib.parse.urlsplit(self.path).path == '/cross-clear.mpd':
                data = (directory / 'clear.mpd').read_bytes()
                prefix = f'http://localhost:{port}/'.encode()
                for attribute in [b'initialization="', b'media="']:
                    data = data.replace(attribute, attribute + prefix)
                self.send_response(200)
                self.send_header('Content-Type', 'application/dash+xml')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            super().do_GET()
        def end_headers(self):
            self.send_header('Cache-Control', 'no-store')
            if self.path.startswith('/cors/'):
                self.send_header('Access-Control-Allow-Origin', '*')
            policy = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query).get('policy', [''])[0]
            policies = {'deny': '()', 'self': '(self)', 'all': '*',
                'origins': f'(self "http://localhost:{port}")', 'other': '("https://example.invalid")',
                'path': f'("http://127.0.0.1:{port}/not-an-origin")'}
            if policy in policies:
                self.send_header('Permissions-Policy', 'encrypted-media=' + policies[policy])
            super().end_headers()
    http.server.ThreadingHTTPServer((host, port), Handler).serve_forever()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=pathlib.Path, required=True)
    parser.add_argument('--source', type=pathlib.Path, default=pathlib.Path('.vm/issue33-fixture'))
    parser.add_argument('--packager', type=pathlib.Path)
    parser.add_argument('--serve', action='store_true')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8773)
    args = parser.parse_args()
    if args.packager: generate(args.directory.resolve(), args.packager.resolve(), args.source.resolve())
    if args.serve: serve(args.directory.resolve(), args.host, args.port)

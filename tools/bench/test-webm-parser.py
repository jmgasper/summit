#!/usr/bin/env python3
"""Compare incremental WebM demuxing with FFmpeg, under ASan and UBSan."""
import argparse
import hashlib
import json
import os
import pathlib
import random
import struct
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, **kwargs)


def build(output, engine):
    lib = engine / 'Source/ThirdParty/libwebrtc/Source/third_party'
    webm = lib / 'libwebm/webm_parser'
    port = engine / 'Source/WebCore/platform/graphics/haiku'
    sources = [str(p) for p in sorted((webm / 'src').glob('*.cc'))
               if not p.name.endswith('_test.cc')]
    command = ['g++', '-std=c++23', '-DWEBRTC_WEBKIT_BUILD=1', '-O1', '-g',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
               '-I' + str(webm), '-I' + str(webm / 'include'), '-I' + str(port),
               '-I' + str(ROOT / 'vendor'), '-I' + str(lib / 'opus/src/include'),
               str(ROOT / 'tests/EngineWebMParserTests.cpp'), str(port / 'WebMParserHaiku.cpp'),
               *sources, '-Wl,-l:libopus.so.0', '-ldl', '-lcrypto', '-o', str(output / 'probe')]
    compilation = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (output / 'compile.log').write_text(compilation.stdout)
    compilation.check_returncode()


def fixtures(output):
    for video, encoder in [('vp8', 'libvpx'), ('vp9', 'libvpx-vp9'), ('av1', 'libaom-av1')]:
        for audio, audio_encoder in [('opus', 'libopus'), ('vorbis', 'libvorbis')]:
            path = output / (video + '-' + audio + '.webm')
            command = ['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                       'testsrc2=size=160x90:rate=20', '-f', 'lavfi', '-i',
                       'sine=frequency=701:sample_rate=48000', '-t', '1.3',
                       '-c:v', encoder, '-threads', '2', '-g', '10', '-b:v', '120k',
                       '-c:a', audio_encoder, '-b:a', '64k', '-cluster_time_limit', '200']
            if video == 'av1':
                command += ['-cpu-used', '8']
            run(command + [str(path)])
    for audio, encoder in [('opus', 'libopus'), ('vorbis', 'libvorbis')]:
        run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
             'sine=frequency=911:sample_rate=48000', '-t', '0.75', '-ac', '2',
             '-c:a', encoder, '-b:a', '96k', str(output / (audio + '.webm'))])
    for duration in ('2.5', '5', '120'):
        run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
             'sine=frequency=911:sample_rate=48000', '-t', '0.75', '-ac', '2',
             '-c:a', 'libopus', '-frame_duration', duration, '-b:a', '96k',
             str(output / ('opus-' + duration + 'ms.webm'))])
    tones = '|'.join(f'0.1*sin(2*PI*{frequency}*t)' for frequency in (211, 401, 601, 71, 1009, 1511))
    run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', f'aevalsrc={tones}:s=48000:c=5.1',
         '-t', '0.75', '-c:a', 'libopus', '-mapping_family', '1', '-b:a', '768k', str(output / 'opus-surround.webm')])


def probe(executable, path, chunk, reset=None):
    command = [str(executable), str(path), str(chunk)]
    if reset is not None:
        command.append(str(reset))
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    process = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             env=env, timeout=30)
    if process.returncode not in (0, 1):
        raise RuntimeError(f'{path.name}: probe crashed ({process.returncode}): {process.stderr}')
    return json.loads(process.stdout)


def check_fixture(executable, path):
    reference = json.loads(run(['ffprobe', '-v', 'error', '-show_packets', '-show_streams',
                                '-show_data_hash', 'sha256', '-of', 'json', str(path)]).stdout)
    baseline = probe(executable, path, 1 << 30)
    assert baseline['ok'], (path, baseline)
    assert len(baseline['init']) == 1, path
    tracks = baseline['init'][0]['tracks']
    assert len(tracks) == len(reference['streams']), path
    samples = baseline['samples']
    assert len(samples) == len(reference['packets']), (path, len(samples), len(reference['packets']))
    # Packet order, byte identity, timestamps and random access flags are
    # independent of how the browser's appendBuffer call divides the bytes.
    for ours, theirs in zip(samples, reference['packets']):
        track = tracks[theirs['stream_index']]
        assert ours['track'] == track['id'], path
        assert ours['sha256'] == theirs['data_hash'].split(':', 1)[1].lower(), path
        assert ours['size'] == int(theirs['size']), path
        assert ours['key'] == ('K' in theirs['flags']), path
        assert abs(ours['pts'] / track['scale'] - float(theirs['pts_time'])) < 0.0011, (path, ours, theirs)
        if 'duration_time' in theirs:
            # ffprobe reports integer-millisecond WebM packet duration;
            # audio here preserves exact decoded frame counts.
            assert abs(ours['duration'] / track['scale'] - float(theirs['duration_time'])) < 0.0011, (path, ours, theirs)
    for chunk in (1, 2, 3, 7, 31, 188, 1024, 65536):
        actual = probe(executable, path, chunk)
        assert actual == baseline, (path, chunk, actual.get('failedAt'))
    return {'file': path.name, 'packets': len(samples), 'appendSizes': 9,
            'tracks': tracks, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def vint(value):
    for size in range(1, 9):
        if value < (1 << (7 * size)) - 1:
            return ((1 << (7 * size)) | value).to_bytes(size, 'big')
    raise ValueError(value)


def element(identifier, payload):
    key = identifier.to_bytes((identifier.bit_length() + 7) // 8, 'big')
    return key + vint(len(payload)) + payload


def integer(identifier, value):
    return element(identifier, value.to_bytes(max(1, (value.bit_length() + 7) // 8), 'big'))


def structural_checks(executable, output):
    header = element(0x1a45dfa3, element(0x4282, b'webm') + integer(0x4285, 2))
    segment = bytes.fromhex('18538067') + b'\xff'
    info = element(0x1549a966, integer(0x2ad7b1, 1000000))
    opus_head = b'OpusHead' + bytes([1, 2]) + struct.pack('<HIhB', 312, 48000, 0, 0)
    track = (integer(0xd7, 1) + integer(0x73c5, 1) + integer(0x83, 2)
             + element(0x86, b'A_OPUS') + element(0x63a2, opus_head)
             + integer(0x56aa, 6500000)
             + element(0xe1, element(0xb5, struct.pack('>d', 48000)) + integer(0x9f, 2)))
    tracks = element(0x1654ae6b, element(0xae, track))
    init = header + segment + info + tracks
    packet = bytes.fromhex('f8fffe')  # One valid 20 ms Opus silence packet.

    def cluster(payload, timecode=0, unknown=False):
        body = integer(0xe7, timecode) + payload
        return bytes.fromhex('1f43b675') + b'\xff' + body if unknown else element(0x1f43b675, body)

    def simple(payload=packet, flags=0x80, track_id=1):
        return element(0xa3, vint(track_id) + b'\0\0' + bytes([flags]) + payload)

    count = 0

    def check(name, data, expected=True, reset=None):
        nonlocal count
        path = output / (name + '.bin')
        path.write_bytes(data)
        baseline = probe(executable, path, 1 << 30, reset)
        assert baseline['ok'] == expected, (name, baseline)
        for chunk in (1, 7, 188):
            actual = probe(executable, path, chunk, reset)
            assert actual['ok'] == expected, (name, chunk, actual)
            if expected:
                assert actual == baseline, (name, chunk)
        count += 1
        return baseline

    for name, flags, lace in [('fixed', 0x84, b'\x02'), ('xiph', 0x82, b'\x02\x03\x03'),
                              ('ebml', 0x86, b'\x02\x83\xbf')]:
        parsed = check(name + '-lace', init + cluster(simple(lace + packet * 3, flags)))
        assert [p['pts'] for p in parsed['samples']] == [-312, 648, 1608], name
        assert [p['duration'] for p in parsed['samples']] == [960] * 3, name
    group = element(0xa0, element(0xa1, b'\x81\0\0\0' + packet)
                    + integer(0x9b, 20) + element(0x75a2, (3125000).to_bytes(4, 'big', signed=True)))
    parsed = check('discard-padding', init + cluster(group))
    assert parsed['samples'][0]['trimEnd'] == 150
    for name, padding in [('start', -25000000), ('end', 25000000)]:
        group = element(0xa0, element(0xa1, b'\x81\0\0\x04\x02' + packet * 3)
                        + element(0x75a2, padding.to_bytes(4, 'big', signed=True)))
        parsed = check('laced-discard-' + name, init + cluster(group))
        assert [p['trimStart'] for p in parsed['samples']] == ([960, 552, 0] if name == 'start' else [312, 0, 0])
        assert [p['trimEnd'] for p in parsed['samples']] == ([0, 0, 0] if name == 'start' else [0, 240, 960])
    group = element(0xa0, element(0xa1, b'\x81\0\0\x04\x02' + packet * 3)
                    + element(0x75a2, (-59000000).to_bytes(4, 'big', signed=True)))
    check('discard-overlaps-preskip', init + cluster(group), False)
    parsed = check('unknown-clusters', init + cluster(simple(), unknown=True) + cluster(simple(), 20, True))
    assert len(parsed['samples']) == 2
    parsed = check('new-init', init + cluster(simple()) + init + cluster(simple(), 1000))
    assert len(parsed['init']) == 2 and len(parsed['samples']) == 2
    partial = init + cluster(simple())[:-2]
    parsed = check('abort-frame', partial + cluster(simple(), 1000), reset=len(partial))
    assert len(parsed['init']) == 1 and len(parsed['samples']) == 1
    # Aborting a replacement initialization segment retains the committed
    # track setup, even when its EBML header was already fully parsed.
    partial = init + cluster(simple()) + header + segment + info + tracks[:-8]
    parsed = check('abort-new-init', partial + cluster(simple(), 1000), reset=len(partial))
    assert len(parsed['init']) == 1 and len(parsed['samples']) == 2
    check('missing-info', header + segment + tracks, False)
    check('empty-tracks', header + segment + info + element(0x1654ae6b, b''), False)
    check('duplicate-track', header + segment + info + element(0x1654ae6b, element(0xae, track) * 2), False)
    check('wrong-doctype', init.replace(b'webm', b'xxxx', 1), False)
    check('unsupported-codec', init.replace(b'A_OPUS', b'A_FAKE', 1), False)
    check('bad-opus-header', init.replace(b'OpusHead', b'OpusHxxx', 1), False)
    check('cluster-before-init', header + segment + info + cluster(simple()), False)
    check('missing-timecode', init + element(0x1f43b675, simple()), False)
    check('unknown-track', init + cluster(simple(track_id=2)), False)
    check('overflow-track', init + cluster(simple(track_id=(1 << 32) + 1)), False)
    check('bad-opus-frame', init + cluster(simple(b'\xff')), False)
    check('bad-lace', init + cluster(simple(b'\x02' + packet * 3 + b'\0', 0x84)), False)
    check('timestamp-overflow', init + cluster(simple(), (1 << 64) - 1), False)
    check('oversized-codec-private', header + segment + info + bytes.fromhex('1654ae6b')
          + vint(100) + b'\xae' + vint(90) + bytes.fromhex('63a2') + vint(2 << 20), False)
    encrypted = element(0xae, track + element(0x6d80, b''))
    check('encrypted-webm', header + segment + info + element(0x1654ae6b, encrypted), False)
    # Reproducible malformed-input smoke coverage. All outcomes are allowed;
    # a sanitizer error, signal, or excessive parse time always fails.
    rng = random.Random(47)
    seed = init + cluster(simple(packet * 3))
    for index in range(128):
        mutated = bytearray(seed)
        for _ in range(rng.randint(1, 6)):
            mutated[rng.randrange(len(mutated))] = rng.randrange(256)
        if index % 3 == 0:
            mutated = mutated[:rng.randrange(len(mutated))]
        path = output / 'mutation.bin'
        path.write_bytes(mutated)
        probe(executable, path, (1, 7, 188)[index % 3])
    return {'structuralCases': count, 'mutations': 128}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=pathlib.Path, default=ROOT / '.cache/WebKit')
    parser.add_argument('--output', type=pathlib.Path, default=ROOT / '.vm/webm-parser')
    parser.add_argument('--no-build', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if not args.no_build:
        build(args.output, args.engine)
    fixtures(args.output)
    results = []
    for path in sorted(args.output.glob('*.webm')):
        results.append(check_fixture(args.output / 'probe', path))
        print(path.name, results[-1]['packets'], 'packets PASS', flush=True)
    structure = structural_checks(args.output / 'probe', args.output)
    print('Structure/abort/malformed-input PASS:', structure, flush=True)
    (args.output / 'result.json').write_text(json.dumps({'fixtures': results, **structure}, indent=2) + '\n')
    print('WebM parser PASS:', len(results), 'fixtures')


if __name__ == '__main__':
    main()

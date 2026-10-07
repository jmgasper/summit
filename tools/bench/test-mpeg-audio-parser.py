#!/usr/bin/env python3
"""Check incremental ADTS/MPEG audio packets, metadata and malformed input."""
import argparse
import importlib.util
import json
import pathlib
import random
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('webm_test', pathlib.Path(__file__).with_name('test-webm-parser.py'))
common = importlib.util.module_from_spec(spec)
spec.loader.exec_module(common)


def build(output, engine):
    port = engine / 'Source/WebCore/platform/graphics/haiku'
    command = ['g++', '-std=c++23', '-DSUMMIT_TEST_MPEG_AUDIO=1', '-O1', '-g',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
               '-I' + str(port), '-I' + str(ROOT / 'vendor'),
               str(ROOT / 'tests/EngineWebMParserTests.cpp'), str(port / 'MPEGAudioParserHaiku.cpp'),
               '-lcrypto', '-o', str(output / 'probe')]
    compilation = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (output / 'compile.log').write_text(compilation.stdout)
    compilation.check_returncode()


def generate(output):
    paths = []
    for codec, encoder, extension in [('aac', 'aac', 'aac'), ('mp3', 'libmp3lame', 'mp3'), ('mp2', 'mp2', 'mp2')]:
        for rate in ([48000, 44100, 22050, 8000] if codec != 'mp2' else [48000, 44100, 24000]):
            path = output / f'{codec}-{rate}.{extension}'
            command = ['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', f'sine=frequency=701:sample_rate={rate}',
                       '-t', '0.7', '-ac', '2', '-c:a', encoder, '-b:a', '64k']
            if codec == 'mp3':
                command += ['-write_xing', '0', '-id3v2_version', '4']
            subprocess.run(command + [str(path)], check=True)
            paths.append(path)
    # A valid Layer I silence frame: every subband's bit allocation is zero.
    # This exercises a decoder-supported layer for which FFmpeg has no encoder.
    frame = bytes.fromhex('ffff4000') + bytes(132)
    path = output / 'layer1.mp1'
    path.write_bytes(frame * 20)
    paths.append(path)
    return paths


def verify(executable, path):
    reference = json.loads(common.run(['ffprobe', '-v', 'error', '-show_packets', '-show_streams',
                                      '-show_data_hash', 'sha256', '-of', 'json', str(path)]).stdout)
    baseline = common.probe(executable, path, 1 << 30)
    assert baseline['ok'] and len(baseline['init']) == 1, (path, baseline)
    samples = baseline['samples']
    assert len(samples) == len(reference['packets']), (path, len(samples), len(reference['packets']))
    track = baseline['init'][0]['tracks'][0]
    elapsed = 0
    for ours, theirs in zip(samples, reference['packets']):
        assert ours['sha256'] == theirs['data_hash'].split(':', 1)[1].lower(), (path, ours, theirs)
        assert ours['size'] == int(theirs['size']) and ours['key'], path
        assert ours['pts'] == elapsed and ours['dts'] == elapsed, path
        assert abs(ours['duration'] / track['scale'] - float(theirs['duration_time'])) < .000002, (path, ours, theirs)
        elapsed += ours['duration']
    for chunk in [1, 2, 3, 7, 31, 188, 1024, 65536]:
        assert common.probe(executable, path, chunk) == baseline, (path, chunk)
    return baseline


def structural(executable, output, reference):
    seed = output / 'mp3-48000.mp3'
    data = seed.read_bytes()
    # Strip generated ID3 so synthetic metadata can also be inserted between
    # valid frames. Every metadata case must retain the same media packets.
    if data.startswith(b'ID3'):
        size = 10 + sum(data[6 + i] << (21 - i * 7) for i in range(4))
        data = data[size:]
    first_size = reference['samples'][0]['size']
    count = 0

    def check(name, content, expected=True, compare=None, extension='mp3', reset=None):
        nonlocal count
        path = output / (name + '.' + extension)
        path.write_bytes(content)
        baseline = common.probe(executable, path, 1 << 30, reset)
        assert baseline['ok'] == expected, (name, baseline)
        if compare is not None:
            assert baseline == compare, name
        for chunk in (1, 7, 188):
            actual = common.probe(executable, path, chunk, reset)
            assert actual['ok'] == expected, (name, chunk, actual)
            if expected:
                assert actual == baseline, (name, chunk)
        count += 1
        return baseline

    plain = check('plain', data)
    id3 = b'ID3\x04\0\x10\0\0\0\x04abcd' + b'3DI\x04\0\x10\0\0\0\x04'
    metadata = id3 + b'TAG' + bytes(125) + b'ICY 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n'
    check('metadata-prefix', metadata + data, compare=plain)
    check('metadata-between', data[:first_size] + metadata + data[first_size:], compare=plain)
    check('metadata-tail', data + metadata, compare=plain)
    # A large ID3 body must be skipped without retaining it in parser storage.
    large = b'ID3\x03\0\0\0\x10\0\0' + bytes(262144)
    check('large-id3', large + data, compare=plain)
    check('bad-id3-version', b'ID3\xff\0\0\0\0\0\0' + data, False)
    check('bad-id3-size', b'ID3\x04\0\0\x80\0\0\0' + data, False)
    check('oversized-icy', b'ICY ' + b'a' * 65536, False)
    check('bad-sync', b'\xff\xe0\0\0', False)
    check('bad-frequency', bytes.fromhex('fffbfc00'), False)
    check('bad-adts-profile', bytes.fromhex('fff10080013ffc') + bytes(20), False, extension='aac')
    check('bad-adts-size', bytes.fromhex('fff14c80001ffc'), False, extension='aac')
    partial = data[:first_size] + data[first_size:first_size + 8]
    parsed = check('abort-frame', partial + data[first_size:], reset=len(partial))
    assert len(parsed['samples']) == len(plain['samples'])
    assert parsed['samples'][1]['pts'] == 0  # MSE's generated timestamps resume independently.
    # Free-format Layer I frames differ only in bitrate-index bits. Their
    # valid all-zero payload makes the inferred boundary deterministic.
    free = (bytes.fromhex('ffff0000') + bytes(132)) * 20
    parsed = check('free-format', free)
    assert len(parsed['samples']) == 20 and all(s['duration'] == 384 for s in parsed['samples'])
    rng = random.Random(471)
    path = output / 'mutation.mp3'
    for _ in range(128):
        changed = bytearray(data[:first_size * 3])
        for _ in range(rng.randint(1, 5)):
            changed[rng.randrange(len(changed))] = rng.randrange(256)
        path.write_bytes(changed)
        common.probe(executable, path, rng.choice([1, 7, 188]))
    return {'structuralCases': count, 'mutations': 128}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=pathlib.Path, default=ROOT / '.cache/WebKit')
    parser.add_argument('--output', type=pathlib.Path, default=ROOT / '.vm/mpeg-audio-parser')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    build(args.output, args.engine)
    results = []
    for path in generate(args.output):
        result = verify(args.output / 'probe', path)
        results.append({'file': path.name, 'packets': len(result['samples']), 'appendSizes': 9})
        print(path.name, len(result['samples']), 'packets PASS', flush=True)
        if path.name == 'mp3-48000.mp3':
            mp3 = result
    report = {'fixtures': results, **structural(args.output / 'probe', args.output, mp3)}
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('MPEG audio parser PASS', report['structuralCases'], 'structural cases', flush=True)


if __name__ == '__main__':
    main()

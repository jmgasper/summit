#!/usr/bin/env python3
"""Check incremental TS demuxing against FFmpeg, with sanitizers and bad input."""
import argparse
from collections import defaultdict
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def run(args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def packet_bytes(packet):
    return bytes.fromhex(''.join(line.split(':', 1)[1].strip().split('  ', 1)[0]
                                for line in packet['data'].splitlines() if ':' in line))


def avc_bytes(data):
    units = re.split(b'\x00\x00\x00?\x01', data)[1:]
    return b''.join(len(unit.rstrip(b'\x00')).to_bytes(4, 'big') + unit.rstrip(b'\x00') for unit in units)


def fnv(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


ROLLOVER_SHIFT = (1 << 33) - 126000 - 90000


def shift_transport(data, shift):
    data = bytearray(data)
    mask = (1 << 33) - 1
    def timestamp(offset):
        value = ((data[offset] >> 1 & 7) << 30 | data[offset + 1] << 22
                 | (data[offset + 2] >> 1) << 15 | data[offset + 3] << 7 | data[offset + 4] >> 1)
        value = (value + shift) & mask
        data[offset] = (data[offset] & 0xf1) | (value >> 29 & 14)
        data[offset + 1] = value >> 22 & 255
        data[offset + 2] = (value >> 14 & 254) | 1
        data[offset + 3] = value >> 7 & 255
        data[offset + 4] = (value << 1 & 254) | 1
    for base in range(0, len(data), 188):
        control = data[base + 3] >> 4 & 3
        offset = 4
        if control & 2:
            length = data[base + 4]
            if length and data[base + 5] & 0x10:
                p = base + 6
                value = (data[p] << 25 | data[p + 1] << 17 | data[p + 2] << 9
                         | data[p + 3] << 1 | data[p + 4] >> 7)
                value = (value + shift) & mask
                data[p:p + 4] = bytes([value >> 25, value >> 17 & 255, value >> 9 & 255, value >> 1 & 255])
                data[p + 4] = (data[p + 4] & 127) | (value & 1) << 7
            offset += 1 + length
        if control & 1 and data[base + 1] & 0x40 and data[base + offset:base + offset + 3] == b'\0\0\1':
            flags = data[base + offset + 7] >> 6
            if flags & 2:
                timestamp(base + offset + 9)
            if flags == 3:
                timestamp(base + offset + 14)
    return bytes(data)


def generate(directory):
    fixtures = []
    choices = [('h264-aac', 'libx264', 'aac', 48000), ('hevc-aac', 'libx265', 'aac', 48000),
               ('h264-mp3', 'libx264', 'libmp3lame', 44100), ('h264-mp2', 'libx264', 'mp2', 48000),
               ('h264-ac3', 'libx264', 'ac3', 48000), ('h264-eac3', 'libx264', 'eac3', 48000),
               ('aac-44100', None, 'aac', 44100), ('aac-8000', None, 'aac', 8000),
               ('mp3-8000', None, 'libmp3lame', 8000)]
    for name, video, audio, rate in choices:
        path = directory / (name + '.ts')
        command = ['ffmpeg', '-v', 'error', '-y']
        if video:
            command += ['-f', 'lavfi', '-i', 'testsrc2=size=160x90:rate=20']
        command += ['-f', 'lavfi', '-i', f'sine=frequency=701:sample_rate={rate}', '-t', '2',
                    '-c:a', audio, '-b:a', '32k' if rate == 8000 else '96k', '-ac', '2']
        if video:
            command += ['-c:v', video, '-threads', '2', '-g', '10', '-bf', '2', '-b:v', '160k']
            if video == 'libx265':
                command += ['-x265-params', 'pools=2:log-level=error']
        command += ['-mpegts_flags', 'pat_pmt_at_frames', '-f', 'mpegts', str(path)]
        run(command)
        fixtures.append(path)
    rollover = directory / 'rollover.ts'
    rollover.write_bytes(shift_transport((directory / 'h264-aac.ts').read_bytes(), ROLLOVER_SHIFT))
    fixtures.append(rollover)
    return fixtures


def structural(executable, directory, env):
    source = (directory / 'h264-aac.ts').read_bytes()
    packets = [bytearray(source[offset:offset + 188]) for offset in range(0, len(source), 188)]
    def pid(packet):
        return (packet[1] & 31) << 8 | packet[2]
    def payload(packet):
        return 5 + packet[4] if packet[3] & 0x20 else 4
    pat = next(i for i, packet in enumerate(packets) if pid(packet) == 0)
    pat_start = payload(packets[pat]) + 1 + packets[pat][payload(packets[pat])]
    pmt_pid = (packets[pat][pat_start + 10] & 31) << 8 | packets[pat][pat_start + 11]
    pmt = next(i for i, packet in enumerate(packets) if pid(packet) == pmt_pid)
    media = next(i for i, packet in enumerate(packets) if pid(packet) == 256 and packet[1] & 0x40)
    audio = next(i for i, packet in enumerate(packets) if pid(packet) == 257 and packet[1] & 0x40)
    def change(index, offset, value):
        mutated = bytearray(source)
        mutated[index * 188 + offset] = value
        return bytes(mutated)
    invalid = {
        'sync': change(0, 0, 0x46),
        'transport-error': change(media, 1, packets[media][1] | 0x80),
        'scrambled': change(media, 3, packets[media][3] | 0x80),
        'reserved-adaptation': change(media, 3, packets[media][3] & 0xcf),
        'oversized-adaptation': change(media, 4, 184),
        'bad-pat-crc': change(pat, pat_start + 3, packets[pat][pat_start + 3] ^ 1),
        'no-pat': b''.join(packet for packet in packets if pid(packet) != 0),
        'no-pmt': b''.join(packet for packet in packets if pid(packet) != pmt_pid),
        'no-pts': change(media, payload(packets[media]) + 7, packets[media][payload(packets[media]) + 7] & 0x3f),
        'bad-pts-marker': change(media, payload(packets[media]) + 9, packets[media][payload(packets[media]) + 9] & 0xfe),
        'partial-ts': source[:-1],
        'partial-pes': b''.join(packets[:audio + 1]),
        'lost-packet': b''.join(packets[:media + 1] + packets[media + 2:]),
    }
    no_pcr = [bytearray(packet) for packet in packets]
    for packet in no_pcr:
        if packet[3] & 0x20 and packet[4]:
            packet[5] &= 0xef
    invalid['no-pcr'] = b''.join(no_pcr)
    def crc(section):
        value = 0xffffffff
        for byte in section:
            value ^= byte << 24
            for _ in range(8):
                value = ((value << 1) ^ (0x04c11db7 if value & 0x80000000 else 0)) & 0xffffffff
        return value.to_bytes(4, 'big')
    first = packets[pat]
    length = ((first[pat_start + 1] & 15) << 8 | first[pat_start + 2]) + 3
    table = bytearray(first[pat_start:pat_start + length - 4])
    table += bytes([0, 2, 0xf0, 1])
    table[2] += 4
    table += crc(table)
    multiple = bytearray(first)
    multiple[pat_start:] = table + b'\xff' * (188 - pat_start - len(table))
    invalid['multiple-programs'] = b''.join(packets[:pat] + [multiple] + packets[pat + 1:])
    for name, data in invalid.items():
        path = directory / ('invalid-' + name + '.ts')
        path.write_bytes(data)
        for chunk in (1, 65536):
            result = subprocess.run([str(executable), str(path), str(chunk)], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
            assert result.returncode == 1, (name, chunk, result.returncode, result.stderr[-1000:])
            assert not json.loads(result.stdout)['ok'], name
            assert b'Sanitizer' not in result.stderr and b'runtime error:' not in result.stderr, (name, result.stderr)
    duplicate = directory / 'duplicate.ts'
    duplicate.write_bytes(b''.join(packets[:media + 1] + [packets[media]] + packets[media + 1:]))
    original = json.loads(subprocess.check_output([str(executable), str(directory / 'h264-aac.ts'), '188'], env=env))
    repeated = json.loads(subprocess.check_output([str(executable), str(duplicate), '188'], env=env))
    assert original == repeated, 'Duplicate TS packets must not duplicate decoded samples'
    import random
    randomizer = random.Random(47)
    mutation = directory / 'mutation.ts'
    for index in range(128):
        data = bytearray(source)
        for _ in range(1 + index % 5):
            data[randomizer.randrange(len(data))] ^= 1 << randomizer.randrange(8)
        mutation.write_bytes(data)
        result = subprocess.run([str(executable), str(mutation), '65536'], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
        assert result.returncode in (0, 1), (index, result.returncode, result.stderr[-1000:])
        assert b'Sanitizer' not in result.stderr and b'runtime error:' not in result.stderr, (index, result.stderr)
    print('PASS', len(invalid) + 1, 'structural cases and 128 mutations', flush=True)
    return len(invalid) + 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / '.vm/mpegts-parser')
    parser.add_argument('--engine', type=Path, default=ROOT / '.cache/WebKit')
    parser.add_argument('--headers', type=Path, required=True, help='Private public-header prefix from prepare-ffmpeg-headers.py')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    port = args.engine / 'Source/WebCore/platform/graphics/haiku'
    executable = args.output / 'probe'
    run(['g++', '-std=c++23', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
         '-I' + str(args.headers / 'include'), '-I' + str(port), '-I' + str(ROOT / 'vendor'),
         ROOT / 'tests/EngineMPEGTSParserTests.cpp', port / 'MPEGTSParserHaiku.cpp', '-ldl', '-pthread', '-o', executable])
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    results = []
    for path in generate(args.output):
        reference_path = args.output / 'h264-aac.ts' if path.stem == 'rollover' else path
        reference = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_streams', '-show_packets', '-show_data', '-of', 'json', str(reference_path)]))
        streams = {stream['index']: stream for stream in reference['streams']}
        expected = defaultdict(list)
        for packet in reference['packets']:
            stream = streams[packet['stream_index']]
            data = packet_bytes(packet)
            if stream['codec_name'] == 'h264':
                data = avc_bytes(data)
            scale = int(stream['sample_rate']) if stream['codec_type'] == 'audio' else 90000
            num, den = map(int, stream['time_base'].split('/'))
            def rescale(value):
                numerator = int(value) * num * scale
                return (numerator + den // 2) // den if numerator >= 0 else -((-numerator + den // 2) // den)
            shift = ROLLOVER_SHIFT if path.stem == 'rollover' else 0
            expected[int(stream['id'], 16)].append({'pts': rescale(packet['pts'] + shift), 'dts': rescale(packet['dts'] + shift),
                'duration': rescale(packet['duration']), 'sync': 'K' in packet['flags'], 'size': len(data), 'hash': fnv(data)})
        reference_output = None
        for chunk in (1, 2, 3, 7, 187, 188, 189, 4096, 65536):
            completed = run([executable, path, chunk], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
            actual = json.loads(completed.stdout)
            assert actual['ok'] and actual['available'], path.name
            if reference_output is None:
                reference_output = actual
            assert actual == reference_output, (path.name, chunk, 'append-size-dependent output')
        actual_by_id = defaultdict(list)
        for sample in reference_output['samples']:
            sample = dict(sample)
            actual_by_id[sample.pop('id')].append(sample)
        assert set(actual_by_id) == set(expected), path.name
        for track, samples in expected.items():
            assert len(samples) == len(actual_by_id[track]), (path.name, track, len(samples), len(actual_by_id[track]))
            for index, (want, actual) in enumerate(zip(samples, actual_by_id[track])):
                # TS's 90 kHz PTS quantization may differ by one audio sample
                # from the exact PCM clock. No packet bytes or frame is lost.
                for key in ('pts', 'dts', 'duration'):
                    assert abs(want[key] - actual[key]) <= 1, (path.name, track, index, key, want[key], actual[key])
                for key in ('sync', 'size', 'hash'):
                    assert want[key] == actual[key], (path.name, track, index, key)
        results.append({'file': path.name, 'packets': len(reference_output['samples']), 'chunks': 9})
        print('PASS', path.name, len(reference_output['samples']), 'packets', flush=True)
    count = structural(executable, args.output, env)
    (args.output / 'result.json').write_text(json.dumps({'fixtures': results, 'structuralCases': count, 'mutations': 128}, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

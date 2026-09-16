#!/usr/bin/env python3
"""Generate CRX3 signature fixtures with an independent crypto implementation."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa


def varint(value):
    data = bytearray()
    while value > 127:
        data.append((value & 127) | 128)
        value >>= 7
    return bytes(data) + bytes([value])


def field(number, value):
    return varint(number << 3 | 2) + varint(len(value)) + value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    expected = {}
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('manifest.json', json.dumps({'manifest_version': 2, 'name': 'Signed CRX fixture', 'version': '1.0',
            'background': {'scripts': ['background.js']}}))
        archive.writestr('background.js', 'globalThis.signedFixtureStarted = true;')
        archive.writestr('exact %25 雪.txt', 'Signed resource bytes')
    payload = stream.getvalue()
    rsa_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ec_key = ec.generate_private_key(ec.SECP256R1())
    def public(key):
        return key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    def identifier(key):
        return hashlib.sha256(public(key)).digest()[:16]
    def name(identity):
        return ''.join(chr(ord('a') + int(c, 16)) for c in identity.hex())
    def sign(key, data, pss=False):
        if isinstance(key, rsa.RSAPrivateKey):
            scheme = padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32) if pss else padding.PKCS1v15()
            return key.sign(data, scheme, hashes.SHA256())
        return key.sign(data, ec.ECDSA(hashes.SHA256()))
    def wrap(header, body=payload, version=3):
        return b'Cr24' + struct.pack('<II', version, len(header)) + header + body
    def make(keys=(rsa_key,), identity=None, body=payload, signed_extra=b'', header_extra=b'', proof_extra=b'', bad=None):
        identity = identifier(keys[0]) if identity is None else identity
        signed = field(1, identity) + signed_extra
        message = b'CRX3 SignedData\0' + struct.pack('<I', len(signed)) + signed + body
        proofs = []
        for index, key in enumerate(keys):
            pub = public(key)
            sig = sign(key, message, bad == 'pss')
            if bad == 'signature' and index == len(keys) - 1: sig = sig[:-1] + bytes([sig[-1] ^ 1])
            if bad == 'der-tail': pub += b'garbage'
            algorithm = 2 if isinstance(key, rsa.RSAPrivateKey) else 3
            if bad == 'algorithm': algorithm = 5 - algorithm
            proofs.append(field(algorithm, field(1, pub) + field(2, sig) + proof_extra))
        return wrap(b''.join(proofs) + field(10000, signed) + header_extra, body)
    def save(filename, data, valid=False, identity=None, offset=None):
        (args.output / filename).write_bytes(data)
        entry = {'valid': valid, 'sha256': hashlib.sha256(data).hexdigest()}
        if valid:
            entry['identifier'] = name(identity or identifier(rsa_key))
            entry['archive_offset'] = offset or 12 + struct.unpack('<I', data[8:12])[0]
            entry['archive_sha256'] = hashlib.sha256(data[entry['archive_offset']:]).hexdigest()
            entry['extract'] = not filename.startswith('unsafe-')
            if entry['extract']:
                with zipfile.ZipFile(io.BytesIO(data[entry['archive_offset']:])) as archive:
                    entry['files_sha256'] = {item.filename: hashlib.sha256(archive.read(item)).hexdigest() for item in archive.infolist() if not item.is_dir()}
        expected[filename] = entry
    valid = make()
    save('rsa.crx', valid, True)
    save('ecdsa.crx', make((ec_key,)), True, identifier(ec_key))
    save('multiple-proofs.crx', make((rsa_key, ec_key)), True)
    save('developer-second.crx', make((ec_key, rsa_key), identifier(rsa_key)), True)
    # Unknown fields cover every protobuf wire type, including balanced groups.
    unknown = varint(40 << 3) + varint(123) + varint(41 << 3 | 1) + b'12345678' + field(42, b'unknown')
    unknown += varint(43 << 3 | 5) + b'1234' + varint(44 << 3 | 3) + field(1, b'nested') + varint(44 << 3 | 4)
    save('unknown-fields.crx', make(signed_extra=unknown, header_extra=unknown, proof_extra=unknown), True)
    save('header-eocd-tokens.crx', make(header_extra=field(45, b'PK\x05\x06PK\x06\x06PK\x06\x07')), True)
    unsafe_stream = io.BytesIO()
    with zipfile.ZipFile(unsafe_stream, 'w') as archive:
        archive.writestr('manifest.json', '{}')
        archive.writestr('../escape.txt', 'must stay outside')
    save('unsafe-parent.crx', make(body=unsafe_stream.getvalue()), True)
    save('unsafe-invalid-zip.crx', make(body=b'PK\x03\x04not a zip'), True)
    save('duplicate-id-last-wins.crx', make(signed_extra=field(1, identifier(rsa_key))), True)
    # A different ID is signed correctly, but has no matching developer key.
    save('missing-developer.crx', make(identity=bytes(16)))
    for kind in ['signature', 'der-tail', 'algorithm', 'pss']:
        save(kind + '.crx', make(bad=kind))
    save('invalid-extra-proof.crx', make((rsa_key, ec_key), bad='signature'))
    save('short-id.crx', make(identity=bytes(15)))
    save('long-id.crx', make(identity=bytes(17)))
    save('signed-data-empty.crx', wrap(field(10000, b'')))
    save('missing-proof.crx', wrap(field(10000, field(1, identifier(rsa_key)))))
    save('empty-proof.crx', wrap(field(2, b'') + field(10000, field(1, identifier(rsa_key)))))
    save('no-signed-header.crx', wrap(valid[12:12 + struct.unpack('<I', valid[8:12])[0]].split(field(10000, field(1, identifier(rsa_key))))[0]))
    for field_number in [0, 2, 10000]:
        save('invalid-wire-' + str(field_number) + '.crx', wrap(varint(field_number << 3 | 7)))
    save('unterminated-varint.crx', wrap(b'\x80' * 10))
    save('overflowing-varint.crx', wrap(b'\xff' * 9 + b'\x02'))
    save('oversized-tag.crx', wrap(varint(1 << 35)))
    save('oversized-length.crx', wrap(varint(10000 << 3 | 2) + varint(2**64 - 1)))
    save('truncated-field.crx', wrap(field(10000, b'123')[:-1]))
    save('unbalanced-group.crx', wrap(varint(4 << 3 | 3) + varint(5 << 3 | 4)))
    save('unclosed-group.crx', wrap(varint(4 << 3 | 3)))
    save('deep-group.crx', wrap(varint(4 << 3 | 3) * 66 + varint(4 << 3 | 4) * 66))
    save('header-limit.crx', b'Cr24' + struct.pack('<II', 3, 1024 * 1024 + 1))
    save('header-overrun.crx', b'Cr24' + struct.pack('<II', 3, 300) + b'x')
    signed_offset = valid.index(field(10000, field(1, identifier(rsa_key))))
    proof = valid[12:signed_offset]
    save('proof-count-limit.crx', wrap(proof * 65 + field(10000, field(1, identifier(rsa_key)))))
    save('key-limit.crx', wrap(field(2, field(1, b'x' * (16 * 1024 + 1)) + field(2, b'x'))))
    save('signature-limit.crx', wrap(field(2, field(1, b'x') + field(2, b'x' * (16 * 1024 + 1)))))
    for version in [0, 2, 4, 0xffffffff]: save('version-' + str(version) + '.crx', valid[:4] + struct.pack('<I', version) + valid[8:])
    for length in list(range(13)) + [len(valid) - 1, len(valid) - 16]: save('truncated-' + str(length) + '.crx', valid[:length])
    for offset in [0, 12, signed_offset + 6, len(valid) - 1]:
        damaged = bytearray(valid); damaged[offset] ^= 1
        save('modified-' + str(offset) + '.crx', bytes(damaged))
    save('appended.crx', valid + b'extra unsigned bytes')
    save('signed-nonzip.crx', make(body=b'not a zip archive'))
    version_mismatch = bytearray(payload)
    version_mismatch[4:6] = struct.pack('<H', 45)
    save('zip-version-mismatch.crx', make(body=bytes(version_mismatch)), True)
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='summit-crx-zip-fixtures-') as generated:
        subprocess.run(['python3', str(root / 'tools/test-engine-extension-archives-fixtures.py'), generated], check=True, stdout=subprocess.DEVNULL)
        names = ['parent.zip','nested-parent.zip','absolute.zip','windows-drive.zip','unc.zip','backslash.zip','dot.zip','empty-component.zip','colon.zip','symlink.zip','symlink-child.zip','fifo.zip','socket.zip','character.zip','block.zip','duplicate.zip','directory-file.zip','directory-data.zip','long-name.zip','truncated.zip','crc.zip','encrypted.zip','empty.zip','missing-manifest.zip','wrapped-manifest.zip','nul-collision.zip']
        for filename in names:
            body = (Path(generated) / filename).read_bytes()
            save('unsafe-' + filename + '.crx', make(body=body), True)

    root = Path(__file__).resolve().parents[1]
    pinned = root / '.cache/extension-corpus/packages/b6be71ed3e3e85eaad8f02710b9071d06428e141d942c43d5f65d4526e82dc3e/uBlock0_1.74.0.chromium.crx'
    data = pinned.read_bytes()
    assert hashlib.sha256(data).hexdigest() == 'b6be71ed3e3e85eaad8f02710b9071d06428e141d942c43d5f65d4526e82dc3e'
    # The publisher's declared signed CRX ID is retained as a fixed test vector.
    header = data[12:12 + struct.unpack('<I', data[8:12])[0]]
    marker = varint(10000 << 3 | 2) + b'\x12\x0a\x10'
    signed_id = header[header.index(marker) + len(marker):][:16]
    assert name(signed_id) == 'fkgkibajhfbepljeaefdnfnegdcjomkh'
    save('published-ublock.crx', data, True, signed_id)
    (args.output / 'expected.json').write_text(json.dumps(expected, indent=2) + '\n')
    print(json.dumps({'fixtures': len(expected), 'published_identifier': name(signed_id), 'output': str(args.output)}))


if __name__ == '__main__': main()

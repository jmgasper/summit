#!/usr/bin/env python3
"""Create signed CRX3 runtime packages independently of the production verifier."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa


def field(number, value):
    def varint(value):
        output = bytearray()
        while value > 127:
            output.append((value & 127) | 128)
            value >>= 7
        return bytes(output) + bytes([value])
    return varint(number << 3 | 2) + varint(len(value)) + value


def generate(output):
    output.mkdir(parents=True, exist_ok=False)
    background = '''(async () => {
        const previous = await chrome.storage.local.get('boot');
        const boot = (previous.boot || 0) + 1;
        await chrome.storage.local.set({boot});
        await chrome.browserAction.setTitle({title: `CRX ${chrome.runtime.id} boot ${boot}`});
    })().catch(error => chrome.browserAction.setTitle({title: 'FAIL ' + error.message}));'''
    probe = '''(async () => {
        const params = new URLSearchParams(location.hash.slice(1));
        const nonce = params.get('nonce') || '';
        const expectedBoot = Number(params.get('boot') || 0);
        let boot;
        for (let i = 0; i < 200; ++i) {
            ({boot} = await chrome.storage.local.get('boot'));
            if (boot && (!expectedBoot || boot === expectedBoot)) break;
            await new Promise(resolve => setTimeout(resolve, 50));
        }
        const resource = await (await fetch(chrome.runtime.getURL('exact%20%2525%20%E9%9B%AA.txt'))).text();
        if (resource !== 'Signed resource bytes' || chrome.runtime.id !== browser.runtime.id || !boot || (expectedBoot && boot !== expectedBoot))
            throw new Error('signed resource, namespace identity or background storage mismatch');
        document.title = `CRX PROBE ${chrome.runtime.id} ${boot} ${nonce} PASS`;
        document.querySelector('main').textContent = document.title;
        if (!nonce) await chrome.browserAction.setTitle({title: `CRX POPUP ${chrome.runtime.id} boot ${boot} PASS`});
    })().catch(error => {
        document.title = 'CRX PROBE FAIL ' + error.message;
        chrome.browserAction.setTitle({title: 'FAIL ' + error.message});
    });'''
    def archive(version='1.0'):
        manifest = {'manifest_version': 2, 'name': 'Signed CRX runtime fixture', 'version': version,
            'browser_specific_settings': {'gecko': {'id': 'conflicting-manifest@summit.invalid'}},
            'permissions': ['storage'], 'background': {'scripts': ['background.js'], 'persistent': True},
            'browser_action': {'default_title': 'CRX starting', 'default_popup': 'probe.html'}}
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, 'w', zipfile.ZIP_DEFLATED) as zipped:
            for name, data in {'manifest.json': json.dumps(manifest), 'background.js': background,
                    'probe.js': probe, 'probe.html': '<!doctype html><meta charset=utf-8><style>body{width:420px;min-height:170px;font:16px sans-serif;padding:16px}</style><h1>Signed CRX extension</h1><main>Checking package identity and storage…</main><script src="probe.js"></script>',
                    'exact %25 雪.txt': 'Signed resource bytes'}.items():
                zipped.writestr(name, data)
        return stream.getvalue()
    payload = archive()
    files = {'unsigned.zip': payload}
    identities = {}
    keys = {'rsa': rsa.generate_private_key(public_exponent=65537, key_size=2048),
            'ecdsa': ec.generate_private_key(ec.SECP256R1())}
    def signed(key, body):
        public = key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
        identity = hashlib.sha256(public).digest()[:16]
        signed_data = field(1, identity)
        message = b'CRX3 SignedData\0' + struct.pack('<I', len(signed_data)) + signed_data + body
        if isinstance(key, rsa.RSAPrivateKey):
            proof_type = 2
            signature = key.sign(message, padding.PKCS1v15(), hashes.SHA256())
        else:
            proof_type = 3
            signature = key.sign(message, ec.ECDSA(hashes.SHA256()))
        header = field(proof_type, field(1, public) + field(2, signature)) + field(10000, signed_data)
        data = b'Cr24' + struct.pack('<II', 3, len(header)) + header + body
        return data, ''.join(chr(ord('a') + int(value, 16)) for value in identity.hex())
    for name, key in keys.items():
        files[name + '.crx'], identities[name] = signed(key, payload)
    files['updated.crx'], updated_id = signed(keys['rsa'], archive('2.0'))
    assert updated_id == identities['rsa']
    damaged = bytearray(files['rsa.crx']); damaged[-1] ^= 1
    files['corrupt.crx'] = bytes(damaged)
    files['appended.crx'] = files['rsa.crx'] + b'unsigned bytes'
    files['unsupported.crx'] = files['rsa.crx'][:4] + struct.pack('<I', 2) + files['rsa.crx'][8:]
    for name, data in files.items():
        (output / name).write_bytes(data)
    expected = {'identities': identities, 'sha256': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}}
    (output / 'expected.json').write_text(json.dumps(expected, indent=2) + '\n')
    return expected


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    print(json.dumps(generate(parser.parse_args().output)))

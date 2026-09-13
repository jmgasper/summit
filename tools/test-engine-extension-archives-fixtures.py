#!/usr/bin/env python3
"""Create independent ZIP/XPI fixtures for the actual native archive library."""
import base64
import io
import json
import pathlib
import stat
import struct
import sys
import tarfile
import warnings
import zipfile

OUTPUT = pathlib.Path(sys.argv[1])
OUTPUT.mkdir(parents=True, exist_ok=True)
MANIFEST = json.dumps({'manifest_version': 3, 'name': '__MSG_name__', 'version': '1.0',
    'default_locale': 'en', 'icons': {'16': 'icons/icon.png'}}).encode()
ICON = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII=')


def item(name, mode=stat.S_IFREG | 0o644):
    info = zipfile.ZipInfo(name, (2024, 1, 1, 0, 0, 0))
    info.create_system = 3
    info.external_attr = mode << 16
    return info


def archive(name, entries, compression=zipfile.ZIP_STORED):
    with warnings.catch_warnings():
        warnings.simplefilter('ignore', UserWarning)
        with zipfile.ZipFile(OUTPUT / name, 'w', compression=compression) as result:
            for path, data in entries:
                info = path if isinstance(path, zipfile.ZipInfo) else item(path)
                info.compress_type = compression
                result.writestr(info, data)


normal = [(item('manifest.json', stat.S_IFREG | 0o4777), MANIFEST),
    ('_locales/en/messages.json', b'{"name":{"message":"Native archive"}}'),
    (item('_locales/', stat.S_IFDIR | 0o755), b''),
    ('icons/icon.png', ICON), ('asset %25 # 雪.txt', b'exact Unicode resource'),
    (item('empty/', stat.S_IFDIR | 0o755), b'')]
archive('valid.zip', normal)
archive('valid.xpi', normal, zipfile.ZIP_DEFLATED)
archive('small.zip', [('manifest.json', b'{}'), ('payload', b'x' * 64)])
archive('bomb.zip', [('manifest.json', b'{}'), ('payload', b'0' * (1024 * 1024))], zipfile.ZIP_DEFLATED)
archive('empty.zip', [])
archive('missing-manifest.zip', [('readme', b'not an extension')])
archive('wrapped-manifest.zip', [('extension/manifest.json', b'{}')])
with zipfile.ZipFile(OUTPUT / 'zip64.zip', 'w', compression=zipfile.ZIP_DEFLATED) as result:
    with result.open(item('manifest.json'), 'w', force_zip64=True) as stream:
        stream.write(b'{}')

bad_names = {'parent': '../outside.txt', 'nested-parent': 'dir/../../outside.txt',
    'absolute': '/outside.txt', 'windows-drive': 'C:/outside.txt',
    'unc': '//server/share/outside.txt', 'backslash': r'..\outside.txt',
    'dot': './file', 'empty-component': 'dir//file', 'colon': 'file:stream'}
for kind, name in bad_names.items():
    archive(kind + '.zip', [('manifest.json', b'{}'), (name, b'escape')])
for kind, mode in [('symlink', stat.S_IFLNK), ('fifo', stat.S_IFIFO),
    ('socket', stat.S_IFSOCK), ('character', stat.S_IFCHR), ('block', stat.S_IFBLK)]:
    archive(kind + '.zip', [('manifest.json', b'{}'), (item('link', mode | 0o777), b'../../outside.txt')])
archive('symlink-child.zip', [('manifest.json', b'{}'),
    (item('link', stat.S_IFLNK | 0o777), b'../../'), ('link/outside.txt', b'escape')])
archive('duplicate.zip', [('manifest.json', b'{}'), ('manifest.json', b'overwritten')])
archive('directory-file.zip', [('manifest.json', b'{}'), ('assets/file', b'payload'), ('assets', b'conflict')])
archive('directory-data.zip', [('manifest.json', b'{}'), (item('bad/', stat.S_IFDIR | 0o755), b'not empty')])
archive('deep.zip', [('manifest.json', b'{}'), ('a/b/c/d/file', b'deep')])
archive('long-name.zip', [('manifest.json', b'{}'), ('x' * 256, b'long')])
archive('path-limit.zip', [('manifest.json', b'{}'), ('long-directory/long-file', b'path')])

valid = (OUTPUT / 'small.zip').read_bytes()
(OUTPUT / 'truncated.zip').write_bytes(valid[:-15])
(OUTPUT / 'crx-wrapper.crx').write_bytes(b'Cr24' + bytes(12) + valid)
(OUTPUT / 'self-extracting.zip').write_bytes(b'MZ' + bytes(14) + valid)
(OUTPUT / 'not-zip.zip').write_bytes(b'not a zip archive')
corrupt = bytearray(valid)
with zipfile.ZipFile(io.BytesIO(valid)) as source:
    entry = source.getinfo('payload')
    offset = entry.header_offset + 30 + len(entry.filename.encode()) + len(entry.extra)
    corrupt[offset] ^= 1
(OUTPUT / 'crc.zip').write_bytes(corrupt)
encrypted = bytearray(valid)
for signature, offset in [(b'PK\x03\x04', 6), (b'PK\x01\x02', 8)]:
    begin = 0
    while (begin := encrypted.find(signature, begin)) >= 0:
        struct.pack_into('<H', encrypted, begin + offset, struct.unpack_from('<H', encrypted, begin + offset)[0] | 1)
        begin += 4
(OUTPUT / 'encrypted.zip').write_bytes(encrypted)
# Standard ZIP has no hard-link object. A genuine tar hard-link package is
# rejected at the ZIP format boundary, never passed to a general extractor.
with tarfile.open(OUTPUT / 'hardlink.tar', 'w') as result:
    entry = tarfile.TarInfo('manifest.json'); entry.size = 2
    result.addfile(entry, io.BytesIO(b'{}'))
    link = tarfile.TarInfo('escape'); link.type = tarfile.LNKTYPE; link.linkname = '../../outside.txt'
    result.addfile(link)
# libzip deliberately maps impossible native NUL filename bytes to spaces.
# Exercise that behavior and a resulting duplicate, without a second parser.
archive('nul.zip', [('manifest.json', b'{}'), ('nameXtail', b'nul')])
(OUTPUT / 'nul.zip').write_bytes((OUTPUT / 'nul.zip').read_bytes().replace(b'nameXtail', b'name\0tail'))
archive('nul-collision.zip', [('manifest.json', b'{}'), ('nameXtail', b'nul'), ('name tail', b'collision')])
(OUTPUT / 'nul-collision.zip').write_bytes((OUTPUT / 'nul-collision.zip').read_bytes().replace(b'nameXtail', b'name\0tail'))
print('Created', len(list(OUTPUT.iterdir())), 'ZIP/XPI and malicious fixtures.')

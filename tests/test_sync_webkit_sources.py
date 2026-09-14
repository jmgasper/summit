import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location('sync_webkit',
    Path(__file__).resolve().parents[1] / 'tools/sync-webkit-sources.py')
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)


def archive(files):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode='w:gz') as stream:
        for name, data, mode in files:
            member = tarfile.TarInfo(name)
            member.size, member.mode, member.mtime = len(data), mode, 1
            stream.addfile(member, io.BytesIO(data))
    output.seek(0)
    return output


class SourceSyncTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def upload(self, files, patch='a' * 64):
        with contextlib.redirect_stdout(io.StringIO()):
            sync.synchronize(self.root, patch, archive(files))

    def test_repeated_upload_preserves_inode_timestamps_and_permissions(self):
        files = [('Source/empty', b'', 0o600), ('Tools/script', b'#!/bin/sh\n', 0o755)]
        self.upload(files)
        before = {name: (self.root / name).stat() for name, _, _ in files}
        with mock.patch.object(sync.tempfile, 'NamedTemporaryFile', side_effect=AssertionError('Unchanged source must not be staged')):
            self.upload(files)
        for name, _, mode in files:
            after = (self.root / name).stat()
            self.assertEqual((after.st_ino, after.st_mtime_ns, after.st_ctime_ns),
                             (before[name].st_ino, before[name].st_mtime_ns, before[name].st_ctime_ns))
            self.assertEqual(after.st_mode & 0o777, mode)

    def test_content_change_updates_build_timestamp_and_mode_only_does_not(self):
        name = 'Source/example.cpp'
        self.upload([(name, b'old', 0o644)])
        path = self.root / name
        os.utime(path, (100, 100))
        self.upload([(name, b'new', 0o644)])
        self.assertEqual(path.read_bytes(), b'new')
        self.assertGreater(path.stat().st_mtime, 100)
        timestamp = path.stat().st_mtime_ns
        self.upload([(name, b'new', 0o600)])
        self.assertEqual(path.stat().st_mtime_ns, timestamp)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_large_file_comparison_and_replacement(self):
        name = 'Source/large.cpp'
        with mock.patch.object(sync, 'COMPARE_BUFFER_LIMIT', 8):
            self.upload([(name, b'original data', 0o644)])
            before = (self.root / name).stat()
            self.upload([(name, b'original data', 0o644)])
            self.assertEqual((self.root / name).stat().st_ino, before.st_ino)
            self.assertEqual((self.root / name).stat().st_ctime_ns, before.st_ctime_ns)
            self.upload([(name, b'different data', 0o600)])
        self.assertEqual((self.root / name).read_bytes(), b'different data')
        self.assertEqual([path.name for path in (self.root / 'Source').iterdir()], ['large.cpp'])

    def test_file_replaces_symlink_without_writing_its_target(self):
        (self.root / 'Source').mkdir()
        outside = self.root / 'unrelated'
        outside.write_bytes(b'original')
        path = self.root / 'Source/example.cpp'
        path.symlink_to(outside)
        self.upload([('Source/example.cpp', b'replacement', 0o600)])
        self.assertFalse(path.is_symlink())
        self.assertEqual(path.read_bytes(), b'replacement')
        self.assertEqual(outside.read_bytes(), b'original')

    def test_removes_only_previous_members_and_rejects_path_escape(self):
        self.upload([('Source/old.cpp', b'old', 0o644)])
        unrelated = self.root / 'WebKitBuild/object.o'
        unrelated.parent.mkdir()
        unrelated.write_bytes(b'object')
        self.upload([('Source/new.cpp', b'new', 0o644)], 'b' * 64)
        self.assertFalse((self.root / 'Source/old.cpp').exists())
        self.assertEqual(unrelated.read_bytes(), b'object')
        manifest = self.root / '.summit-source-manifest.json'
        before = manifest.read_bytes()
        with self.assertRaises(ValueError):
            self.upload([('Source/../../escape', b'bad', 0o644)], 'c' * 64)
        self.assertEqual(manifest.read_bytes(), before)
        self.assertEqual(json.loads(before)['patch_sha256'], 'b' * 64)


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Check conditional IPC dispatch metadata with WebKit's generator and a C++ preprocessor."""
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ROW = re.compile(r'MessageDescription\s*\{\s*"([^"]+)"_s,\s*ReceiverName::\w+,\s*'
                 r'(true|false),\s*(true|false),\s*(true|false),')


def main():
    sys.dont_write_bytecode = True
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine-root', type=Path, default=ROOT / '.cache/WebKit')
    parser.add_argument('--overlay', type=Path, help='Candidate tree containing Source/WebKit/Scripts/webkit/messages.py')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    scripts = args.engine_root / 'Source/WebKit/Scripts'
    sys.path.insert(0, str(scripts.resolve()))
    import webkit
    from webkit import messages, model, parser as ipc_parser
    generator = scripts / 'webkit/messages.py'
    if args.overlay:
        generator = args.overlay / 'Source/WebKit/Scripts/webkit/messages.py'
        spec = importlib.util.spec_from_file_location('webkit.messages', generator)
        messages = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(messages)
        webkit.messages = messages
        sys.modules['webkit.messages'] = messages
    from webkit import messages_unittest
    entrypoint_path = scripts / 'generate-message-receiver.py'
    if args.overlay and (args.overlay / 'Source/WebKit/Scripts/generate-message-receiver.py').is_file():
        entrypoint_path = args.overlay / 'Source/WebKit/Scripts/generate-message-receiver.py'
    entrypoint_spec = importlib.util.spec_from_file_location('ipc_generator_entrypoint', entrypoint_path)
    entrypoint = importlib.util.module_from_spec(entrypoint_spec)
    entrypoint_spec.loader.exec_module(entrypoint)

    compilations = []
    sources = {}

    def preprocess(receiver, definitions, selected):
        receivers = model.generate_global_model([receiver])
        generated = messages.generate_message_names_implementation(receivers)
        # Only preprocessing is required: the real config macros select the
        # active declaration, and no unrelated WebKit headers are needed.
        body = '\n'.join(line for line in generated.splitlines() if not line.startswith('#include'))
        prelude = '#define PLATFORM(x) PLATFORM_##x\n#define USE(x) USE_##x\n#define ENABLE(x) ENABLE_##x\n'
        prelude += '\n'.join('#define ' + name + ' ' + str(value) for name, value in definitions.items()) + '\n'
        output = subprocess.run([args.cxx, '-E', '-P', '-x', 'c++', '-'], input=prelude + body,
                                capture_output=True, text=True, check=True).stdout
        rows = {name: [] for name in selected}
        for match in ROW.finditer(output):
            if match[1] in rows:
                rows[match[1]].append(tuple(value == 'true' for value in match.groups()[1:]))
        compilations.append({'receiver': receiver.name, 'definitions': definitions, 'rows': rows,
                             'generated_sha256': hashlib.sha256(generated.encode()).hexdigest()})
        return rows

    def parse_fixture(body):
        source = '[DispatchedFrom=UI, DispatchedTo=Networking, ExceptionForEnabledBy]\nmessages -> Conditional {\n' + body + '\n}\n'
        return ipc_parser.parse(io.StringIO(source))

    class ConditionalDispatchTests(unittest.TestCase):
        def test_actual_cookie_observers(self):
            path = args.engine_root / 'Source/WebKit/NetworkProcess/Cookies/WebCookieManager.messages.in'
            sources[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
            names = ['WebCookieManager_StartObservingCookieChanges', 'WebCookieManager_StopObservingCookieChanges']
            for haiku, curl in ((1, 1), (0, 1), (1, 0)):
                with self.subTest(haiku=haiku, curl=curl):
                    with path.open() as source:
                        receiver = ipc_parser.parse(source)
                    rows = preprocess(receiver, {'PLATFORM_HAIKU': haiku, 'USE_CURL': curl, 'USE_SOUP': 0}, names)
                    for name in names:
                        self.assertEqual(rows[name], [(bool(haiku and curl), False, False)], name)

        def test_bounded_and_unbounded_permissions_follow_active_declaration(self):
            body = '''
#if ENABLE(PRIORITY)
    void Changed(uint64_t value) AllowedWhenWaitingForSyncReply
#endif
#if !ENABLE(PRIORITY)
    void Changed(uint64_t value) AllowedWhenWaitingForSyncReplyDuringUnboundedIPC
#endif
'''
            for priority in (0, 1):
                with self.subTest(priority=priority):
                    rows = preprocess(parse_fixture(body), {'ENABLE_PRIORITY': priority}, ['Conditional_Changed'])
                    self.assertEqual(rows['Conditional_Changed'], [(bool(priority), not priority, False)])

        def test_async_replies_keep_reply_metadata(self):
            body = '''
#if ENABLE(PRIORITY)
    void Query(uint64_t value) -> (uint64_t result) AllowedWhenWaitingForSyncReply
#endif
#if !ENABLE(PRIORITY)
    void Query(uint64_t value) -> (uint64_t result)
#endif
'''
            for priority in (0, 1):
                with self.subTest(priority=priority):
                    rows = preprocess(parse_fixture(body), {'ENABLE_PRIORITY': priority},
                                      ['Conditional_Query', 'Conditional_QueryReply'])
                    self.assertEqual(rows['Conditional_Query'], [(bool(priority), False, False)])
                    self.assertEqual(rows['Conditional_QueryReply'], [(False, False, True)])

    class IncrementalGenerationTests(unittest.TestCase):
        def setUp(self):
            self.temporary = tempfile.TemporaryDirectory(prefix='summit-ipc-generation-')
            self.addCleanup(self.temporary.cleanup)
            self.root = Path(self.temporary.name)
            self.output = self.root / 'generated'
            self.input = self.root / 'Conditional.messages.in'
            self.input.write_text('[DispatchedFrom=UI, DispatchedTo=Networking, ExceptionForEnabledBy]\n'
                                  'messages -> Conditional {\n'
                                  '    void Updated(uint64_t value) AllowedWhenWaitingForSyncReply\n}\n')

        def generate(self):
            self.assertEqual(entrypoint.main(['generator', str(self.root), 'Conditional',
                                             '--output-dir', str(self.output)]), 0)

        def stamp_and_snapshot(self):
            snapshot = {}
            for path in self.output.rglob('*'):
                if path.is_file():
                    os.utime(path, ns=(1600000000123456789, 1600000000123456789))
                    snapshot[str(path.relative_to(self.output))] = (path.read_bytes(), path.stat().st_mtime_ns)
            self.assertIn('MessageNames.cpp', snapshot)
            self.assertIn('ConditionalMessages.h', snapshot)
            return snapshot

        def test_unchanged_generation_keeps_bytes_and_timestamps(self):
            self.generate()
            before = self.stamp_and_snapshot()
            self.generate()
            after = {str(path.relative_to(self.output)): (path.read_bytes(), path.stat().st_mtime_ns)
                     for path in self.output.rglob('*') if path.is_file()}
            self.assertEqual(before, after)

        def test_dispatch_change_rewrites_only_metadata(self):
            self.generate()
            before = self.stamp_and_snapshot()
            self.input.write_text(self.input.read_text().replace(' AllowedWhenWaitingForSyncReply', ''))
            self.generate()
            changed = set()
            for name, (data, timestamp) in before.items():
                path = self.output / name
                if path.read_bytes() != data:
                    changed.add(name)
                    self.assertNotEqual(path.stat().st_mtime_ns, timestamp)
                else:
                    self.assertEqual(path.stat().st_mtime_ns, timestamp, name)
            self.assertEqual(changed, {'MessageNames.cpp'})

        def test_source_list_keeps_timestamp_until_inputs_change(self):
            path = self.root / 'receiver-sources.txt'
            command = ['generator', str(self.root), 'Conditional', '--output-sources', str(path)]
            self.assertEqual(entrypoint.main(command), 0)
            os.utime(path, ns=(1600000000123456789, 1600000000123456789))
            before = (path.read_bytes(), path.stat().st_mtime_ns)
            self.assertEqual(entrypoint.main(command), 0)
            self.assertEqual((path.read_bytes(), path.stat().st_mtime_ns), before)
            self.assertEqual(entrypoint.main(command[:3] + ['Another'] + command[3:]), 0)
            self.assertEqual(path.read_text(), 'ConditionalMessageReceiver.cpp\nAnotherMessageReceiver.cpp\n')
            self.assertNotEqual(path.stat().st_mtime_ns, before[1])

    suite = unittest.TestSuite([
        unittest.defaultTestLoader.loadTestsFromTestCase(ConditionalDispatchTests),
        unittest.defaultTestLoader.loadTestsFromTestCase(IncrementalGenerationTests),
        unittest.defaultTestLoader.loadTestsFromTestCase(messages_unittest.GeneratedFileContentsTest),
    ])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    report = {'scope': 'actual IPC generator, host C++ preprocessing across seven configurations, incremental output bytes/timestamps, and unchanged upstream generated-file expectations; no native engine runtime',
              'harness_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'generator': str(generator), 'generator_sha256': hashlib.sha256(generator.read_bytes()).hexdigest(),
              'entrypoint': str(entrypoint_path), 'entrypoint_sha256': hashlib.sha256(entrypoint_path.read_bytes()).hexdigest(),
              'module_sha256': {name: hashlib.sha256((scripts / 'webkit' / name).read_bytes()).hexdigest()
                                for name in ('model.py', 'parser.py', 'messages_unittest.py')},
              'sources': sources, 'tests_run': result.testsRun,
              'failures': [{'test': test.id(), 'details': details} for test, details in result.failures],
              'errors': [{'test': test.id(), 'details': details} for test, details in result.errors],
              'preprocessed_configurations': compilations, 'passed': result.wasSuccessful()}
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())

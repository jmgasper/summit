import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
from native_crash_log import NativeCrashLog


class NativeCrashLogTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = pathlib.Path(self.directory.name) / 'syslog'
        self.path.write_text('KERN: debug_server: Thread 12 entered the debugger: old crash\n')

    def test_old_crashes_and_unrelated_messages_do_not_fail(self):
        monitor = NativeCrashLog(self.path)
        with self.path.open('a') as stream:
            stream.write('AUTH: private data\nKERN: ordinary diagnostic\n')
        report = monitor.finish(0)
        self.assertTrue(report['passed'])
        self.assertEqual(report['events'], [])

    def test_new_trap_fails_even_if_helper_has_disappeared(self):
        monitor = NativeCrashLog(self.path)
        with self.path.open('a') as stream:
            stream.write('KERN: debug_server: Thread 34 entered the debugger: Segment violation\n')
            stream.write('KERN: 35: DEBUGGER: bogus pointer (double free?) 0x1234\n')
            stream.write('AUTH: unrelated private data\n')
        report = monitor.finish(0)
        self.assertFalse(report['passed'])
        self.assertTrue(report['coverage_complete'])
        self.assertEqual(len(report['events']), 2)
        self.assertNotIn('private data', str(report))

    def test_rotation_cannot_pass_as_no_crashes(self):
        monitor = NativeCrashLog(self.path)
        self.path.rename(self.path.with_suffix('.old'))
        self.path.write_text('KERN: new file\n')
        self.assertFalse(monitor.finish(0)['passed'])

    def test_truncation_cannot_pass_as_no_crashes(self):
        monitor = NativeCrashLog(self.path)
        self.path.write_text('')
        self.assertFalse(monitor.finish(0)['passed'])

    def test_rewritten_log_regrown_past_start_cannot_pass(self):
        monitor = NativeCrashLog(self.path)
        self.path.write_text('replacement\n' * 100)
        self.assertFalse(monitor.finish(0)['passed'])


if __name__ == '__main__':
    unittest.main()

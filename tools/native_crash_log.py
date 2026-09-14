"""Reject fresh native debugger events during an isolated Haiku runtime test.

Helper disappearance alone does not prove clean shutdown: the UI process can
kill a helper after it has trapped in the debugger. Read only the log interval
covered by the test, preserving debugger diagnostics without unrelated syslog
messages. Events are VM-wide; run these integration tests one at a time.
"""
import os
import pathlib
import re
import time


class NativeCrashLog:
    def __init__(self, path='/var/log/syslog'):
        self.path = pathlib.Path(path)
        self.stream = self.path.open('rb')
        self.start = os.fstat(self.stream.fileno())
        self.stream.seek(0, os.SEEK_END)
        self.offset = self.stream.tell()
        self.anchor_offset = max(0, self.offset - 256)
        self.stream.seek(self.anchor_offset)
        self.anchor = self.stream.read(self.offset - self.anchor_offset)

    def finish(self, settle_seconds=1):
        # debug_server writes through the native syslog daemon. Allow its last
        # queued diagnostics to reach the file after the child has exited.
        time.sleep(settle_seconds)
        try:
            current = self.path.stat()
            self.stream.seek(self.anchor_offset)
            anchor_matches = self.stream.read(len(self.anchor)) == self.anchor
            self.stream.seek(self.offset)
            data = self.stream.read()
            end = os.fstat(self.stream.fileno())
            coverage = ((current.st_dev, current.st_ino) == (self.start.st_dev, self.start.st_ino)
                        and end.st_size >= self.offset and anchor_matches)
            events = [line for line in data.decode('utf-8', errors='replace').splitlines()
                      if 'KERN:' in line and re.search(
                          r'debug_server: Thread \d+ entered the debugger|\bDEBUGGER:|\bPANIC:', line)]
            return {'path': str(self.path), 'start_offset': self.offset,
                    'end_offset': self.stream.tell(), 'coverage_complete': coverage,
                    'events': events, 'passed': coverage and not events}
        finally:
            self.stream.close()

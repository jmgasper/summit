#!/usr/bin/env python3
"""Load pages in Firefox through Marionette and print their timings as JSON.

Runs on the Haiku machine (python3.10), started by run-pageload.py:

  python3.10 marionette_client.py JOB.json

JOB: {"firefox": path, "profile": dir, "urls": [...], "settle": seconds,
      "window": [x, y, width, height], "setup": js, "collect": js}

For every URL Firefox is started with a Marionette port on the given profile,
the page is loaded (WebDriver:Navigate returns after the load event), the
page settles, the "setup" and "collect" scripts run in it and Firefox quits,
so every load starts a fresh browser as Summit's runs do.
"""
import json
import os
import signal
import socket
import subprocess
import sys
import time


class Marionette:
    def __init__(self, port, timeout=120):
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.5)
        self.sock.settimeout(300)
        self.buffer = b''
        self.id = 0
        self.receive()  # greeting

    def receive(self):
        while b':' not in self.buffer:
            self.buffer += self.recv()
        length, _, rest = self.buffer.partition(b':')
        length = int(length)
        while len(rest) < length:
            rest += self.recv()
        self.buffer = rest[length:]
        return json.loads(rest[:length])

    def recv(self):
        data = self.sock.recv(65536)
        if not data:
            raise ConnectionError('Marionette closed the connection')
        return data

    def command(self, name, params=None):
        self.id += 1
        body = json.dumps([0, self.id, name, params or {}]).encode()
        self.sock.sendall(str(len(body)).encode() + b':' + body)
        while True:
            message = self.receive()
            if message[0] == 1 and message[1] == self.id:
                if message[2]:
                    raise RuntimeError(f'{name}: {message[2]}')
                return message[3]


def run(job):
    results = []
    for index, url in enumerate(job['urls']):
        port = 2828 + index % 50
        os.makedirs(job['profile'], exist_ok=True)
        with open(os.path.join(job['profile'], 'user.js'), 'w') as prefs:
            prefs.write(f'user_pref("marionette.port", {port});\n'
                        'user_pref("browser.shell.checkDefaultBrowser", false);\n'
                        'user_pref("browser.startup.homepage_override.mstone", "ignore");\n'
                        'user_pref("datareporting.policy.dataSubmissionEnabled", false);\n'
                        'user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);\n'
                        'user_pref("browser.aboutwelcome.enabled", false);\n'
                        'user_pref("app.update.disabledForTesting", true);\n')
        log = open(os.path.join(job['profile'], f'firefox-{index}.log'), 'wb')
        firefox = subprocess.Popen([job['firefox'], '--marionette', '--no-remote', '--profile', job['profile']],
                                   stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        entry = {'url': url}
        try:
            client = Marionette(port)
            client.command('WebDriver:NewSession', {'capabilities': {'pageLoadStrategy': 'normal'}})
            x, y, width, height = job['window']
            try:
                client.command('WebDriver:SetWindowRect', {'x': x, 'y': y, 'width': width, 'height': height})
            except RuntimeError as error:
                entry['windowError'] = str(error)
            client.command('WebDriver:Navigate', {'url': 'about:blank'})
            started = time.time()
            try:
                client.command('WebDriver:Navigate', {'url': url})
            except RuntimeError as error:
                entry['navigateError'] = str(error)
            entry['navigateSeconds'] = time.time() - started
            time.sleep(job['settle'])
            client.command('WebDriver:ExecuteScript', {'script': job['setup'], 'args': []})
            time.sleep(1)
            entry['metrics'] = client.command('WebDriver:ExecuteScript',
                                              {'script': 'return ' + job['collect'], 'args': []})['value']
            try:
                client.command('Marionette:Quit', {'flags': ['eForceQuit']})
            except (RuntimeError, ConnectionError, OSError):
                pass
        except Exception as error:  # noqa: BLE001 - reported per URL
            entry['error'] = repr(error)
        try:
            firefox.wait(30)
        except subprocess.TimeoutExpired:
            os.killpg(firefox.pid, signal.SIGKILL)
            firefox.wait()
        results.append(entry)
        print(json.dumps(entry), flush=True)
    return results


if __name__ == '__main__':
    with open(sys.argv[1]) as source:
        run(json.load(source))

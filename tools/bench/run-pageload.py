#!/usr/bin/env python3
"""Compare page loads of complex sites in Summit and Firefox on the workstation.

Every load starts a fresh browser on a profile of its own that is kept for the
run: the first round of each browser meets an empty cache ("cold"), the later
rounds a filled one ("warm"). After the load event and a settling time, the
same script reads the page's own timings in both browsers (Navigation Timing,
first contentful paint, largest contentful paint, resources), so neither
browser's instrumentation is compared with the other's. Summit is driven with
summitctl and its Developer Tools console (opened only after the load),
Firefox through Marionette (tools/bench/marionette_client.py).

  SUMMIT_BENCH_HOST=workstation python3 tools/bench/run-pageload.py \\
      --bundle /boot/home/summit/build-modern-browser/bundle-XXXX [--rounds 3] [--url URL ...]

Results: .vm/bench/pageload-<time>/results.json and summary.md. The machine
must be quiet: other builds or benchmarks change the numbers more than most
engine changes do.
"""
import argparse
import os
import json
import pathlib
import shlex
import statistics
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import guest  # noqa: E402

DEFAULT_URLS = [
    'https://www.reddit.com/',
    'https://www.theguardian.com/international',
    'https://www.youtube.com/',
    'https://en.wikipedia.org/wiki/Haiku_(operating_system)',
    'https://www.bbc.com/news',
    'https://github.com/WebKit/WebKit',
]
MESA = os.environ.get('SUMMIT_BENCH_MESA', '/boot/home/summit-mesa/prefix-20261002')
SETUP = ('(window.__pageload = {lcp: null}, (() => { try { new PerformanceObserver(list => { const e = list.getEntries(); '
         'if (e.length) { const last = e[e.length - 1]; window.__pageload.lcp = last.startTime; '
         'window.__pageload.lcpWhat = (last.element ? last.element.tagName : "") + " " + (last.url || "").slice(0, 80); '
         'window.__pageload.lcpAll = e.map(x => [Math.round(x.startTime), x.size, x.element ? x.element.tagName : "", (x.url || "").slice(-30)]); } })'
         '.observe({type: "largest-contentful-paint", buffered: true}); } catch (error) '
         '{ window.__pageload.lcpError = String(error); } })(), true)')
COLLECT = ('(() => { const n = performance.getEntriesByType("navigation")[0]; const paint = {}; '
           'for (const e of performance.getEntriesByType("paint")) paint[e.name] = e.startTime; '
           'const r = performance.getEntriesByType("resource"); '
           'return {url: location.href, ttfb: n ? n.responseStart : null, dcl: n ? n.domContentLoadedEventEnd : null, '
           'load: n ? n.loadEventEnd : null, fcp: paint["first-contentful-paint"] ?? null, '
           'phases: n ? {fetch: n.fetchStart, redirectEnd: n.redirectEnd, dns: n.domainLookupStart, dnsEnd: n.domainLookupEnd, '
           'connect: n.connectStart, tls: n.secureConnectionStart, connectEnd: n.connectEnd, request: n.requestStart, '
           'response: n.responseStart, responseEnd: n.responseEnd, interactive: n.domInteractive, dclStart: n.domContentLoadedEventStart} : null, '
           'lcp: window.__pageload ? window.__pageload.lcp : null, lcpWhat: window.__pageload ? window.__pageload.lcpWhat : null, '
           'lcpAll: window.__pageload ? window.__pageload.lcpAll : null, '
           'resources: r.length, '
           'transfer: r.reduce((a, x) => a + (x.transferSize || 0), 0), elements: document.getElementsByTagName("*").length, '
           'last: [...r].filter(x => !n || !n.loadEventStart || x.responseEnd <= n.loadEventStart)'
           '.sort((a, b) => b.responseEnd - a.responseEnd).slice(0, 4).map(x => '
           '[x.initiatorType, Math.round(x.startTime), Math.round(x.responseEnd), x.name.slice(0, 90)])}; })()')
# SUMMIT_BENCH_RESOURCES=1 adds every resource's timing (the first 100 by start).
if os.environ.get('SUMMIT_BENCH_RESOURCES') == '1':
    COLLECT = COLLECT[:-len('}; })()')] + (', all: [...r].sort((a, b) => a.startTime - b.startTime).slice(0, 45).map(x => '
                                         '[x.initiatorType, Math.round(x.startTime), Math.round(x.responseStart), '
                                         'Math.round(x.responseEnd), x.transferSize || 0, x.name.slice(0, 64)])}; })()')
METRICS = ('ttfb', 'fcp', 'lcp', 'dcl', 'load')


def log(message):
    print(time.strftime('[%H:%M:%S] ') + message, flush=True)


def summit_evaluate(ctl, team, expression, attempts=40):
    marker = 'PL%d' % int(time.time() * 1000)
    wrapped = (f'(() => {{ try {{ return "{marker}" + JSON.stringify(eval({json.dumps(expression)})); }} '
               f'catch (e) {{ return "{marker}ERR " + e; }} }})()')
    guest.ctl(ctl, team, 'devtools-do', 'clear-console')
    guest.ctl(ctl, team, 'devtools-do', 'evaluate', wrapped, timeout_ms=15000)
    for _ in range(attempts):
        time.sleep(0.5)
        code, out, _ = guest.ctl(ctl, team, 'devtools-do', 'state', timeout_ms=15000)
        try:
            text = json.loads(out)['console']['text']
        except (ValueError, KeyError):
            continue
        for line in text.splitlines():
            if line.startswith('"' + marker):
                value = json.loads(line)[len(marker):]
                if value.startswith('ERR '):
                    raise guest.GuestError(value)
                return json.loads(value)
    raise guest.GuestError('no result from the page')


def summit_load(ctl, bundle, profile, url, settle, window, log_path):
    env = {'SUMMIT_LIBRARY_PATH_PREFIX': MESA + '/lib',
           '__EGL_VENDOR_LIBRARY_FILENAMES': MESA + '/data/glvnd/egl_vendor.d/50_mesa.json',
           'SUMMIT_SKIA_GL_CONTEXT': '1', 'SUMMIT_SCROLL_REFRESH_TIMER': '16',
           'SUMMIT_ENABLE_INPUT_SYNTHESIS': '1'}
    # SUMMIT_BENCH_EXTRA_ENV="NAME=VALUE NAME=VALUE": traces for one run.
    for item in os.environ.get('SUMMIT_BENCH_EXTRA_ENV', '').split():
        name, _, value = item.partition('=')
        env[name] = value
    group = guest.launch(bundle, profile, 'about:blank', log_path, env)
    entry = {'url': url}
    team = None
    try:
        team = guest.find_browser(ctl, group)
        guest.ctl(ctl, team, 'frame', *window)
        guest.ctl(ctl, team, 'sidebar')
        time.sleep(2)
        started = time.time()
        guest.ctl(ctl, team, 'navigate', url)
        deadline = started + 120
        loading_seen = False
        while time.time() < deadline:
            time.sleep(0.25)
            current = guest.state(ctl, team)
            tabs = current.get('tabs') or [{}]
            tab = tabs[0]
            if tab.get('loading'):
                loading_seen = True
            elif loading_seen or tab.get('loadOutcome') in ('succeeded', 'failed'):
                if tab.get('url', '').startswith('about:'):
                    continue
                break
        entry['navigateSeconds'] = time.time() - started
        time.sleep(settle)
        guest.ctl(ctl, team, 'devtools', 'console')
        time.sleep(2)
        summit_evaluate(ctl, team, SETUP)
        time.sleep(1)
        entry['metrics'] = summit_evaluate(ctl, team, COLLECT)
    except Exception as error:  # noqa: BLE001 - reported per URL
        entry['error'] = repr(error)
    finally:
        guest.terminate(ctl, team or group, group=group)
    return entry


def firefox_loads(urls, profile, settle, window, directory):
    job = {'firefox': '/boot/system/apps/Firefox/Firefox', 'profile': profile, 'urls': urls, 'settle': settle,
           'window': [int(value) for value in window], 'setup': SETUP, 'collect': COLLECT}
    remote = f'{guest.GUEST_ROOT}/bin'
    guest.ssh(f'mkdir -p {remote} && cat > {remote}/marionette_client.py',
              input_bytes=(guest.BENCH / 'marionette_client.py').read_bytes())
    guest.ssh(f'cat > {profile}.job.json', input_bytes=json.dumps(job).encode())
    result = guest.ssh(f'python3.10 {remote}/marionette_client.py {profile}.job.json',
                       timeout=300 * len(urls), check=False)
    (directory / 'firefox.log').open('a').write(result.stdout + result.stderr)
    entries = []
    for line in result.stdout.splitlines():
        try:
            entries.append(json.loads(line))
        except ValueError:
            pass
    return entries


def summarize(results):
    lines = ['| Page | Browser | Loads | ' + ' | '.join(f'{m} cold' for m in METRICS) + ' | '
             + ' | '.join(f'{m} warm' for m in METRICS) + ' | Resources |',
             '| --- | --- | --- |' + ' --- |' * (2 * len(METRICS) + 1)]
    for url in dict.fromkeys(entry['url'] for entry in results):
        for browser in ('summit', 'firefox'):
            runs = [entry for entry in results if entry['url'] == url and entry['browser'] == browser]
            good = [entry for entry in runs if isinstance(entry.get('metrics'), dict)]
            cold = [entry for entry in good if entry['round'] == 0]
            warm = [entry for entry in good if entry['round'] > 0]

            def value(entries, metric):
                values = [entry['metrics'].get(metric) for entry in entries]
                values = [v for v in values if isinstance(v, (int, float)) and v > 0]
                return f'{statistics.median(values):.0f}' if values else '–'
            resources = [entry['metrics'].get('resources') for entry in good]
            lines.append(f'| {url} | {browser} | {len(good)}/{len(runs)} | '
                         + ' | '.join(value(cold, m) for m in METRICS) + ' | '
                         + ' | '.join(value(warm, m) for m in METRICS) + ' | '
                         + (f'{statistics.median(resources):.0f}' if resources else '–') + ' |')
    return '\n'.join(lines) + '\n\nMilliseconds from the start of navigation (medians; warm = rounds after the first).\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bundle', default=guest.DEFAULT_BUNDLE)
    parser.add_argument('--url', action='append', default=[])
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--settle', type=float, default=5)
    parser.add_argument('--browser', choices=['both', 'summit', 'firefox'], default='both')
    parser.add_argument('--window', default='4,24,1916,1076')
    parser.add_argument('--label', default='')
    args = parser.parse_args()
    if guest.IS_VM:
        parser.error('run this with SUMMIT_BENCH_HOST=workstation: the VM has no Firefox and no GPU')
    urls = args.url or DEFAULT_URLS
    window = args.window.split(',')
    run_id = time.strftime('pageload-%Y%m%d-%H%M%S') + (f'-{args.label}' if args.label else '')
    directory = guest.ROOT / '.vm/bench' / run_id
    directory.mkdir(parents=True)
    guest_dir = f'{guest.GUEST_ROOT}/runs/{run_id}'
    guest.ssh(f'mkdir -p {guest_dir}')
    ctl = guest.ensure_ctl()
    if args.browser != 'firefox':
        guest.wait_for_free_gui(ctl, args.bundle)
    guest.wake_display()
    results = []
    facts = {'bundle': args.bundle, 'rounds': args.rounds, 'settle': args.settle, 'urls': urls,
             'load': guest.load_report(ctl, (), 3000)}
    def load_summit(round_index, url):
        log(f'round {round_index + 1}: Summit {url}')
        entry = summit_load(ctl, args.bundle, f'{guest_dir}/summit-profile', url, args.settle, window,
                            f'{guest_dir}/summit-{round_index}.log')
        entry.update(browser='summit', round=round_index)
        results.append(entry)
        log('  ' + json.dumps(entry.get('metrics') or entry.get('error')))

    def load_firefox(round_index, url):
        log(f'round {round_index + 1}: Firefox {url}')
        for entry in firefox_loads([url], f'{guest_dir}/firefox-profile', args.settle, window, directory):
            entry.update(browser='firefox', round=round_index)
            results.append(entry)
            log('  ' + json.dumps(entry.get('metrics') or entry.get('error')))

    for round_index in range(args.rounds):
        for url in urls:
            # Who goes first alternates: some sites' servers (GitHub, The
            # Guardian) answer a page they rendered moments ago in a tenth of
            # the time, so the second browser's first byte comes sooner.
            loaders = []
            if args.browser in ('both', 'summit'):
                loaders.append(load_summit)
            if args.browser in ('both', 'firefox'):
                loaders.append(load_firefox)
            if round_index % 2:
                loaders.reverse()
            for loader in loaders:
                loader(round_index, url)
            (directory / 'results.json').write_text(json.dumps({'facts': facts, 'results': results}, indent=1))
            guest.wake_display()
    summary = summarize(results)
    (directory / 'summary.md').write_text(summary)
    print(summary)
    log(f'results in {directory}')


if __name__ == '__main__':
    main()

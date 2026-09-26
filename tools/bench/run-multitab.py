#!/usr/bin/env python3
"""Many tabs in several windows: load times, cores used, memory, idle cost.

The target load is 10-15 tabs in each of 2-3 windows. One Summit instance (its
own fresh profile, addressed by team id only) opens WINDOWS windows of TABS
real sites each, all at once, the way a restored session does. Then:

  load    every tab's load time (from the browser's own start/finish stamps),
          the selected tabs' load times, and how long until all were done;
          whole-system busy cores sampled every second while it happens
  idle    after the load settles, the cost of the open tabs doing nothing:
          busy cores and each process's share
  scroll  a wheel burst in the first window's selected tab while every tab of
          the other windows reloads: frame rate and long frames under load
  memory  resident memory of every process in the browser's group

Artifacts: .vm/bench/multitab-<id>/{run.json,samples.jsonl,browser.log}.

  SUMMIT_BENCH_HOST=workstation python3 tools/bench/run-multitab.py \\
      --bundle /boot/home/summit/build-modern-browser/bundle-X --windows 3 --tabs 12

The workstation's display is two 4K monitors at 200% (a 3840x1080 logical
desktop); the first window takes the left monitor, the second the right one
and the third sits over the first.
"""
import argparse
import json
import pathlib
import statistics
import sys
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import guest  # noqa: E402

SITES = [
    'https://en.wikipedia.org/wiki/Haiku_(operating_system)',
    'https://www.bbc.com/news',
    'https://www.theguardian.com/international',
    'https://news.ycombinator.com/',
    'https://github.com/WebKit/WebKit',
    'https://developer.mozilla.org/en-US/docs/Web/JavaScript',
    'https://www.youtube.com/',
    'https://stackoverflow.com/questions',
    'https://www.abc.net.au/news',
    'https://arstechnica.com/',
    'https://www.theverge.com/',
    'https://www.apple.com/',
    'https://www.reddit.com/r/haikuOS/',
    'https://www.haiku-os.org/',
    'https://www.python.org/',
    'https://www.rust-lang.org/',
    'https://www.npr.org/',
    'https://www.imdb.com/',
    'https://www.espn.com/',
    'https://www.microsoft.com/en-us/',
    'https://www.openstreetmap.org/#map=12/-42.8821/147.3272',
    'https://www.smh.com.au/',
    'https://www.nationalgeographic.com/',
    'https://www.w3.org/',
    'https://www.gnu.org/',
    'https://www.cnn.com/',
    'https://www.ebay.com/',
    'https://medium.com/',
    'https://www.twitch.tv/',
    'https://www.booking.com/',
    'https://www.weather.com/',
    'https://www.amazon.com/',
    'https://en.wikipedia.org/wiki/WebKit',
    'https://www.nasa.gov/',
    'https://www.economist.com/',
    'https://www.nature.com/',
    'https://www.bloomberg.com/',
    'https://www.washingtonpost.com/',
    'https://www.lemonde.fr/',
    'https://www.spiegel.de/',
    'https://www.etsy.com/',
    'https://www.ikea.com/au/en/',
    'https://www.airbnb.com/',
    'https://www.tripadvisor.com/',
    'https://www.samsung.com/au/',
]

FRAMES = ['0,24,1919,1079', '1920,24,3839,1079', '160,100,1760,1000']


def log(message):
    print(time.strftime('[%H:%M:%S] ') + message, flush=True)


class Sampler(threading.Thread):
    """summitctl sample once a second, tagged with the phase it ran in."""

    def __init__(self, ctl, teams_of, path):
        super().__init__(daemon=True)
        self.ctl, self.teams_of, self.path = ctl, teams_of, path
        self.phase = 'start'
        self.samples = []
        self.stopping = False

    def run(self):
        with open(self.path, 'w') as out:
            while not self.stopping:
                try:
                    data = guest.sample(self.ctl, self.teams_of(), 1000)
                except Exception as error:  # a slow ssh must not end the run
                    log(f'sample failed: {error}')
                    continue
                data['phase'] = self.phase
                self.samples.append(data)
                out.write(json.dumps(data) + '\n')
                out.flush()


def summarize_samples(samples, members):
    """Busy cores and per-role shares for a list of samples."""
    if not samples:
        return None
    roles = {}
    ram = {}
    for sample in samples:
        for team in sample['teams']:
            role = members.get(team['team'])
            if role is None:
                name = (team.get('image') or team['args']).rsplit('/', 1)[-1]
                if name in guest.SYSTEM_SERVERS:
                    role = name
                else:
                    continue
            roles.setdefault(role, []).append(team['cores'])
            if 'ramBytes' in team:
                ram[team['team']] = team['ramBytes']
    busy = [s['busyCores'] for s in samples]
    per_role = {role: round(sum(values) / len(samples), 3) for role, values in roles.items()}
    return {
        'seconds': len(samples),
        'busyCoresMean': round(statistics.mean(busy), 2),
        'busyCoresPeak': round(max(busy), 2),
        'coresByRole': dict(sorted(per_role.items(), key=lambda item: -item[1])),
        'residentMiB': round(sum(ram.values()) / 2**20, 1) if ram else None,
    }


def window_states(ctl, team, windows):
    states = []
    for index in range(windows):
        code, out, err = guest.ctl(ctl, team, '--window', str(index), 'state', timeout_ms=8000)
        if code:
            states.append({'ok': False, 'error': err.strip()})
            continue
        states.append(dict(json.loads(out), ok=True))
    return states


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bundle', default=guest.DEFAULT_BUNDLE)
    parser.add_argument('--windows', type=int, default=3)
    parser.add_argument('--tabs', type=int, default=12, help='tabs per window')
    parser.add_argument('--load-timeout', type=float, default=90)
    parser.add_argument('--idle', type=float, default=30, help='seconds of idle measurement')
    parser.add_argument('--notches', type=int, default=240)
    parser.add_argument('--skip-scroll', action='store_true')
    parser.add_argument('--restore', action='store_true', help='quit and time a relaunch of the saved session')
    parser.add_argument('--env', action='append', default=[], metavar='NAME=VALUE')
    parser.add_argument('--label', default='')
    parser.add_argument('--keep-open', action='store_true')
    args = parser.parse_args()

    needed = args.windows * args.tabs
    if needed > len(SITES):
        raise SystemExit(f'only {len(SITES)} sites are listed')
    sites = SITES[:needed]
    run_id = 'multitab-' + time.strftime('%Y%m%d-%H%M%S') + (f'-{args.label}' if args.label else '')
    local = guest.ROOT / '.vm' / 'bench' / run_id
    local.mkdir(parents=True)
    remote = f'{guest.GUEST_ROOT}/{run_id}'
    guest.require_volume()
    ctl = guest.ensure_ctl()
    guest.wait_for_free_gui(ctl, args.bundle)
    guest.wake_display()
    env = dict(item.split('=', 1) for item in args.env)
    env.setdefault('SUMMIT_ENABLE_INPUT_SYNTHESIS', '1')
    # Frame counting views: tab switch latency and scroll frame statistics.
    env.setdefault('SUMMIT_UI_FRAME_STATS', '1')
    report = {'id': run_id, 'machine': guest.HOST, 'bundle': args.bundle, 'windows': args.windows,
              'tabsPerWindow': args.tabs, 'sites': sites, 'env': env, 'startedAt': time.time()}

    def save():
        (local / 'run.json').write_text(json.dumps(report, indent=2) + '\n')

    group = guest.launch(args.bundle, f'{remote}/profile', 'about:blank', f'{remote}/browser.log', env)
    team = guest.find_browser(ctl, group)
    report['team'] = team
    log(f'Summit team {team}')
    members = {}

    def refresh_members():
        for pid, image in guest.members(ctl, group):
            members[pid] = guest.role(image)
        return list(members)

    sampler = Sampler(ctl, lambda: list(members), local / 'samples.jsonl')
    refresh_members()
    guest.ctl(ctl, team, 'frame', *FRAMES[0].split(','))
    time.sleep(2)
    guest.wake_display()
    sampler.start()

    # Load: every window with all its tabs at once.
    sampler.phase = 'load'
    opened = time.time()
    first = sites[:args.tabs]
    guest.ctl(ctl, team, 'navigate', first[0])
    for url in first[1:]:
        guest.ctl(ctl, team, 'newtab', url)
    for index in range(1, args.windows):
        chunk = sites[index * args.tabs:(index + 1) * args.tabs]
        guest.ctl(ctl, team, 'newwindow', f'frame={FRAMES[index % len(FRAMES)]}', *chunk)
    log(f'opened {needed} tabs in {time.time() - opened:.1f} s')
    deadline = opened + args.load_timeout
    states = []
    while time.time() < deadline:
        time.sleep(2)
        refresh_members()
        states = window_states(ctl, team, args.windows)
        tabs = [tab for state in states if state.get('ok') for tab in state['tabs']]
        loading = [tab for tab in tabs if tab['loading']]
        if len(tabs) >= needed and not loading:
            break
    loaded_at = time.time()
    loads, selected_loads, unfinished = [], [], []
    starts, finishes = [], []
    for state in states:
        if not state.get('ok'):
            continue
        for tab in state['tabs']:
            if tab['loadStartedAt'] and tab['loadFinishedAt'] and tab['loadFinishedAt'] > tab['loadStartedAt']:
                seconds = (tab['loadFinishedAt'] - tab['loadStartedAt']) / 1e6
                loads.append({'url': tab['url'], 'seconds': round(seconds, 2), 'outcome': tab['loadOutcome']})
                starts.append(tab['loadStartedAt'])
                finishes.append(tab['loadFinishedAt'])
                if tab['id'] == state['selected']:
                    selected_loads.append(round(seconds, 2))
            else:
                unfinished.append(tab['url'])
    durations = sorted(item['seconds'] for item in loads)
    report['load'] = {
        'wallSeconds': round(loaded_at - opened, 1),
        'tabs': len(loads) + len(unfinished),
        'finished': len(loads),
        'unfinished': unfinished,
        'medianSeconds': round(statistics.median(durations), 2) if durations else None,
        'p90Seconds': round(durations[int(len(durations) * 0.9) - 1], 2) if durations else None,
        'maxSeconds': durations[-1] if durations else None,
        'allDoneSeconds': round((max(finishes) - min(starts)) / 1e6, 2) if starts else None,
        'selectedTabSeconds': selected_loads,
        'perTab': loads,
        'cpu': summarize_samples([s for s in sampler.samples if s['phase'] == 'load'], members),
    }
    save()
    log(f"load: {report['load']['finished']}/{needed} finished, median {report['load']['medianSeconds']} s, "
        f"all done {report['load']['allDoneSeconds']} s, selected {selected_loads}; cpu {report['load']['cpu']}")

    # Idle: nothing happens; the cost is what the open tabs do by themselves.
    time.sleep(10)
    sampler.phase = 'idle'
    time.sleep(args.idle)
    refresh_members()
    report['idle'] = summarize_samples([s for s in sampler.samples if s['phase'] == 'idle'], members)
    save()
    log(f"idle: {report['idle']}")

    # Switch through the first window's tabs: time from the tab's view being
    # shown to its first frame arriving. A tab whose page has not changed
    # needs no new frame: its view shows the last frame it kept, at once
    # (checked with screenshots in the VM), so those are counted apart.
    sampler.phase = 'switch'
    switches = []
    state = window_states(ctl, team, 1)[0]
    for tab in state.get('tabs', [])[1:] + state.get('tabs', [])[:1]:
        guest.ctl(ctl, team, 'selecttab', str(tab['id']))
        time.sleep(1.5)
        after = window_states(ctl, team, 1)[0]
        shown = next((t for t in after.get('tabs', []) if t['id'] == tab['id']), None)
        if shown and shown.get('shownAt') and shown.get('firstFrameAfterShow'):
            switches.append({'url': shown['url'], 'ms': round((shown['firstFrameAfterShow'] - shown['shownAt']) / 1000, 1)})
        elif shown:
            switches.append({'url': shown['url'], 'ms': None})
    measured = sorted(item['ms'] for item in switches if item['ms'] is not None)
    report['tabSwitch'] = {
        'switches': len(switches),
        'noFrame': sum(1 for item in switches if item['ms'] is None),
        'medianMs': measured[len(measured) // 2] if measured else None,
        'maxMs': measured[-1] if measured else None,
        'perTab': switches,
    }
    save()
    log(f"tab switch: median {report['tabSwitch']['medianMs']} ms, max {report['tabSwitch']['maxMs']} ms, "
        f"{report['tabSwitch']['noFrame']} shown from the kept frame (nothing to repaint)")

    # Scroll the first window while every tab of the others reloads.
    if not args.skip_scroll and args.windows > 1:
        sampler.phase = 'scroll'
        for index in range(1, args.windows):
            state = window_states(ctl, team, args.windows)[index]
            for tab in state.get('tabs', []):
                guest.ctl(ctl, team, '--window', str(index), 'selecttab', str(tab['id']))
                guest.ctl(ctl, team, '--window', str(index), 'reload')
        guest.ctl(ctl, team, 'scroll', str(args.notches), '16', '3')
        time.sleep(args.notches * 0.016 + 3)
        code, out, err = guest.ctl(ctl, team, 'framestats')
        report['scrollUnderLoad'] = {
            'frames': json.loads(out) if not code else {'error': err.strip()},
            'cpu': summarize_samples([s for s in sampler.samples if s['phase'] == 'scroll'], members),
        }
        save()
        log(f"scroll under load: {report['scrollUnderLoad']}")

    sampler.stopping = True
    sampler.join(timeout=10)
    report['memory'] = summarize_samples(sampler.samples[-3:], members)
    report['processes'] = sorted({role for role in members.values()})
    report['processCount'] = {role: sum(1 for r in members.values() if r == role) for role in set(members.values())}
    if args.restore:
        # Quit (the session is saved), relaunch on the same profile and time
        # the selected tabs, as someone reopening the browser sees it.
        report['shutdownBeforeRestore'] = guest.terminate(ctl, team, group=group, grace=60)
        time.sleep(3)
        launched = time.time()
        group = guest.launch(args.bundle, f'{remote}/profile', '', f'{remote}/browser-restore.log', env)
        team = guest.find_browser(ctl, group)
        members.clear()
        restored = {'selectedDoneSeconds': None, 'windows': 0, 'tabs': 0}
        while time.time() < launched + args.load_timeout:
            time.sleep(1)
            states = [state for state in window_states(ctl, team, args.windows) if state.get('ok')]
            selected = [tab for state in states for tab in state['tabs'] if tab['id'] == state['selected']]
            if len(states) == args.windows and selected and all(tab['loadFinishedAt'] for tab in selected):
                restored['selectedDoneSeconds'] = round(time.time() - launched, 1)
                break
        refresh_members()
        time.sleep(10)
        refresh_members()
        restored['windows'] = len(states)
        restored['tabs'] = sum(len(state['tabs']) for state in states)
        restored['webProcesses'] = sum(1 for pid, image in guest.members(ctl, group) if guest.role(image) == 'WebProcess')
        report['restore'] = restored
        save()
        log(f"restore: {restored}")
    if not args.keep_open:
        report['shutdown'] = guest.terminate(ctl, team, group=group)
    guest.fetch_file(f'{remote}/browser.log', local / 'browser.log', tail_bytes=4_000_000)
    report['finishedAt'] = time.time()
    save()
    log(f'report: {local / "run.json"}')


if __name__ == '__main__':
    main()

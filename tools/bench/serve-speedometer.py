#!/usr/bin/env python3
"""Serve a pristine Speedometer 3.1 checkout to the Summit test VM and collect its results.

The benchmark files under .cache/speedometer-3.1 are never modified. Normally,
the only difference from the official deployment is one inline module script
added to the served top-level index.html. It wraps two *client callbacks* on
``globalThis.benchmarkClient`` (``didFinishLastIteration`` and ``handleError``),
which the runner invokes once, after the last measured iteration. Nothing runs
inside the measured sync/async windows and no timers or observers are installed.
An optional CodeMirror-only diagnostic wraps IntersectionObserver callbacks in
the served iframe and uploads their aggregate duration when the iframe unloads.

Response headers mirror https://browserbench.org/Speedometer3.1/ (checked
2026-09-18): ETag + Last-Modified validators without Cache-Control, plus
COOP same-origin / COEP require-corp. The isolation headers matter: WebKit gives
cross-origin-isolated documents a finer performance.now() resolution, so
dropping them would change what the benchmark measures.

Guest URL (QEMU user networking): http://10.0.2.2:<port>/?startAutomatically=true
"""
import argparse
import datetime
import email.utils
import hashlib
import http.server
import json
import mimetypes
import pathlib
import subprocess
import sys
import threading
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_SOURCE = ROOT / '.cache/speedometer-3.1'
DEFAULT_PORT = 8931
PAGES = pathlib.Path(__file__).resolve().parent / 'pages'

MIME = {
    '.html': 'text/html; charset=UTF-8', '.htm': 'text/html; charset=UTF-8',
    '.mjs': 'text/javascript', '.js': 'text/javascript', '.cjs': 'text/javascript',
    '.css': 'text/css', '.json': 'application/json', '.map': 'application/json',
    '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
    '.gif': 'image/gif', '.webp': 'image/webp', '.avif': 'image/avif', '.ico': 'image/x-icon',
    '.woff': 'font/woff', '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.otf': 'font/otf',
    '.wasm': 'application/wasm', '.txt': 'text/plain; charset=UTF-8', '.md': 'text/markdown; charset=UTF-8',
    '.xml': 'application/xml', '.webmanifest': 'application/manifest+json',
}

# Runs as a deferred inline module *after* resources/main.mjs has created
# globalThis.benchmarkClient and *before* DOMContentLoaded starts the run.
COLLECTOR = r'''<script type="module" id="summit-bench-collector">
const client = globalThis.benchmarkClient;
// No keepalive: Fetch caps keepalive bodies at 64 KiB and a 10-iteration result is larger.
const send = (kind, payload) => fetch("/__bench/" + kind, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
}).catch((error) => { document.title = "result upload failed: " + error; });
const environment = () => ({
    href: location.href, userAgent: navigator.userAgent,
    crossOriginIsolated: globalThis.crossOriginIsolated === true,
    hardwareConcurrency: navigator.hardwareConcurrency,
    innerWidth, innerHeight, devicePixelRatio,
    visibilityState: document.visibilityState,
});
if (!client) {
    send("error", { message: "globalThis.benchmarkClient is missing", environment: environment() });
} else {
    const timing = { pageLoadedAt: Date.now() };
    const wrap = (name, after) => {
        const original = client[name];
        client[name] = function (...args) {
            const value = typeof original === "function" ? original.apply(this, args) : undefined;
            try {
                const pending = after.apply(this, args);
                if (pending && pending.catch)
                    pending.catch((error) => send("error", { message: String(error), hook: name }));
            } catch (error) { send("error", { message: String(error), hook: name }); }
            return value;
        };
    };
    wrap("willStartFirstIteration", () => { timing.startedAt = Date.now(); });
    wrap("didFinishLastIteration", async function (metrics) {
        timing.finishedAt = Date.now();
        const { params } = await import("./resources/params.mjs");
        const plain = JSON.parse(JSON.stringify(metrics));
        send("result", {
            benchmark: "Speedometer 3.1", instrumented: /*PROGRESS*/false,
            params: JSON.parse(JSON.stringify(params)), timing, environment: environment(),
            displayed: {
                score: document.getElementById("result-number").textContent,
                confidence: document.getElementById("confidence-number").textContent,
                valid: document.getElementById("summary").className,
            },
            score: plain.Score, geomean: plain.Geomean,
            metrics: plain, measuredValuesList: this._measuredValuesList,
        });
    });
    wrap("handleError", (error) => send("error", {
        message: String(error), stack: error && error.stack, timing, environment: environment(),
    }));
    /*PROGRESS_HOOK*/
}
</script>
'''

# Optional and off by default: one sendBeacon per test, issued from willRunTest
# (outside the timed region, but it does add network activity to the run, so
# results collected this way are marked "instrumented").
PROGRESS_HOOK = r'''
    let step = 0;
    let last = null;
    const beacon = (phase, suite, test) => {
        last = { phase, suite: suite && suite.name, test: test && test.name, at: Date.now() };
        navigator.sendBeacon("/__bench/progress", JSON.stringify({ step: step++, ...last }));
    };
    wrap("willRunTest", (suite, test) => beacon("will", suite, test));
    wrap("didRunTest", (suite, test) => beacon("did", suite, test));
    wrap("didRunSuites", () => beacon("didSuites", null, null));
    // A hang shows up as "the last beacon stopped moving". Beacons travel through
    // the network process, so the tick also goes into the document title, which
    // the harness reads over the browser's own control channel: if the title
    // keeps counting while the beacons stop, the page is alive and the network
    // path is not.
    let tick = 0;
    const baseTitle = document.title;
    setInterval(() => {
        ++tick;
        document.title = baseTitle + " [tick " + tick + "]";
        navigator.sendBeacon("/__bench/progress",
            JSON.stringify({ step: -1, phase: "alive", tick, since: last && (Date.now() - last.at), last }));
    }, 2000);
'''

INTERSECTION_TRACE = r'''<script id="summit-intersection-trace">
(() => {
    const NativeObserver = window.IntersectionObserver;
    if (!NativeObserver)
        return;
    const deferGap = /*DEFER_GAP*/false;
    const observers = [];
    const gapMeasure = window.__summitGapMeasure = {
        viewMeasure: 0, forceFlush: 0, scrollDOM: 0,
        lineBlock: 0, viewStateMeasure: 0, measureReads: 0, measureWrites: 0,
        updatePlugins: 0, inputStateUpdate: 0, updateAttrs: 0,
        docViewUpdate: 0, lineBlockAt: 0, updateSelection: 0,
        loops: 0, updates: 0, redraws: 0, changes: {}, viewStateSamples: [], timeline: [],
    };
    window.__summitMeasureStep = (name, callback) => {
        if (!window.__summitInGapCallback)
            return callback();
        const start = performance.now();
        try { return callback(); }
        finally { gapMeasure[name] += performance.now() - start; }
    };
    function TracedObserver(callback, options) {
        const record = { callbacks: 0, entries: 0, duration: 0, maxDuration: 0,
            targets: [], positiveCallbacks: 0, samples: [],
            origin: new Error().stack?.split("\n").slice(1, 4).join(" | ") || "" };
        const observerIndex = observers.length;
        const runCallback = (entries, instance) => {
            const start = performance.now();
            if (observerIndex === 2) {
                window.__summitInGapCallback = true;
                if (gapMeasure.timeline.length < 10)
                    gapMeasure.timeline.push({event: "gap", at: start,
                        ratio: entries[entries.length - 1]?.intersectionRatio});
            }
            try {
                return callback(entries, instance);
            } finally {
                if (observerIndex === 2)
                    window.__summitInGapCallback = false;
                const duration = performance.now() - start;
                const last = entries[entries.length - 1];
                if (last?.intersectionRatio > 0)
                    ++record.positiveCallbacks;
                if (last && record.samples.length < 4)
                    record.samples.push({ratio: last.intersectionRatio,
                        isIntersecting: last.isIntersecting,
                        target: [last.boundingClientRect?.top, last.boundingClientRect?.bottom],
                        root: [last.rootBounds?.top, last.rootBounds?.bottom]});
                ++record.callbacks;
                record.entries += entries.length;
                record.duration += duration;
                record.maxDuration = Math.max(record.maxDuration, duration);
            }
        };
        const observer = new NativeObserver((entries, instance) => {
            if (deferGap && observerIndex === 2)
                requestAnimationFrame(() => runCallback(entries, instance));
            else
                runCallback(entries, instance);
        }, options);
        const nativeObserve = observer.observe.bind(observer);
        observer.observe = (target) => {
            if (record.targets.length < 4) {
                const classes = typeof target.className === "string" ? target.className.split(/\s+/).slice(0, 3).join(".") : "";
                record.targets.push(`${target.tagName || "?"}${classes ? "." + classes : ""}`);
            }
            return nativeObserve(target);
        };
        observers.push(record);
        return observer;
    }
    TracedObserver.prototype = NativeObserver.prototype;
    Object.setPrototypeOf(TracedObserver, NativeObserver);
    window.IntersectionObserver = TracedObserver;
    window.addEventListener("pagehide", () => {
        navigator.sendBeacon("/__bench/progress", JSON.stringify({
            phase: "intersection-callbacks", page: location.pathname, observers,
            gapMeasure,
        }));
    });
})();
</script>
'''

CODEMIRROR_MEASURE_TRACE = (
    (b'    this.updateState = 0;\n    this.requestMeasure();\n    if (config2.parent)',
     b'    this.updateState = 0;\n    if (window.__summitGapMeasure?.timeline.length < 10) window.__summitGapMeasure.timeline.push({event: "constructed", at: performance.now()});\n    this.requestMeasure();\n    if (config2.parent)'),
    (b'  measure(flush = true) {\n    if (this.destroyed)',
     b'  measure(flush = true) {\n    if (window.__summitGapMeasure?.timeline.length < 10) window.__summitGapMeasure.timeline.push({event: "measure", at: performance.now(), inGap: !!window.__summitInGapCallback, previousContentHeight: this.viewState.contentDOMHeight});\n    if (this.destroyed)'),
    (b'  measure(view) {\n    let dom = view.contentDOM, style = window.getComputedStyle(dom);',
     b'  measure(view) {\n    let __summitViewSample = window.__summitInGapCallback && window.__summitGapMeasure.viewStateSamples.length < 6 ? {previousContentHeight: this.contentDOMHeight, previousContentWidth: this.contentDOMWidth, previousEditorWidth: this.editorWidth, previousEditorHeight: this.editorHeight, previousPadding: [this.paddingTop, this.paddingBottom], previousViewport: [this.pixelViewport.top, this.pixelViewport.bottom]} : null;\n    let dom = view.contentDOM, style = window.getComputedStyle(dom);'),
    (b'    let measureContent = refresh || this.mustMeasureContent || this.contentDOMHeight != domRect.height;',
     b'    let measureContent = refresh || this.mustMeasureContent || this.contentDOMHeight != domRect.height;\n    if (__summitViewSample) Object.assign(__summitViewSample, {domHeight: domRect.height, domWidth: domRect.width, scrollWidth: view.scrollDOM.clientWidth, scrollHeight: view.scrollDOM.clientHeight, initialRefresh: refresh, initialMeasureContent: measureContent});'),
    (b'    let dTop = pixelViewport.top - this.pixelViewport.top, dBottom = pixelViewport.bottom - this.pixelViewport.bottom;',
     b'    let dTop = pixelViewport.top - this.pixelViewport.top, dBottom = pixelViewport.bottom - this.pixelViewport.bottom;\n    if (__summitViewSample) __summitViewSample.pixelViewport = [pixelViewport.top, pixelViewport.bottom];'),
    (b'    if (!this.inView && !this.scrollTarget)\n      return 0;',
     b'    if (!this.inView && !this.scrollTarget) {\n      if (__summitViewSample) { __summitViewSample.early = true; window.__summitGapMeasure.viewStateSamples.push(__summitViewSample); }\n      return 0;\n    }'),
    (b'    if (this.mustEnforceCursorAssoc) {\n      this.mustEnforceCursorAssoc = false;\n      view.docView.enforceCursorAssoc();\n    }\n    return result;',
     b'    if (this.mustEnforceCursorAssoc) {\n      this.mustEnforceCursorAssoc = false;\n      view.docView.enforceCursorAssoc();\n    }\n    if (__summitViewSample) { Object.assign(__summitViewSample, {result, finalRefresh: refresh, finalMeasureContent: measureContent, viewportChange, heightChanged: oracle.heightChanged}); window.__summitGapMeasure.viewStateSamples.push(__summitViewSample); }\n    return result;'),
    (b'if (this.intersecting)\n      this.view.measure();',
     b'if (this.intersecting)\n      window.__summitMeasureStep("viewMeasure", () => this.view.measure());'),
    (b'this.observer.forceFlush();',
     b'window.__summitMeasureStep("forceFlush", () => this.observer.forceFlush());'),
    (b'let { scrollHeight, scrollTop, clientHeight } = this.scrollDOM;',
     b'let { scrollHeight, scrollTop, clientHeight } = window.__summitMeasureStep("scrollDOM", () => ({scrollHeight: this.scrollDOM.scrollHeight, scrollTop: this.scrollDOM.scrollTop, clientHeight: this.scrollDOM.clientHeight}));'),
    (b'let refBlock = this.viewState.lineBlockAtHeight(refHeight);',
     b'let refBlock = window.__summitMeasureStep("lineBlock", () => this.viewState.lineBlockAtHeight(refHeight));'),
    (b'      for (let i = 0; ; i++) {',
     b'      for (let i = 0; ; i++) {\n        if (window.__summitInGapCallback) window.__summitGapMeasure.loops++;'),
    (b'let changed = this.viewState.measure(this);',
     b'let changed = window.__summitMeasureStep("viewStateMeasure", () => this.viewState.measure(this));\n        if (window.__summitInGapCallback) { let counts = window.__summitGapMeasure.changes; counts[changed] = (counts[changed] || 0) + 1; }'),
    (b'        let measured = measuring.map((m) => {',
     b'        let measured = window.__summitMeasureStep("measureReads", () => measuring.map((m) => {'),
    (b'        });\n        let update = ViewUpdate.create(this, this.state, []), redrawn = false, scrolled = false;',
     b'        }));\n        let update = ViewUpdate.create(this, this.state, []), redrawn = false, scrolled = false;'),
    (b'        if (!update.empty) {',
     b'        if (!update.empty) {\n          if (window.__summitInGapCallback) window.__summitGapMeasure.updates++;'),
    (b'          this.updatePlugins(update);',
     b'          window.__summitMeasureStep("updatePlugins", () => this.updatePlugins(update));'),
    (b'          this.inputState.update(update);',
     b'          window.__summitMeasureStep("inputStateUpdate", () => this.inputState.update(update));'),
    (b'          this.updateAttrs();',
     b'          window.__summitMeasureStep("updateAttrs", () => this.updateAttrs());'),
    (b'          redrawn = this.docView.update(update);',
     b'          redrawn = window.__summitMeasureStep("docViewUpdate", () => this.docView.update(update));\n          if (redrawn && window.__summitInGapCallback) window.__summitGapMeasure.redraws++;'),
    (b'        for (let i2 = 0; i2 < measuring.length; i2++)',
     b'        let __summitWriteStarted = window.__summitInGapCallback ? performance.now() : 0;\n        for (let i2 = 0; i2 < measuring.length; i2++)'),
    (b'        if (this.viewState.editorHeight) {',
     b'        if (__summitWriteStarted) window.__summitGapMeasure.measureWrites += performance.now() - __summitWriteStarted;\n        if (this.viewState.editorHeight) {'),
    (b'let diff = this.viewState.lineBlockAt(refBlock.from).top - refBlock.top;',
     b'let diff = window.__summitMeasureStep("lineBlockAt", () => this.viewState.lineBlockAt(refBlock.from)).top - refBlock.top;'),
    (b'          this.docView.updateSelection(true);',
     b'          window.__summitMeasureStep("updateSelection", () => this.docView.updateSelection(true));'),
)


class State:
    def __init__(self, source, out_dir, cache_policy, progress, intersection_trace, intersection_defer_gap):
        self.source = source
        self.out_dir = out_dir
        self.cache_policy = cache_policy
        self.progress = progress
        self.intersection_trace = intersection_trace
        self.intersection_defer_gap = intersection_defer_gap
        self.lock = threading.Lock()
        self.started = time.time()
        out_dir.mkdir(parents=True, exist_ok=True)
        try:
            self.source_date = int(subprocess.check_output(
                ['git', '-C', str(source), 'log', '-1', '--format=%ct'], text=True).strip())
        except (OSError, subprocess.CalledProcessError, ValueError):
            self.source_date = int((source / 'index.html').stat().st_mtime)
        self.request_log = (out_dir / 'requests.log').open('a', buffering=1)

    def log(self, line):
        with self.lock:
            self.request_log.write(f'{time.time():.3f} {line}\n')

    def write_json(self, name, payload, append=False):
        with self.lock:
            if append:
                with (self.out_dir / name).open('a') as stream:
                    stream.write(json.dumps(payload) + '\n')
                return
            temporary = self.out_dir / (name + '.tmp')
            temporary.write_text(json.dumps(payload, indent=1))
            temporary.replace(self.out_dir / name)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'  # keep-alive; every response carries Content-Length
    server_version = 'SummitBench/1'
    state = None

    def log_message(self, fmt, *args):
        self.state.log(f'{self.client_address[0]} {fmt % args}')

    def common_headers(self):
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')

    def respond(self, status, body=b'', content_type='text/plain; charset=UTF-8', headers=(), head=False):
        self.send_response(status)
        self.common_headers()
        if status != 304:
            self.send_header('Content-Type', content_type)
        if status != 304:  # a 304 never has a body; a zero length could poison the cached entry
            self.send_header('Content-Length', str(len(body)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        if body and not head and status != 304:
            self.wfile.write(body)

    def resolve(self, url_path):
        relative = urllib.parse.unquote(url_path).lstrip('/')
        if relative.startswith('__bench/pages/'):
            base, relative = PAGES, relative[len('__bench/pages/'):]
        else:
            base = self.state.source
        target = (base / relative).resolve()
        if base.resolve() != target and base.resolve() not in target.parents:
            return None
        if target.is_dir():
            target = target / 'index.html'
        return target if target.is_file() else None

    def do_HEAD(self):
        self.do_GET(head=True)

    def do_GET(self, head=False):
        split = urllib.parse.urlsplit(self.path)
        if split.path == '/__bench/status':
            body = json.dumps({'uptime': time.time() - self.state.started,
                               'result': (self.state.out_dir / 'result.json').exists(),
                               'error': (self.state.out_dir / 'error.json').exists()}).encode()
            return self.respond(200, body, 'application/json', [('Cache-Control', 'no-store')], head)
        target = self.resolve(split.path)
        if target is None:
            return self.respond(404, b'Not found\n', head=head)
        is_entry = target == (self.state.source / 'index.html').resolve()
        base_is_source = not split.path.startswith('/__bench/')
        data = target.read_bytes()
        if is_entry:
            script = COLLECTOR.replace('/*PROGRESS*/false', 'true' if (self.state.progress or self.state.intersection_trace) else 'false')
            script = script.replace('/*PROGRESS_HOOK*/', PROGRESS_HOOK if self.state.progress else '')
            marker = b'</head>'
            if data.count(marker) != 1:
                return self.respond(500, b'Unexpected Speedometer index.html layout\n', head=head)
            data = data.replace(marker, script.encode() + marker)
        elif self.state.intersection_trace and split.path == '/resources/editors/dist/codemirror.html':
            marker = b'<script type="module" crossorigin src="./assets/codemirror-521de7ab.js"></script>'
            if data.count(marker) != 1:
                return self.respond(500, b'Unexpected CodeMirror iframe layout\n', head=head)
            trace = INTERSECTION_TRACE.replace('/*DEFER_GAP*/false', 'true' if self.state.intersection_defer_gap else 'false')
            data = data.replace(marker, trace.encode() + marker)
        elif self.state.intersection_trace and split.path == '/resources/editors/dist/assets/codemirror-521de7ab.js':
            for original, replacement in CODEMIRROR_MEASURE_TRACE:
                if data.count(original) != 1:
                    return self.respond(500, b'Unexpected CodeMirror asset layout\n', head=head)
                data = data.replace(original, replacement)
        stat = target.stat()
        etag = '"' + hashlib.sha256(data).hexdigest()[:24] + '"'
        # Heuristic freshness is a fraction of (now - Last-Modified). A fresh clone has
        # mtime == now, which would force a revalidation per suite load; the release
        # commit date gives the long-lived freshness the official deployment has.
        modified_at = self.state.source_date if base_is_source else stat.st_mtime
        modified = email.utils.formatdate(modified_at, usegmt=True)
        headers = [('ETag', etag), ('Last-Modified', modified)]
        policy = 'no-store' if is_entry or split.path.startswith('/__bench/') else self.state.cache_policy
        if policy == 'no-store':
            headers = [('Cache-Control', 'no-store')]
        elif policy == 'revalidate':
            headers.append(('Cache-Control', 'no-cache'))
        # policy == 'official': validators only, heuristic freshness, like browserbench.org
        if policy != 'no-store':
            if self.headers.get('If-None-Match') == etag:
                return self.respond(304, headers=headers, head=head)
            since = self.headers.get('If-Modified-Since')
            if since and not self.headers.get('If-None-Match'):
                try:
                    if email.utils.parsedate_to_datetime(since).timestamp() >= int(modified_at):
                        return self.respond(304, headers=headers, head=head)
                except (TypeError, ValueError):
                    pass
        content_type = MIME.get(target.suffix.lower()) or mimetypes.guess_type(target.name)[0] or 'application/octet-stream'
        self.respond(200, data, content_type, headers, head)

    def do_POST(self):
        kind = urllib.parse.urlsplit(self.path).path.removeprefix('/__bench/')
        if kind not in ('result', 'error', 'progress', 'probe'):
            return self.respond(404, b'Not found\n')
        length = int(self.headers.get('Content-Length') or 0)
        if length > 64 * 1024 * 1024:
            return self.respond(413, b'Too large\n')
        try:
            payload = json.loads(self.rfile.read(length) or b'null')
        except ValueError:
            return self.respond(400, b'Invalid JSON\n')
        received = datetime.datetime.now(datetime.timezone.utc).isoformat()
        if kind == 'progress':
            self.state.write_json('progress.jsonl', payload, append=True)
        else:
            name = {'result': 'result.json', 'error': 'error.json', 'probe': 'probe.json'}[kind]
            self.state.write_json(name, {'receivedAt': received, 'payload': payload})
            print(f'[serve-speedometer] received {kind} -> {self.state.out_dir / name}', flush=True)
        self.respond(200, b'OK\n', headers=[('Cache-Control', 'no-store')])


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 128


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', type=pathlib.Path, default=DEFAULT_SOURCE)
    parser.add_argument('--port', type=int, default=DEFAULT_PORT)
    parser.add_argument('--bind', default='127.0.0.1',
                        help='listen address (0.0.0.0 to serve the workstation over the LAN)')
    parser.add_argument('--out-dir', type=pathlib.Path, default=None,
                        help='where result.json/requests.log go (default .vm/bench/manual-<timestamp>)')
    parser.add_argument('--cache-policy', choices=['official', 'revalidate', 'no-store'], default='official',
                        help='official: ETag/Last-Modified only, as browserbench.org (default); '
                             'revalidate: Cache-Control no-cache; no-store: never cached')
    parser.add_argument('--progress-beacons', action='store_true',
                        help='also report each test start (adds network activity; result marked instrumented)')
    parser.add_argument('--intersection-trace', action='store_true',
                        help='time CodeMirror IntersectionObserver callbacks (diagnostic; result marked instrumented)')
    parser.add_argument('--intersection-defer-gap', action='store_true',
                        help='deliver CodeMirror gap observer callbacks in the next animation frame (diagnostic only)')
    args = parser.parse_args()
    if args.intersection_defer_gap and not args.intersection_trace:
        parser.error('--intersection-defer-gap requires --intersection-trace')
    source = args.source.resolve()
    if not (source / 'resources/benchmark-runner.mjs').is_file():
        sys.exit(f'{source} is not a Speedometer checkout; see docs/performance.md')
    out_dir = (args.out_dir or ROOT / '.vm/bench' / time.strftime('manual-%Y%m%d-%H%M%S')).resolve()
    Handler.state = State(source, out_dir, args.cache_policy, args.progress_beacons,
                          args.intersection_trace, args.intersection_defer_gap)
    server = Server((args.bind, args.port), Handler)
    print(f'[serve-speedometer] {source} on http://{args.bind}:{args.port}/'
          f' cache={args.cache_policy} out={out_dir}', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()

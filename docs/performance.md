# Summit performance: Speedometer 3.1 baseline, where the time goes, stress test

## Current coordinated Skia result (September 24, 2026)

The workstation launcher uses `bundle-w9ti9d76`: Skia CPU tile painting with
two workers, GL Canvas, raster coordinated scrollbars, mimalloc, asynchronous
scrolling, display-rate composition pacing, guarded reuse of exact text
widths, and preserved font registrations for simple CSS rule insertions. It
completes all 580 steps of Speedometer 3.1. The latest matched-viewport
ten-iteration run is
`.vm/bench/speedometer-20260924-221933-async-seek-full/`.

| Browser | Content viewport | Speedometer 3.1, 10 iterations |
| --- | ---: | ---: |
| Summit, current CPU tiles and GL Canvas | 1280×887 | **7.298 ± 0.333** |
| Summit, prior synchronous media seek build | 1280×887 | **7.288 ± 0.359** |
| Summit, earlier CPU-tile build with GL Canvas off | 1280×887 | **6.730 ± 0.302** |
| Summit, earlier batched CSS and mimalloc build | 1280×887 | **6.896 ± 0.293** |
| Firefox 155 on the same workstation | 1280×887 | **8.338 ± 0.374** |

These Summit runs were uncontended. The current same-size gap to Firefox is
1.14x by score. GL Canvas makes Chart.js and Perf Dashboard faster than
Firefox in this run; the largest remaining gaps are Observable Plot, Preact,
CodeMirror, TipTap, and other DOM-heavy suites. Older score comparisons in
this log used different viewport sizes and should be treated as directional.
`tools/bench/compare-runs.py` flags that mismatch.
`tools/bench/compare-runs.py` gives the per-suite synchronous and asynchronous
split from the saved results.

The workstation reports 32 logical processors (16 physical cores) through
both Haiku's system information and `sysconf`. WebKit's JSC and garbage
collector discover that count independently. Summit caps Skia CPU tile
painting at two workers on Haiku because the controlled sweep below found no
Speedometer gain from four and a large regression from eight. With GL Canvas
on, the current 400-card scrolling probe ran at 58.88 fps over 600 frames,
with p95 18 ms, p99 22 ms, a 24 ms maximum, and no frame above 33 ms
(`.vm/bench/probe-20260924-222157-summit-async-seek-scroll/`). Earlier
live Reddit scrolls showed occasional 650–790 ms gaps attributed to page
update, layout, or script work; subsequent live Reddit attempts sometimes
received a JavaScript challenge instead of the feed.

## Scrolling (September 21, 2026)

Scrolling a busy page was the owner's first complaint, so the drawing area now
reports what it does with every frame. `SUMMIT_FRAME_STATS=<seconds>` prints one
line per period to the WebProcess's stderr:

```
Summit frames: 27.4/s over 1.0 s (painted=28 unchanged=0 dropped=0) update=0.70 ms
  paint=33.8 ms flush=… present=2.1 ms interval=36.7 ms (max 43.8) longest frame=41.7 ms
  dirty=0.14 Mpx/frame
```

`update` is style, layout and script; `paint` is walking the page and sending
drawing commands; `flush` is waiting for app_server to carry them out;
`present` is the UI process copying and showing the frame. It works on any page,
including one that cannot be instrumented from JavaScript.

Two changes came out of it, measured in the VM (1280x800, software painting)
with `tools/bench/pages/scroll.html`, a 400-card feed that scrolls itself from
`requestAnimationFrame` and reports its own frame intervals:

| Configuration | Scrolling | Median frame | p95 |
| --- | --- | --- | --- |
| **everything below, together** | **34.8 fps** | 28 ms | 37 ms |
| without the shadow scratch buffer | 27.4 fps | 36 ms | 43 ms |
| without the presentation bitmap pool (`SUMMIT_PRESENT_POOL=0`) | 26.5 fps | 37 ms | 45 ms |
| without frame pacing (`SUMMIT_FRAME_PACING=ack`) | 18.3 fps | 54 ms | 62 ms |
| without scroll copying (`SUMMIT_SCROLL_COPY=0`) | 15.4 fps | 64 ms | 76 ms |
| **as before this work** (pacing, copying and pool all off) | **12.2 fps** | 81 ms | 91 ms |

That is 2.8x on the same page and machine. An idle page reaches 59.5 fps in
every configuration, so the cost is all in producing changed frames.

1. **Frame pacing.** A frame used to be scheduled 16 ms after the UI process
   acknowledged the previous one, so painting and presentation were added to
   every interval: a page that paints in 10 ms was held near 30 fps however much
   headroom the machine had. A frame is now due one display period after the
   previous frame *started*.
2. **Scroll copying.** The frame bitmap survives between frames, so what a
   scroll keeps on screen is moved inside it (`DrawingAreaHaiku::scroll()` keeps
   the scrolled rectangle and offset, as `DrawingAreaCoordinatedGraphics` does,
   and `copyScrolledPixels()` moves the rows) and only the strip that came into
   view is painted. Painted area per frame fell from 0.90 Mpx to 0.14 Mpx and
   painting from 65 ms to 34 ms.
3. **Presentation bitmap pool.** The UI process used to construct a `BBitmap`
   for every frame, which asks app_server for a shared area and registers it.
   Frames are now recycled through a small pool as views release them, and
   `BWebKitView::Draw()` copies only the invalidated rectangle.
4. **Shadows** (below) stopped allocating an `ImageBuffer` per shadow per
   frame.

Painting is what is left: 34 ms for 0.14 Mpx of this page. Waiting for
app_server to finish (`flush`) is 0.1 ms of that, so the cost is all in the web
process. Turning parts of the page's style off says which part:

| Page style | Painting | Scrolling |
| --- | --- | --- |
| cards with `box-shadow` and gradients (as above) | 34.0 ms | 27.4 fps |
| `style=nogradient` (gradients replaced by flat colour) | 33.7 ms | 27.8 fps |
| `style=noshadow` (no `box-shadow`) | **10.6 ms** | **59.2 fps** |
| `style=flat` (neither) | 11.7 ms | 59.2 fps |

`box-shadow` costs 23 ms a frame — more than twice everything else put
together — and gradients cost nothing. The reason is `ShadowBlur`: upstream
keeps the blurred shadow template in a scratch `ImageBuffer` between draws, but
only `#if USE(CG)`. Every other port allocates an `ImageBuffer` for every shadow
of every frame, and on Haiku an `ImageBuffer` is an app_server offscreen window
with a `BView` and a thread, about 0.84 ms each. The scratch buffer is written
in port-independent terms, so the Haiku port now enables it too, stops redrawing
the template for every differently sized element (only the template goes into
the buffer, and it does not depend on that size), and asks app_server to finish
once per buffer instead of once for each of the nine pieces a tiled shadow is
drawn from (`ImageBufferHaikuSurfaceBackend::syncDrawing()`).

## The workstation, 1913x935 (September 21, 2026)

Same page, same harness, on the GeForce machine (`docs/workstation.md`), with a
viewport 2.2x the area of the VM's:

| Path | Scrolling | Painting | Flush | Present | Dirty |
| --- | --- | --- | --- | --- | --- |
| software | **42.1 fps** | 19.5 ms | 0.05 ms | 3.9 ms | 0.21 Mpx |
| GL compositing (zink on NVK) | 10.0 fps | 105 ms | 0.00 ms | 4.4 ms | whole view |

An idle page reaches 59.6 fps on both.

**GPU compositing works on this hardware and is four times slower.** The
compositor is selected automatically and reports
`zink Vulkan 1.3(NVIDIA GeForce GTX 1070 (NVK GP104-A) (MESA_NVK)),
OpenGL ES 3.1 Mesa 25.3.6`, renders correctly, and reads back a frame in about
4 ms — the readback is not the problem, and an idle page still reaches 59.6 fps
through it. Scrolling is: `DrawingAreaHaiku::scroll()` invalidates the scrolled
area of the non-composited content layer, so TextureMapper repaints every tile
that intersects the viewport, each through the same app_server drawing path,
and only then composites. Nothing about the GPU is used for the part that
costs. The software path keeps its scroll copy and wins.

Making it pay off needs the tiles to live in page coordinates and move when the
page scrolls, as CoordinatedGraphics does, so that a scroll paints only the
newly exposed tiles. Until then software painting stays the default, which it
is in practice: the stock Haiku Mesa cannot initialize EGL, so GL is only
chosen when the private Mesa is on `LIBRARY_PATH`.

## What painting is made of (September 21, 2026)

`SUMMIT_FRAME_STATS` now also tallies what each frame asks the port to draw,
which is how the guesses above were replaced with numbers. A second line goes
with the frame line:

```
Summit painting: per frame drawing calls 14.37 ms of which 11 text runs
  (410 glyphs) 0.07 ms, 83 images 2.05 ms; 32 fills, 0 strokes, 41 clips,
  558 state changes 0.01 ms
```

Measuring it costs about 3.5% (44.9 fps becomes 43.4), and nothing at all when
`SUMMIT_FRAME_STATS` is unset.

On the scroll page, at 1913x935, painting a frame was 20.65 ms and **text was
0.07 ms of it** — drawing glyphs, the thing that looked most suspicious in the
code, is not worth optimising. What the tally did show:

- **Shadow templates were thrashing a one-slot cache.** The cards carry two
  shadows (`0 1px 3px` and `0 8px 20px`), and `ShadowBlur`'s scratch buffer held
  exactly one template, so the two evicted each other and *every* draw redrew
  and reblurred its template, with a `getPixelBuffer`/`putPixelBuffer` round
  trip each time. The scratch buffer now keeps four templates, chosen by what
  they were drawn from and evicted least-recently-used
  (`SUMMIT_SHADOW_CACHE=0` restores an `ImageBuffer` per shadow).
- **Every `PopState()` flushed the link to app_server.** `BView::PopState()`
  calls `_FlushIfNotInTransaction()`, and WebKit brackets nearly everything in
  a state saver: 558 save/restore pairs a frame, so 558 forced `write_port`
  calls and no batching at all. Painting a frame, and drawing into an
  `ImageBuffer`, now runs inside a `BWindow` view transaction, which holds the
  flushes back until the drawing is done (`SUMMIT_BATCH_DRAWING=0` turns it
  off).

| Scroll page, workstation | Painting | Scrolling |
| --- | --- | --- |
| before this round | 20.65 ms | 42.1 fps |
| four shadow template slots | 19.5 ms | 42.7 fps |
| + batched drawing | **17.2 ms** | **44.9 fps** |

What is left of those 17.2 ms: 14.4 ms inside the port's drawing calls and
2.8 ms of WebCore deciding what to draw. Inside the drawing calls, images are
2.1 ms, text is 0.1 ms, and the remaining ~12 ms belongs to 32 fills, 41 clips
and **558 state changes** — about seventeen `PushState()`/`PopState()` pairs for
every drawing operation. Most of those pairs enclose no drawing at all, which
suggested holding the pushes back and applying them only when a call needs the
state. That was tried next, and measured at 0.07 ms; the section below has what
the time is really spent on.

## Two hypotheses, measured and dropped (September 21, 2026)

The paint tally above ended with a prediction: that the 558 state changes a
frame were the next thing worth removing. The tally was extended to time them,
and to count the one cost it had not yet looked at — the calls that *read* state
back out of app_server. Both halves of the prediction turned out to be wrong,
and one of them turned out to be dangerous.

```
Summit painting: per frame drawing calls 14.65 ms of which 11 text runs
  (420 glyphs) 0.07 ms, 83 images 2.04 ms; 32 fills 18.02 ms, 0 strokes,
  41 clips 0.05 ms, 558 state changes 0.01 ms (0.07 ms in save/restore),
  75 app_server round trips 2.09 ms (10 for the clip)
```

- **State save/restore is 0.07 ms a frame.** Once the flushes were held back by
  the view transaction, the 558 `PushState()`/`PopState()` pairs cost almost
  nothing: they are a few bytes each into an already-batched `PortLink`. A lazy
  scheme that kept the pushes pending and applied them only when a drawing call
  needed the state was written, measured at 0.07 ms, and then **reverted** — see
  below.
- **Text is still 0.07 ms.** Confirmed a second time, with glyph counts.
- **Reading state back is 2.09 ms a frame.** This is the one real finding. A
  `BView` getter like `HighColor()`, `PenSize()`, `DrawingMode()`, `FillRule()`
  or `Transform()` is not a local field read: it is `FlushWithReply()`, a
  synchronous round trip to app_server. Each one measured about 28 µs, and a
  frame was making 75 of them. They also defeat the batching, because every one
  flushes the link.

Six of those call sites were reading back a value the port already knows, to
save and restore it around a drawing call, and were replaced with the value from
`GraphicsContext`'s own state: `drawLine`, `strokeRect` and `drawLinesForText`
now restore the pen with `strokeThickness()`, `fillRoundedRectImpl` and
`fillPath` compute the colour with `withGlobalAlpha(...)` and the drawing mode
with a new `drawingModeForCompositeHaiku()`, and `clipPath` restores the fill
rule from `fillRule()`. Rendering is unchanged: the scroll page and Wikipedia
both came back **0 pixels different**.

| Workstation | Scrolling | Speedometer 3.1 |
| --- | --- | --- |
| after the shadow and batching round | 44.94 fps | 3.23 ± 0.083 |
| + six read-backs removed | **46.33 fps** | 3.28 ± 0.058 |

Scrolling moved; Speedometer did not. The two confidence intervals overlap, so
the honest reading is that removing 0.2 ms of round trips per frame is not
measurable on a benchmark whose frames are 100–120 ms long. Both runs were on a
quiet machine (0.05 foreign cores, `vncserver` stopped).

That is 3%, which is what a 2.09 ms cost is worth against a 21 ms frame — and
most of it is still there. Of the 75 round trips, 65 are `Transform()` inside
`getCTM()` and `concatCTM()` and 10 are `GetClippingRegion()` inside
`clipBounds()`. `GetClippingRegion()` genuinely has to ask the server, because
app_server owns the clip; `Transform()` does not, and mirroring the CTM in the
port would remove the remaining 1.8 ms. It is left for the drawing-model work
rather than done here, because a mirrored CTM has to stay correct across
`PushState`/`PopState`, layers and `ImageBuffer` switches, and the payoff is
under two milliseconds.

**The lazy save/restore experiment was reverted.** Making `PushState()` lazy
rendered Wikipedia nearly blank, with truncated text; `SUMMIT_LAZY_STATE=0`
produced a pixel-identical image to the previous bundle, which both isolated the
cause and cleared the read-back removals. The likely reason is that
`beginTransparencyLayer()` needs its `PushState()` to have reached the view
before `BeginLayer()`, and there are probably other such orderings. Since the
whole mechanism was worth 0.07 ms, it was removed rather than fixed.

### What this round actually says

Per frame, painting is 14.6 ms inside the port's drawing calls. Text is 0.07 ms,
state changes are 0.07 ms, clips are 0.05 ms, images are 2.0 ms and round trips
are 2.1 ms. Everything else — about 10 ms — is inside 32 fill calls, spread
across gradients, rounded rectangles, shadows and image scaling, with no single
call worth removing. Three successive micro-optimisation hypotheses (glyphs,
state changes, read-backs) have now been tested; the first two were worth
nothing and the third was worth 3%. There is no fourth one that is worth more.

The remaining structural facts are unchanged and much larger: painting is
~95 ms per megapixel inside the web process, one thread does all of it, and 31
cores are idle while it does. The next step is the drawing model itself —
painting in parallel — not another pass over the drawing calls.

## Speedometer 3.1 against Firefox (September 21, 2026)

Workstation, local copy of the benchmark, 10 iterations, full-screen window:

| Browser | Score |
| --- | --- |
| Firefox 155.0 | **8.34 ± 0.37** |
| Summit, after removing the app_server read-backs | 3.28 ± 0.058 |
| Summit, after the shadow and batching work above | 3.23 ± 0.083 |
| Summit, before it | 3.05 ± 0.085 |

Firefox is 2.6x faster. A run is only comparable on a quiet machine: with
`vncserver` polling the frame buffer (about one core) the same build scored
1.94 ± 0.40, and `run.json` records the load so a contended run can be told
apart from a real change. The frame statistics say where Summit's time goes: per
frame about 40 ms of style, layout and script, about 40 ms of painting for only
0.3–0.5 Mpx of dirty area, and 9–30 ms of presentation, for a frame every
100–120 ms. Waiting for app_server is 0.04 ms, so painting is 95 ms per
megapixel *inside the web process*: the cost is building and issuing drawing
commands, not rasterising them. That, and the fact that all of it happens on one
thread while 31 others idle, is the next thing to attack.

Measured 2026-09-19 in Summit's Haiku test VM (docs/VM.md). Everything here can be
re-run with the scripts in `tools/bench/`; raw artifacts are under `.vm/bench/<run-id>/`.

## Current results (September 19, afternoon)

Uncontended runs (no compiler active in the VM; recorded per run in `run.json`),
local copy of Speedometer 3.1 unless noted, 10 iterations unless noted.

| Bundle | Changes | Score |
|---|---|---|
| bundle-la4ytsek | earlier engine, default settings | 0.575 ± 0.015 |
| bundle-la4ytsek | same with `JSC_useConcurrentJIT=0` | 2.30 ± 0.08 |
| bundle-ggdk304g | in-process timers, dirty-region painting, frame bitmap reuse; app set `JSC_useConcurrentJIT=0` | 2.42 ± 0.08 |
| bundle-ggdk304g | same, `JSC_useConcurrentJIT=1` (5 iterations) | **2.70 ± 0.11** |
| bundle-ggdk304g | same, `SUMMIT_FULL_REPAINT=1` (5 iterations) | 2.34 ± 0.08 |
| bundle-ggdk304g | **https://browserbench.org/Speedometer3.1/** (read from screenshot) | **2.35 ± 0.085** |
| bundle-fo_egzax | + frame drawing context kept, fill/text fixes (5 iterations) | 3.07 ± 0.28 |
| bundle-gewxjzbi | + **mimalloc** instead of Haiku `malloc`, software painting (5 iterations) | **3.53 ± 0.21** |
| bundle-gewxjzbi | same, GL compositing forced on llvmpipe (5 iterations) | 3.46 ± 0.31 |
| bundle-gewxjzbi | **https://browserbench.org/Speedometer3.1/**, default settings | **3.67 ± 0.099** |

Overall, from the start of the session: 0.575 → 3.53 locally (6.1x), and the
official page went from crashing to 3.67. mimalloc alone is +15%. GL
compositing on llvmpipe — CPU rasterisation — is level with software painting,
so it costs nothing to ship; a real GPU is expected to help the half of each
step that is rendering, but that is unmeasured.

What this shows:

1. The official site now runs to completion. It previously killed its page
   process through the Screen Wake Lock API, which is now reported as absent.
2. After timers stopped going through `BMessageRunner`, the concurrent-JIT
   "slow mode" described below disappeared: concurrent JIT is now 11% *faster*,
   so the app no longer disables it. Best configuration: 2.70, about 4.7x the
   la4ytsek default.
3. Painting only invalidated regions is worth about 3%. Half of each step is
   still "async" time (rendering update, paint, frame hand-off), so the frame
   path remains the largest target. The following bundle also keeps the frame's
   app_server drawing context alive (0.84 ms to 0.09 ms per frame in isolation)
   and is not measured yet.
4. The next full rebuild switches from Haiku's `malloc` to WebKit's bundled
   mimalloc (7.8x on a threaded allocation stress test) and adds optional GL
   compositing; see [gpu-compositing.md](gpu-compositing.md).

Reference: headless Chromium 151 on the VM's host CPU scores 28.4 ± 1.3 on the
same local copy (host, not the VM, so an upper bound for comparison only).

The analysis below was written earlier in the day, while builds were running,
and describes bundles j6jimkn8 and la4ytsek.

## Earlier results (contended)

**Every number in this section was taken while other engineers were compiling in
the same VM ("contended").** Treat absolute scores as lower bounds and trust only
the large effects (2x and more).

### Headline results (earlier)

| What | Bundle | Result |
|---|---|---|
| Speedometer 3.1, local copy, 10 iterations, default settings | bundle-la4ytsek | **0.537 ± 0.014** (contended) |
| Same, 10 iterations | bundle-j6jimkn8 | **2.27 ± 0.42** (contended; iteration 1 = 0.63, iterations 2-10 = 2.24-2.56) |
| Same with `JSC_useConcurrentJIT=0`, 3 iterations | bundle-la4ytsek | **2.22 ± 0.05** (contended) - 4.1x the default |
| Same with `JSC_useConcurrentJIT=0`, 10 iterations | bundle-j6jimkn8 | 1.96 ± 0.34 (contended; iteration 1 = 1.90 instead of 0.63) |
| Reference: headless Chromium 151 on the VM's host CPU, same local copy | - | 28.4 ± 1.3 |
| https://browserbench.org/Speedometer3.1/ | bundle-la4ytsek | **cannot run**: the page process exits when the benchmark requests a screen wake lock |
| Stress test, 10 min, 157 tab/navigation actions | bundle-la4ytsek | no crash, no hang, no load timeout |

The three findings that matter most:

1. **Concurrent JIT compilation puts the WebProcess into a slow mode** whenever a page's
   scripts are freshly loaded. One environment variable (`JSC_useConcurrentJIT=0`)
   removes it: 4.1x on the Speedometer score of the current bundle, 13x on
   TodoMVC-jQuery. The mechanism is not identified (no profiler could be used).
2. bundle-la4ytsek is **4.6x slower than bundle-j6jimkn8 from the second iteration on**
   in default settings, although both contain the byte-identical `libJavaScriptCore.so`.
   la4ytsek behaves in every iteration the way j6jimkn8 behaves only in its first
   (cold) iteration, and the way j6jimkn8 behaves in *all* iterations when the server
   forbids caching. See [The slow mode](#1-the-slow-mode-concurrent-jit--freshly-loaded-scripts).
3. Once the slow mode is out of the way, **60-95% of most suites is "async" time**
   (rendering update, paint, frame hand-off, zero-delay timer), about 20x the reference,
   and it is a fixed per-frame cost: it does not shrink with the viewport.

## Harness

| File | Purpose |
|---|---|
| `tools/bench/serve-speedometer.py` | Threaded static server (127.0.0.1) for the pristine checkout in `.cache/speedometer-3.1`; injects the result collector into the served `index.html`; receives results |
| `tools/bench/run-speedometer.py` | One benchmark run: load check, server, browser launch with a fresh profile, window sizing, contamination/crash/hang detection, screenshots, JSON, shutdown of its own instance, summary table. `--official` for browserbench.org |
| `tools/bench/run-probe.py` + `pages/probe.html` | Port-level diagnostic page: JIT tier check, allocation/DOM/layout kernels, timer and task latency, rAF cadence, the cost of Speedometer's "async window" |
| `tools/bench/stress-browser.py` | Seeded random tab/navigation stress with memory/thread sampling and crash/hang detection |
| `tools/bench/compare-runs.py` | Per-suite ratio table (sync/async split) between two results |
| `tools/bench/summitctl.cpp` | Native guest helper (built on demand in the guest): scripts **one Summit team by id** (`state`, `navigate`, `newtab`, `closetab`, `selecttab`, `frame`, `sidebar`, `quit`), lists a process group with full image paths, samples system and per-team CPU/threads/area memory |
| `tools/bench/guest.py` | Shared plumbing: ssh, launch in an own session/process group, single-launch guard, contention report, crash watch, display wake |
| `tools/bench/pages/wakelock.html` | Minimal reproduction of the page-process exit that blocks the official site |

Source of the benchmark: `git clone --depth 1 --branch release/3.1 https://github.com/WebKit/Speedometer .cache/speedometer-3.1`
(commit `1386415be8fef2f6b6bbdbe1828872471c5d802a`, 2025-03-11; the repository has a
`release/3.1` branch and no `v3.1` tag).

```sh
python3 tools/bench/run-speedometer.py                         # 10 iterations, default bundle (la4ytsek)
python3 tools/bench/run-speedometer.py --iterations 3          # quick check
python3 tools/bench/run-speedometer.py --wait-quiet 120        # poll up to 2 h for an idle VM first
python3 tools/bench/run-speedometer.py --bundle /SummitExtensions/summit/build-modern-browser/bundle-j6jimkn8
python3 tools/bench/run-speedometer.py --env JSC_useConcurrentJIT=0
python3 tools/bench/run-speedometer.py --official              # browserbench.org; score is read from final.png
python3 tools/bench/run-speedometer.py --annotate .vm/bench/<run-id> --score 4.2 --ci 0.1
python3 tools/bench/run-probe.py [--env NAME=VALUE]
python3 tools/bench/stress-browser.py --duration 600 [--seed N] [--local-only] [--max-tabs N]
python3 tools/bench/compare-runs.py .vm/bench/<run-A> .vm/bench/<run-B>
```

How results are collected without touching measured code: the server adds one inline
`<script type="module">` before `</head>` of the top-level `index.html` only. Module
scripts run in document order, so it runs after `resources/main.mjs` has created
`globalThis.benchmarkClient` and before `DOMContentLoaded` starts the run. It wraps the
client callbacks `willStartFirstIteration`, `didFinishLastIteration` and `handleError`
on that instance; nothing is installed inside the timed sync/async windows, no timers,
no observers. After the last iteration it POSTs the score, confidence interval, every
per-suite/per-test/per-iteration metric, `params` and the page environment to
`/__bench/result`. `--progress-beacons` (off by default) additionally reports each test
start and marks the result `instrumented`.

Details that turned out to matter:

- The server mirrors browserbench.org's response headers (ETag + Last-Modified, no
  Cache-Control, COOP same-origin, COEP require-corp). `Last-Modified` is the release
  commit date, not the clone's mtime, otherwise heuristic freshness would be seconds.
  `--cache-policy revalidate|no-store` exist for experiments.
- `http://10.0.2.2` is not a secure context, so COOP/COEP are ignored there:
  `crossOriginIsolated` is false and `performance.now()` has 1 ms resolution in the local
  runs. The same is the reason the local copy does not hit the wake-lock bug below.
- Do not send results with `fetch(..., {keepalive: true})`: keepalive bodies are capped
  at 64 KiB and a 10-iteration result is well over that (`result.json` is ~180 KiB
  indented); this lost the JSON of one run.
- The browser starts on `summit:home`, the window is set to `4,1,1270,797` and the sidebar
  is closed, then the URL is loaded: viewport 1267x666 (Speedometer asks for >= 850x650;
  the default window only gives 915x550).
- The guest screen saver blanks the display; the runner moves the pointer at the screen
  edge (no click, no key, outside the browser window) once a minute.
- **Summit is `B_SINGLE_LAUNCH` per executable file.** Launching `<bundle>/Summit` while
  that same file is already running only forwards the URL into the running instance as a
  new tab, whatever `--profile` says. Two of my runs were destroyed this way by a
  colleague's launch. The tools now refuse to start if `<bundle>/Summit` is already
  running, check once a minute that the window still has exactly the one expected tab,
  and mark a run `contaminated` otherwise. Instances from *other* bundle directories
  coexist and only appear as foreign CPU load.
- `ps` and `team_info.args` cut command lines at 64 bytes, which is shorter than the
  bundle path; roles are taken from the application image path instead.
- Contention: `summitctl sample` reads per-CPU active time and per-team CPU time over an
  interval. A run is labelled contended if a compiler/build tool is seen, other teams use
  more than 0.25 cores, or more than 0.5 cores of busy time cannot be attributed (short
  compiles that start and exit inside the interval).

## Test environment

- VM: QEMU/KVM, 12 vCPUs, 20 GiB RAM, host is a 32-core AMD Ryzen AI MAX+ 395;
  Haiku R1/beta6 hrev59866+79 x86_64; 1280x800 framebuffer, app_server software
  rendering, no GPU. `navigator.hardwareConcurrency` reports 8.
- Bundles (guest): `/SummitExtensions/summit/build-modern-browser/bundle-j6jimkn8`
  (built 2026-09-18 23:32 UTC, engine patch sha256 `2ff87cb1...`) and `bundle-la4ytsek`
  (2026-09-19 00:16 UTC, patch `87f133e1...`: opacity-layer paint fix, BFS disk-cache fix,
  extension APIs). WebKit upstream `00991b6cdc59`. `libJavaScriptCore.so.18.7.4` is
  byte-identical in both (sha256 `53596aab293e3a79...`, built 2026-09-14).
- Engine build (`/SummitExtensions/WebKit/WebKitBuild/Modern`): `CMAKE_BUILD_TYPE=Release`,
  `-O3 -DNDEBUG` (plus `-ftrack-macro-expansion=0 --param ggc-min-expand=10`), GCC 13.3,
  no LTO. `ENABLE_JIT=1`, `ENABLE_DFG_JIT=1`, `ENABLE_FTL_JIT=1`, `ENABLE_C_LOOP=0`,
  `ENABLE_WEBASSEMBLY=1` (BBQ and OMG on), `ENABLE_SAMPLING_PROFILER=0`,
  **`USE_SYSTEM_MALLOC=1`** (`Source/cmake/OptionsHaiku.cmake:90`; bmalloc/libpas off, so
  all of WTF/WebCore/DOM allocation goes to libroot's OpenBSD-derived malloc),
  `USE_ISO_MALLOC=1`, `USE_SKIA=0`, `USE_TEXTURE_MAPPER=0`, `USE_COORDINATED_GRAPHICS=0`,
  `ENABLE_GPU_PROCESS=0`, `ENABLE_ASYNC_SCROLLING=0`, `ENABLE_WEBGL=0`, `ENABLE_RELEASE_LOG=0`.
- The JIT tiers work at run time: the probe's integer kernel (20M iterations) takes 86-93 ms
  per round with the JIT and 620-660 ms with `JSC_useJIT=0`; headless Chromium on the host
  needs 140-160 ms for the same loop.

## Results

All local-copy runs: `?startAutomatically=true`, `measurementMethod=raf`, viewport 1267x666.
"Foreign cores" is CPU used by other people's processes in the VM during the run (median/max
of the 60 s samples).

| Run id (`.vm/bench/`) | Bundle | Settings | Iter. | Score ± 95% CI | Wall | Foreign cores | Label |
|---|---|---|---|---|---|---|---|
| speedometer-20260919-102837-la4ytsek-run1 | la4ytsek | default | 10 | **0.537 ± 0.014** | 602 s | 3.4 / 7.2 | contended |
| speedometer-20260919-105928-la4ytsek-noconcjit | la4ytsek | `JSC_useConcurrentJIT=0` | 3 | **2.218 ± 0.047** | 63 s | 2.5 / 2.5 | contended |
| speedometer-20260919-110826-j6jimkn8-run2 | j6jimkn8 | default | 10 | **2.274 ± 0.418** | 268 s | 3.3 / 5.5 | contended |
| speedometer-20260919-111325-j6jimkn8-noconcjit | j6jimkn8 | `JSC_useConcurrentJIT=0` | 10 | 1.964 ± 0.341 | 246 s | 4.3 / 7.4 | contended |
| speedometer-20260919-111902-j6jimkn8-nostore | j6jimkn8 | server sends `Cache-Control: no-store` | 3 | 0.538 ± 0.106 | 190 s | 2.8 / 5.9 | contended, diagnostic |
| speedometer-20260919-100209-baseline-a | j6jimkn8 | default | 10 | 2.03 ± 0.42 (from screenshot) | 357 s | 6.1 / 7.0 | contended, JSON lost (keepalive limit) |
| speedometer-20260919-095043-smoke2, -095305-smoke3 | j6jimkn8 | default | 1 | 0.570, 0.556 | 65 s | 5.1 | contended, harness bring-up |
| speedometer-20260919-110123-official-la4ytsek, -110408-...-retry | la4ytsek | browserbench.org | - | none | - | - | page process exited (see below) |
| reference-host-chromium-headless-* | Chromium 151 headless on the host | default | 10 | 28.37 ± 1.33 | ~2 min | - | host busy with the VM's builds |
| DISCARDED-contaminated-* (2), speedometer-20260919-101601-profiled | | | | | | | discarded: foreign tab injected / guest kernel panic |

Per-iteration scores: la4ytsek default `0.54 0.50 0.53 0.55 0.53 0.53 0.55 0.57 0.53 0.55`;
j6jimkn8 default `0.63 2.46 2.40 2.54 2.24 2.56 2.54 2.40 2.42 2.56`;
j6jimkn8 `JSC_useConcurrentJIT=0` `1.90 2.27 2.07 1.54 0.79 2.31 2.06 2.27 2.35 2.07` (the 0.79
coincides with 7 foreign cores); j6jimkn8 no-store `0.55 0.49 0.57`.

Per-suite means in ms (± 95% CI half-width where it fits). "sync" is time inside the
benchmark's rAF callback (script plus forced style/layout), "async" is from there to a
zero-delay timer after the frame (rendering update, paint, frame hand-off, timer).

| Suite | la4ytsek default | sync / async | la4ytsek no-concurrent-JIT | j6jimkn8 iterations 2-10 (median) | sync / async | Host Chromium | j6 steady / Chromium |
|---|---|---|---|---|---|---|---|
| TodoMVC-JavaScript-ES5 | 1251 ± 122 | 1088 / 163 | 265 | 263 | 87 / 170 | 33.8 | 7.8 |
| TodoMVC-JavaScript-ES6-Webpack-Complex-DOM | 1098 ± 89 | 850 / 248 | 348 | 331 | 76 / 254 | 33.0 | 10.0 |
| TodoMVC-WebComponents | 758 ± 50 | 539 / 219 | 240 | 236 | 35 / 199 | 20.3 | 11.6 |
| TodoMVC-React-Complex-DOM | 2634 ± 487 | 1719 / 915 | 912 | 378 | 125 / 243 | 32.7 | 11.6 |
| TodoMVC-React-Redux | 2075 ± 359 | 1423 / 652 | 426 | 292 | 136 / 155 | 31.0 | 9.4 |
| TodoMVC-Backbone | 1540 ± 116 | 1019 / 520 | 282 | 272 | 85 / 180 | 25.3 | 10.7 |
| TodoMVC-Angular-Complex-DOM | 3075 ± 530 | 1628 / 1446 | 641 | 404 | 170 / 233 | 30.8 | 13.1 |
| TodoMVC-Vue | 1417 ± 174 | 250 / 1167 | 274 | 261 | 25 / 237 | 24.5 | 10.7 |
| TodoMVC-jQuery | 7188 ± 821 | 7032 / 156 | 557 | 492 | 308 / 156 | 89.6 | 5.5 |
| TodoMVC-Preact-Complex-DOM | 980 ± 123 | 181 / 799 | 264 | 260 | 14 / 245 | 14.5 | 17.9 |
| TodoMVC-Svelte-Complex-DOM | 674 ± 67 | 170 / 505 | 274 | 260 | 13 / 242 | 12.9 | 20.1 |
| TodoMVC-Lit-Complex-DOM | 912 ± 65 | 191 / 721 | 307 | 298 | 15 / 285 | 16.8 | 17.7 |
| NewsSite-Next | 3570 ± 233 | 2290 / 1280 | 701 | 492 | 119 / 365 | 80.9 | 6.1 |
| NewsSite-Nuxt | 3007 ± 378 | 302 / 2705 | 364 | 381 | 73 / 268 | 63.5 | 6.0 |
| Editor-CodeMirror | 870 ± 59 | 562 / 308 | 279 | 248 | 96 / 151 | 25.9 | 9.6 |
| Editor-TipTap | 3144 ± 143 | 3048 / 96 | 2037 | **1988** | **1888** / 96 | 58.0 | **34.3** |
| Charts-observable-plot | 2390 ± 209 | 1971 / 418 | 559 | 498 | 95 / 408 | 44.3 | 11.2 |
| Charts-chartjs | 2715 ± 419 | 2016 / 699 | 515 | 410 | 205 / 200 | 73.4 | 5.6 |
| React-Stockcharts-SVG | 6330 ± 391 | 4690 / 1640 | 1178 | 997 | 212 / **768** | 77.9 | 12.8 |
| Perf-Dashboard | 1598 ± 185 | 1412 / 186 | 579 | 557 | 418 / 142 | 53.6 | 10.4 |
| **Geomean / score** | 1864 ms / 0.537 | 69% / 31% of total | 451 ms / 2.22 | ~400 ms / ~2.5 | | 35 ms / 28.4 | geomean 10.9 |

`python3 tools/bench/compare-runs.py <A> <B>` regenerates such tables, including the sync and
async ratios.

### Official site: the page process exits

`run-speedometer.py --official` never gets past the first second of the run on
bundle-la4ytsek (twice, reproducible): the tab's title falls back to the URL, the status
bar shows "The page process exited. Reload to try again.", `loadOutcome` is
`process-exited`. There is **no crash report** on the guest Desktop, **no debug_server
line** in `/var/log/syslog`, and `browser.log` is empty, so the process was terminated or
left deliberately rather than crashing (e.g. a failed IPC `MESSAGE_CHECK` in the UI
process, or the helper's own exit on connection loss).

Narrowing down (own instance, 10 s per page): the Speedometer home page, `about.html` and a
suite page opened directly all load; starting the benchmark with a single suite kills the
page. The first thing `BenchmarkRunner._runAllSuites()` does is
`navigator.wakeLock.request("screen")` (resources/benchmark-runner.mjs:324-334, 423),
which exists only in secure contexts. `tools/bench/pages/wakelock.html` opened as a
`file:` URL reproduces it without the benchmark: title goes `secure=true api=true` ->
`requesting` -> page process exited within ~2.5 s.

Where to look: `WebPageProxy::queryPermission` (Source/WebKit/UIProcess/WebPageProxy.cpp:15324,
"screen-wake-lock" at :15376, the default `API::UIClient::queryPermission` answers
`std::nullopt`, APIUIClient.h:242) and `WebPageProxy::didCreateSleepDisabler`
(WebPageProxy.cpp:19418, has a `MESSAGE_CHECK_BASE`); there is no Haiku `PAL::SleepDisabler`
(`Source/WebCore/PAL/pal/system/haiku/` only has `SoundHaiku.cpp`). I did not identify the
exact failing check. Any site that asks for a screen wake lock (video players, recipe
sites, benchmarks) will lose its tab the same way. Also note that `loadOutcome` reads
`process-exited` after a harmless COOP process swap too; only the status text tells the
two apart.

### Rendering correctness in the screenshots

Speedometer's own UI (suite name, progress counter, score, gauge) and the suites' content
painted with text in every screenshot I kept for both bundles, so the paint times above
are not flattered by missing glyphs on these pages. The lead reported that
`https://example.com/` rendered without any text glyphs in bundle-j6jimkn8 (only the link
underline); I did not capture that page in j6jimkn8. If pages inside CSS opacity layers
lost their text in j6jimkn8 (fixed in la4ytsek), j6jimkn8's async times for such content
are optimistic.

## Where the time goes

No sampling profiler could be used (see [Platform findings](#platform-findings)); the
analysis below uses Speedometer's sync/async split, the probe page, JSC/malloc
environment variables, a same-host reference browser and source reading.

### 1. The slow mode: concurrent JIT + freshly loaded scripts

Probe kernels on bundle-la4ytsek, one run each, all contended (2-8 foreign cores), so
read only the large differences. Times in ms.

| Kernel (tools/bench/pages/probe.html) | default | default #2 | `JSC_useConcurrentJIT=0` | `JSC_useJIT=0` | `JSC_useDFGJIT=0` | `JSC_useFTLJIT=0` | `JSC_useConcurrentGC=0` | `MALLOC_OPTIONS=jj` / `++` | host Chromium |
|---|---|---|---|---|---|---|---|---|---|
| integer loop, 20M iterations, steady state | 92 | 86 | 86 | 623 | 401 | 91 | 91 | 90 / 89 | 140 |
| 2M objects + closures + array methods | 1507 | **2658** | **321** | 744 | 487 | 679 | 2555 | 1536 / 1356 | 61 |
| 4M plain objects (GC heap only) | - | 89 | 118 | 238 | 149 | 99 | 90 | 87 / 93 | 28 |
| 2M `String(i)` (malloc-backed strings) | - | 298 | 134 | 161 | 226 | 192 | 549 | 565 / 540 | 30 |
| 200k detached DOM nodes | - | 524 | **149** | 506 | 197 | 228 | 677 | 616 / 585 | 49 |
| 20 x 2000 spans created + laid out | 970 | 862 | 776 | 1069 | 864 | 920 | 993 | 939 / 892 | 175 |
| layout only, 50 x 2000 spans | - | 453 | 493 | 492 | 465 | 458 | 486 | 540 / 497 | 41 |

- With the default settings, allocation- and call-heavy script is **slower with the JIT than
  with the interpreter** (2658 vs 744 ms) and 5-8x slower than with `JSC_useConcurrentJIT=0`.
  The optimising tiers themselves are fine (integer loop).
- Speedometer shows the same thing at full scale (table above): la4ytsek 0.537 -> 2.22.
- It is tied to **fresh script loads**. j6jimkn8 is slow only in iteration 1 (0.63) and then
  runs at ~2.5; with `JSC_useConcurrentJIT=0` its iteration 1 is 1.90. When my server sends
  `Cache-Control: no-store`, j6jimkn8 is slow in every iteration (0.55/0.49/0.57) - exactly
  la4ytsek's profile.
- la4ytsek is slow in **every** iteration although its requests.log shows every resource
  fetched once only (so iterations 2-10 are served from a cache) and although JavaScriptCore
  is byte-identical to j6jimkn8's. In j6jimkn8 the disk cache could not store large bodies
  (hard-link failure, below) and the repeat iterations came from WebCore's memory cache;
  la4ytsek has the disk cache repaired. My inference, **not verified**: in la4ytsek the
  repeat loads are now re-delivered by the NetworkProcess from the disk cache, which counts
  as a fresh script load and re-enters the slow mode each iteration. Whatever the path, a
  4.6x regression from iteration 2 on between the two bundles is real and reproducible.
- Not the cause, by experiment: junk-filling in libroot malloc (`MALLOC_OPTIONS=jj`), number
  of malloc pools (`++`), number of DFG compiler threads (`JSC_numberOfDFGCompilerThreads=1`),
  concurrent GC (`JSC_useConcurrentGC=0`).
- Candidates I could not separate without a profiler: (a) the compiler threads' malloc/mmap/
  munmap churn serialising the main thread on the per-team address-space lock or the malloc
  pool locks (the system allocator is in use, `OptionsHaiku.cmake:90`); (b) `WTF::Lock`'s
  `sched_yield` spinning against a compiler thread that was preempted by the builds in the VM;
  (c) the main thread staying in a low tier because concurrent plans are installed late.
  Re-running `run-probe.py` with and without the variable on a mimalloc build separates (a)
  from the rest.

### 2. Shape against a healthy engine on the same CPU

Reference: Chromium 151 headless shell on the VM's host, same local copy, 28.4 ± 1.3. It is
Blink/V8, not WebKit, and the host is not a VM, so only factor-of-several differences mean
anything. I did not use published Safari/WebKitGTK sub-scores: I could not fetch a citable
per-suite table, and a same-hardware run is the stricter comparison. (The host also has
WebKitGTK's MiniBrowser; running it would give a same-engine reference but needs a window on
the owner's desktop session, so I left it.)

- la4ytsek default is 52x the reference in geomean (30x-100x per suite), sync 61x, async 47x:
  a uniform, systemic slowdown - that is the slow mode.
- In the fast mode (j6jimkn8 iterations 2-10, or `JSC_useConcurrentJIT=0`) the gap is 10.9x
  in geomean and the shape becomes informative:
  - **async is 150-290 ms in every TodoMVC suite, against 6-19 ms in the reference**, and is
    60-95% of the suite for Vue, Preact, Svelte, Lit, WebComponents, Nuxt, observable-plot.
    That is ~80 ms per benchmark step, close to the probe's 70-75 ms for "replace 100 small
    nodes and paint".
  - **Editor-TipTap is the sync outlier: 1888 ms sync, 34x the reference**, unchanged by the
    JIT setting (2037 vs 1988 ms). TipTap/ProseMirror work on a contenteditable with
    selection and DOM-range geometry, so look at editor-state updates and at text
    measurement: `ComplexTextControllerHaiku.cpp:119` and `SimpleFontDataHaiku.cpp:128` call
    `BFont::GetEscapements`, which is a synchronous app_server round trip per call.
    Unmeasured; source reading only.
  - React-Stockcharts-SVG async 768 ms (SVG path painting), Perf-Dashboard sync 418 ms
    (canvas 2D drawing inside the callback), Charts-observable-plot async 408 ms: all
    painting through the app_server-backed graphics context.
  - TodoMVC-jQuery and the NewsSite/Chart.js suites are the *closest* to the reference
    (5-6x): plain DOM + script throughput is the least of the problems once the slow mode
    is gone.

### 3. Run loop, timers, rAF, frame path

Measured by the probe (la4ytsek and j6jimkn8 agree):

| Quantity | Summit | Host Chromium |
|---|---|---|
| nested `setTimeout(0)` interval (spec minimum 4 ms) | p50 8 ms, mean 9.4-9.7 ms, max 98-212 ms | 4.07 ms |
| MessageChannel task round trip | <= 1 ms (clock resolution is 1 ms) | 0.01 ms |
| rAF interval, nothing changing | 37-42 ms (24-27 fps) | 16.67 ms |
| Speedometer async window, nothing changed | 17.6-23 ms | 0.11 ms |
| same, one text node changed | 18-20 ms | 0.37 ms |
| same, 100 small nodes replaced | 70-76 ms | 1.09 ms |
| async window at 641x389 vs 1267x666 viewport (3.4x the pixels) | p50 23 ms vs 23 ms | - |

The last row is the important one: **the per-frame cost is fixed overhead, not pixel
work.** Source (bundle-era code; the host copy of `DrawingAreaHaiku.cpp` was being
rewritten during this session, line numbers are the host copy at 2026-09-19 11:30):

- Timers. In the bundles measured, every `RunLoop::TimerBase::start()` created a
  `BMessageRunner`, i.e. a synchronous request to the registrar, and deleted it again on
  fire; `MainThreadSharedTimer::setFireInterval()` did the same plus a second synchronous
  `SetInterval()` and leaked the `new BMessage('shrt')` on every arm. That is the extra
  ~4 ms per `setTimeout(0)`. The registrar's old 50 ms floor is not the issue
  (`kMinimalTimeInterval` is 1 us since 2020). The lead has since replaced both with an
  in-process scheduler thread (host copy `RunLoopHaiku.cpp:96-114`); not in either bundle.
- Frame pacing. `DrawingAreaHaiku::scheduleDisplay()` arms a fixed `16_ms` one-shot
  (`DrawingAreaHaiku.cpp:226`) only after the UI process has acknowledged the previous
  frame, so the period is 16 ms + update + paint + presentation round trip = the measured
  ~40 ms. This costs wall-clock time and smoothness, not Speedometer score (waiting for
  the frame is outside the timed windows).
- Painting. Bundle code: `display()` allocated a new full-viewport `ShareableBitmap` per
  frame and painted `page->bounds()` whatever was dirty; `setNeedsDisplayInRect()` dropped
  its rectangle. The host copy now keeps `m_frameBitmap`, unites a dirty region and skips
  unchanged frames (`DrawingAreaHaiku.cpp:259-325`) - not in either bundle, not measured.
  Still per frame in the host copy: `bitmap->createGraphicsContext()` (`:311`) builds a
  `BBitmap` with `B_BITMAP_ACCEPTS_VIEWS` over the shared area plus a `BView`
  (`ShareableBitmapHaiku.cpp:91-96`), i.e. an offscreen window with its own app_server
  thread, ports and a cloned area, and tears it down with a `Sync()` (`:70`); then
  `createReadOnlyHandle()` + `UpdateBitmapHaiku` (`:327-329`).
- UI process, per frame: `DrawingAreaProxyHaiku::updateBitmapHaiku()` maps the handle
  (`DrawingAreaProxyHaiku.cpp:58`); `BitmapPresenterHaiku::publish()` allocates a new
  `BBitmap` and `memcpy`s the whole frame (`BitmapPresenterHaiku.h:39,43`);
  `viewFrameHaiku` -> `Invalidate()` of the whole view (`WebKitView.cpp:384-385`);
  `BWebKitView::Draw()` fills the update rect white, draws the whole bitmap and calls
  `Sync()` (`WebKitView.cpp:287-300`). None of this is WebProcess CPU, but the WebProcess
  cannot start the next frame until it is acknowledged.
- IPC (`ConnectionHaiku.cpp`, `SocketMonitorHaiku.cpp`): SOCK_SEQPACKET with one poll thread
  per connection that hands each readable event to the work queue and waits on a pipe for
  the callback; up to 64 packets per wakeup. No sleeps or busy yields in the send path; the
  only delay is a `dispatchAfter(1_ms)` retry when the socket buffer is full
  (`ConnectionHaiku.cpp:188`). Not a Speedometer factor.
- During a run the WebProcess uses 0.94 cores and app_server 0.06 (5 s sample), so the
  benchmark is bound by one WebProcess thread, not by app_server or the registrar.

### 4. "Failed to create hard link ... NetworkCache"

`BlobStorage::add()` (Source/WebKit/NetworkProcess/cache/NetworkCacheBlobStorage.cpp) stores a
body under `Blobs/<sha1>` and hard-links it into the record; BFS has no hard links. In
bundle-j6jimkn8 that means: every response body above `maximumInlineBodySize()`
(NetworkCacheStorage.cpp:62, :1035-1037) is written to disk, the link fails, the record
never gets its body, and the response is fetched again next time, while the orphan blob
waits for the next cache sweep. Cost observed: 93 log lines for one Speedometer iteration,
174 for ten; in the 10-iteration run the nine Perf-Dashboard JSON files (8 KiB-595 KiB)
were downloaded 10 times each, in la4ytsek once. It is wasted disk writes and network
traffic outside the timed windows, so it does not move the score.
The fix (`#if OS(HAIKU)`: `data.mapToFile(path)` directly into the record) is in the guest
and host source and in bundle-la4ytsek: 0 such lines in its logs, including 10 minutes of
stress browsing. Side effect to keep in mind: with a working disk cache la4ytsek exposes
the slow mode in every iteration (section 1).

## Ranked speedup candidates

Expected impact is on the local Speedometer 3.1 score unless stated otherwise.

1. **Disable concurrent JIT until its cost on Haiku is understood.** Measured: 0.537 -> 2.22
   (4.1x) on la4ytsek; j6jimkn8's cold iteration 0.63 -> 1.90. Zero-risk form:
   `export JSC_useConcurrentJIT=0` in the bundle's `run-browser.sh` (helpers inherit the
   environment: `execve(..., environ)` in `UIProcess/Launcher/haiku/HaikuProcess.h:85`).
   Engine form: `Options::useConcurrentJIT() = false` for `OS(HAIKU)` next to the existing
   assignments in `Source/JavaScriptCore/runtime/Options.cpp` (option declared at
   `OptionsList.h:289`). Re-measure after mimalloc lands; if the gap is gone, turn it back on.
2. **Find out why la4ytsek re-enters the cold path every iteration** (section 1). If the
   memory cache is being bypassed for resources the disk cache can serve, fixing that gives
   ordinary page reloads the same 4x even with concurrent JIT on. Check with
   `run-speedometer.py --iterations 3` per-iteration scores: healthy is slow-fast-fast.
3. **Per-frame fixed cost in the WebProcess (~18-23 ms per frame, ~80 ms per Speedometer
   step, 60-95% of most suites in fast mode).**
   a. Ship the dirty-region/unchanged-frame work already in the host copy.
   b. Keep the graphics context alive with the frame bitmap: store the
      `std::unique_ptr<GraphicsContext>` next to `m_frameBitmap`, create it only when the
      bitmap is (re)created, wrap each frame in a `GraphicsContextStateSaver`, and replace the
      destroy-to-synchronise at `DrawingAreaHaiku.cpp:321` with
      `context->platformContext()->Sync()`. Saves one offscreen window + BView + area clone +
      teardown per frame. Low risk: single thread, same lock held as today.
   c. Send the dirty rects with the frame: add `Vector<WebCore::IntRect> dirtyRects` to
      `UpdateBitmapHaiku` (`DrawingAreaProxy.messages.in:30`). UI side: keep one `BBitmap` per
      geometry in `BitmapPresenterHaiku` instead of `make_unique<BBitmap>` per frame
      (`BitmapPresenterHaiku.h:39`), copy only the dirty rows/rects (`:43`), `Invalidate(rect)`
      instead of `Invalidate()` (`WebKitView.cpp:385`), and in `Draw(update)` drop the white
      `FillRect` when a frame covers `update` and draw `DrawBitmap(image, update, update)`
      (`WebKitView.cpp:294-297`). The reader/writer race on the persistent bitmap is already
      excluded by the present acknowledgement: the WebProcess does not paint while
      `m_pendingFrame` is set and the UI only reads inside `publish()`.
   d. Cache the mapping of the shared frame in the UI process (map once per
      `m_frameBitmap`, e.g. keyed by a bitmap generation number in the message) instead of
      `ShareableBitmap::create(handle)` per frame (`DrawingAreaProxyHaiku.cpp:58`).
   Expected: no-change frames to ~0 ms and small updates to a few ms; on the fast-mode
   numbers that is up to ~2x on the score, more on Svelte/Preact/Lit/Vue.
4. **mimalloc instead of libroot malloc** (in progress by the lead; `OptionsHaiku.cmake:90`).
   Probe kernels that isolate malloc are 3-12x the reference even in fast mode
   (strings 134 vs 30 ms, detached DOM 149 vs 49 ms, create+layout 776 vs 175 ms).
5. **In-process timers instead of BMessageRunner** (done by the lead, not in a bundle yet).
   Removes ~4 ms from every zero-delay timer; each Speedometer step's async window ends with
   one, so roughly 58 x 4 ms = 0.23 s of ~11 s per iteration in fast mode (~2%), more for
   timer-heavy pages.
6. **Text measurement and drawing through app_server** (`GetEscapements` per complex text run
   and per new glyph; every fill, string and path is a message to an app_server window
   thread). Editor-TipTap (34x) and the SVG/canvas suites point here. Needs measurement once
   profiling is possible; the GL compositing path behind the build flag or an in-process
   rasteriser is the structural fix.
7. **Frame pacing**: arm the display timer for `max(0, 16 ms - time since the last frame
   started)` instead of a fixed 16 ms after the acknowledgement (`DrawingAreaHaiku.cpp:226`).
   25 fps -> up to 60 fps rAF; no Speedometer effect.
8. **Memory, not speed**: Haiku's `madvise(MADV_DONTNEED)` is a no-op
   (`src/system/kernel/vm/vm.cpp`, "TODO: Implement!") while `MADV_FREE` works, so
   `OSAllocator::decommit()` / `hintMemoryNotNeededSoon()` (`OSAllocatorPOSIX.cpp:261-285`)
   never return JIT, Wasm or GC pages. Use `MADV_FREE` for `OS(HAIKU)`.
9. **Correctness blockers found on the way**: the wake-lock page-process exit (blocks
   browserbench.org entirely) and `loadOutcome` being `process-exited` after a COOP swap.

## Stress test

`stress-20260919-104136-la4ytsek`: bundle-la4ytsek, 600 s, seed 1, max 4 tabs, 12 internet
sites (Wikipedia, HN, BBC, Guardian, GitHub, MDN, haiku-os.org, Reddit, YouTube,
OpenStreetMap, example.com) plus 9 local pages from the Speedometer checkout; 6-8 foreign
cores busy throughout.

- 157 actions: 47 new tabs, 46 closed tabs, 29 navigations, 14 back/forward, 11 tab
  selections, 10 reloads. **No crash report, no debugger entry in syslog, no hang** (the UI
  answered every scripting request, slowest reply 2.3 ms), **no load timeout** (45 s limit;
  slowest loads: YouTube 26-43 s, BBC News 23-28 s), no load errors, clean quit on request.
  0 hard-link errors; browser.log is 399 bytes.
- Memory (sum of area `ram_size` per team, sampled every ~20 s):

  | Process | start | end (one idle tab, 45 s settle) | max | trend |
  |---|---|---|---|---|
  | Summit (UI) | 27.0 MiB | 38.2 MiB | 38.2 MiB | +0.4 MiB/min, 11 -> 20 threads |
  | NetworkProcess | 21.8 MiB | 80.7 MiB | 80.7 MiB | +3.6 MiB/min overall, flattening (56 MiB at 80 s, 75 at 430 s, 80 at 600 s), 10 -> 25 threads |
  | WebProcesses (sum) | 34.4 MiB | 502 MiB in 4 processes | 882 MiB in 6 | up to 393 MiB for one page |

- After closing all tabs but one, four WebProcesses were still alive (115, 298, 65 and
  24 MiB; the last is the prewarmed spare). That is consistent with WebKit's process and
  back/forward caches, but with `MADV_DONTNEED` being a no-op (candidate 8) nothing they
  decommit is returned either. Ten minutes are not enough to call any of the three growth
  curves a leak; a `--duration 3600 --local-only` run would settle NetworkProcess and UI.
- Known, by source: the bundles' `MainThreadSharedTimer::setFireInterval()` leaks one
  `BMessage` per timer arm (fixed by the lead's timer rewrite).

## Platform findings

- **Haiku's `profile` tool panicked the guest kernel.** I ran one 2-iteration Speedometer
  under `profile -k` (my addition; no such option exists in the tools any more). The
  benchmark finished, and when the profiled teams exited the kernel stopped with
  `PANIC: page fault, but interrupts were disabled. Touching address 0x20 from ip
  0xffffffff800c16fc`, backtrace `profiling_event(timer*) + 0x3c <- timer_interrupt` on an
  idle thread, hrev59866+79. Every process in the shared VM froze, including two other
  engineers' builds; the lead had to hard-reset the guest, and `/SummitExtensions` had to
  be mounted again by hand. Screens: `.vm/claude-kernel-panic-profiler-20260919.png`,
  `.vm/bench/speedometer-20260919-101601-profiled/guest-kernel-panic.png`.
  **Do not use `profile`, `scheduler_recorder` or kernel tracing in this VM.** That is also
  why this document has no function-level profile.
- Summit's single-launch behaviour and the 64-byte `team_info.args` limit (see Harness).
- `/SummitExtensions` does not auto-mount after a reset; the tools check for it and stop
  rather than write to the 95%-full boot volume.

## Noise and what the numbers can and cannot support

- I polled for a quiet VM with `--wait-quiet` from 11:18 (after the reset at 10:21 the
  builds restarted within minutes); 4-8 compiler processes were active at every sample. No
  uncontended number exists. The quiet-window attempt (`chain-3`/`chain-4` logs in
  `.vm/bench/`) was still waiting when this was written; if it completes, its run
  directories end in `-quietwait`.
- The benchmark is bound by one WebProcess thread and the VM has 12 vCPUs, so moderate
  foreign load costs little: two j6jimkn8 default runs an hour apart gave 2.03 ± 0.42 and
  2.27 ± 0.42, la4ytsek's ten iterations span 0.50-0.57. Single iterations do get hit
  (0.79 among 2.0-2.3 with 7 foreign cores).
- Run-to-run evidence is thin: one 10-iteration run per bundle and setting, 3 iterations
  for two of the diagnostic runs, one run per probe variant. The conclusions rest on
  effects of 3x and more that repeat across Speedometer and the probe; the smaller probe
  differences (for example `JSC_useFTLJIT=0` vs default on the DOM kernels) are within the
  noise and I draw nothing from them.
- The wide confidence interval of the j6jimkn8 runs is the cold first iteration, not
  measurement noise; without it the iterations span 2.24-2.56.

## Skia builds on Haiku (September 21, 2026)

The measurements above closed out micro-optimisation: three hypotheses tested,
two worth nothing and one worth 3%, with the remaining ~10 ms a frame spread
diffusely across 32 fill calls. What is left is structural -- one thread paints
at about 95 ms per megapixel while 31 cores idle -- so the next work is the
drawing model.

That work turns out to be mostly *adoption*, not invention. The pinned engine
tree already carries Skia (`Source/ThirdParty/skia`, 216 MB, fully vendored)
and `SkiaPaintingEngine`, which is upstream's parallel tile painter: a
`WTF::WorkerPool` sized to half the cores, tunable with
`WEBKIT_SKIA_CPU_PAINTING_THREADS`, plus a second pool for GPU painting. GTK and
WPE ship it. It is gated on `USE(COORDINATED_GRAPHICS) && USE(SKIA)`, but the
Windows port sets `USE_SKIA` with `USE_TEXTURE_MAPPER` and no coordinated
graphics, so the two can be adopted in stages.

Adopting it answers all three goals with one architecture rather than three:
tiles make a scroll a compositor translate instead of a repaint (which is also
exactly why GPU compositing measured four times *slower* -- `DrawingAreaHaiku`
invalidates the whole layer on scroll), TextureMapper composites through the
zink/NVK GL stack that already works on the GTX 1070, and painting moves off the
single web-process thread. It also removes the cost structure this round was
chipping at, since Skia rasterises in-process: no app_server IPC per drawing
call and no state round trips at all.

So the first step is the cheapest possible test of the riskiest assumption --
does Skia even build on Haiku?

**It does.** `libSkia.a`, 787 objects, from the vendored sources with one
one-line port fix. The build needs `freetype_devel`, `fontconfig_devel`,
`harfbuzz_devel`, `glib2_devel`, `expat_devel` and `libwebp_devel`, all of which
exist in HaikuPorts and had to be fetched with `curl` (see
[workstation](workstation.md) -- `pkgman` still cannot download on that machine).

The port fix: Skia's `include/private/SkFeatures.h` tests for `linux`,
`__FreeBSD__`, `__GLIBC__`, `__unix__` and friends, none of which Haiku defines,
so it fell through to its last resort and chose `SK_BUILD_FOR_MAC`. Three files
then asked for Grand Central Dispatch (`dispatch/dispatch.h`), `xlocale.h` and
`malloc/malloc.h`. The other 784 objects compiled unmodified. Everything Skia
wants on Haiku -- pthreads, mmap, dlopen, FreeType, fontconfig -- is its Unix
configuration, so `Source/ThirdParty/skia/CMakeLists.txt` now defines
`SK_BUILD_FOR_UNIX` for Haiku. It has to be a command line definition: several
sources include `SkFeatures.h` before the `SK_USER_CONFIG_HEADER` is read, so
putting it in `WebKitSkiaConfig.h` is too late, which the first build proved.

Build it with:

```
SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_REMOTE_TAG=ws \
SUMMIT_ENGINE_BUILD_NAME=SkiaSpike SUMMIT_ENGINE_CMAKE_EXTRA="-DUSE_SKIA=ON" \
tools/build-webkit-in-vm.sh --modern-extensions Skia
```

### What stage two needs

`WebCore`'s Skia support is wired as one `include(platform/Skia.cmake)`, the
same line GTK, WPE, Windows and PlayStation use; it brings 81 sources and the
private headers. The Haiku port would add that line, drop the roughly 25 files
in `platform/graphics/haiku` that Skia supersedes (`GraphicsContextHaiku`, the
`Font*` family, `Path`, `Gradient`, `NativeImage`, `Image`, the `ImageBuffer`
backends, the tile store), and keep the dozen that Skia has no opinion about
(the `IntRect`/`FloatRect` conversions, `IconHaiku`, `MediaPlayerPrivateHaiku`,
`PlatformDisplayHaiku`, `ShareableBitmapHaiku`, `SystemFontDatabaseHaiku`). The
new Haiku-specific code is the present path: get the pixels out of Skia's
surface and into the `BBitmap` that `DrawingAreaHaiku` already hands to the UI
process. That is the stage that has to be gated on a pixel comparison.

## Skia draws the page (September 21, 2026)

`USE_SKIA=ON` now builds the whole port: `libWebCore.a`, `libWebKit.so`,
`WebProcess`, `NetworkProcess` and a browser bundle, and it **renders**. The
scroll benchmark and a live Wikipedia article both come out correct -- text,
images, gradients, rounded rectangles, shadows, SVG icons and form controls.

The port keeps both renderers. `USE(HAIKU_GRAPHICS)` is the new name for "the
app_server backend": the port had been using `PLATFORM(HAIKU)` and `USE(HAIKU)`
to mean that, which stopped being true the moment Skia became selectable. 26
sites moved to the new macro; the ones that are about the platform rather than
the renderer -- the `BRect`/`BPoint` conversions, the event loop, the network
stack -- kept `USE(HAIKU)`.

What the switch involved, beyond the source lists:

- **The themes.** `ScrollbarThemeHaiku`, `ThemeHaiku` and `RenderThemeHaiku`
  paint with `BControlLook`, which needs a `BView` to draw into, and under Skia
  `platformContext()` is an `SkCanvas`. Under Skia the port now includes
  `platform/Adwaita.cmake` -- the theme GTK, WPE, Windows and PlayStation all
  use, painted entirely through `GraphicsContext`. The native Haiku look stays
  in the app_server configuration.
- **Fonts.** Skia finds fonts through fontconfig, which on Haiku already knows
  about `/boot/system/data/fonts`: `fc-list` reports 117 fonts and `fc-match
  sans-serif` resolves to Noto Sans. `USE_HARFBUZZ` is on, because Skia shapes
  text with `SkiaHarfBuzzFont`.
- **IPC.** `FontPlatformSerializedData` is a different struct under each
  renderer, so the WebKit layer includes `Platform/Skia.cmake` for the Skia
  coders instead of `WebCoreFontHaiku.serialization.in`.
- **GL.** `GraphicsContextSkia` and `SkiaGPUAtlas` call `GLContext`, `GLFence`
  and `BitmapTexture` even when rasterising on the CPU, and the port only
  compiled those with TextureMapper. They now build under Skia too, with
  `USE_EGL` set so they are not compiled away to nothing.
- **Images.** `IconHaiku` and `MediaPlayerPrivateHaiku` still get a `BBitmap`
  from Tracker and from the media decoder; both now wrap those pixels as an
  `SkImage` (`kUnpremul`, because `B_RGBA32` holds straight alpha). The video
  frame borrows the pixels rather than copying them, since the draw finishes
  before the call returns.
- **The present path needed nothing.** `DrawingAreaHaiku` already paints through
  `ShareableBitmap::createGraphicsContext()`, and the UI process copies raw
  pixels into a `BBitmap`. `ShareableBitmapSkia` wraps the same shared memory
  with `SkSurfaces::WrapPixels`, and Skia's N32 is BGRA on little-endian, like
  `B_RGBA32`.

### Scroll copying does not survive the change

| Scroll page, workstation | Scrolling | Rendering |
| --- | --- | --- |
| app_server backend | 44.4 fps | correct |
| Skia, scroll copy on | 41.0 fps | **bands of doubled text** |
| Skia, scroll copy off | 7.7 fps | correct |

Moving the pixels a scroll keeps on screen assumes the previous frame, shifted
by a whole number of device rows, is still what the page would paint. app_server
lays glyphs out on whole pixels, so it is. Skia positions them at subpixel
offsets, so it is not, and over a couple of hundred frames the difference
collects into bands where a line of text is drawn over the ghost of itself. The
same page on the same machine is clean under app_server, so this is about the
renderer and not about the benchmark.

Two fixes were tried and neither explained it, so both were removed rather than
left in as folklore: clearing the uncovered strip before repainting it changed
nothing at all (the affected rows are not in any repaint rectangle), and
widening every repaint rectangle by a pixel made the tag pills legible again but
left the text bands. The clip is not antialiased -- `GraphicsContextSkia::clip`
passes `false` -- so a blend at the boundary was not the mechanism either.

Scroll copying is therefore off by default when Skia draws (`SUMMIT_SCROLL_COPY=1`
turns it back on to look at the artefact). The honest cost is 41 fps to 7.7, and
the honest reading is that a full repaint of 1.8 megapixels through one thread is
what scrolling costs without it. That is the problem tiles solve: with tiled
compositing a scroll is a translate of already-painted tiles and nothing is
repainted or moved at all, which is stage three.

### Haiku's fontconfig has no conf.d, and that crashed the web process

Speedometer killed the web process every run, in
`FontCache::fontForPlatformData` -> `WTF::HashTable::validateKey`: an empty font
was being used as a hash key. The path is
`FontCache::lastResortFallbackFont()`, which asks for `serif`, and when that
does not resolve falls back to `SkTypeface::MakeEmpty()` -- a font whose
`FontPlatformData` is indistinguishable from the hash table's empty value.

`serif` did not resolve because Haiku's `fontconfig` package installs
`conf.avail` and no `conf.d`, so none of the standard configuration is active,
including `45-generic.conf` and `60-generic.conf`, which are what define the
`serif`, `sans-serif` and `monospace` aliases. `fc-match` hides this -- it
answers every query with a best effort -- but an in-process family match returns
nothing. Before: `fc-match serif`, `sans-serif` and `monospace` all answered
"Noto Sans". After linking the standard set into
`/boot/system/settings/fonts/conf.d`: Noto Serif, Noto Sans and Noto Sans Mono.

This is a property of the machine, not of Summit, and it affects any fontconfig
client on Haiku. It is written up in [workstation](workstation.md).

### Where Skia stands

| Workstation | Scrolling | Speedometer 3.1 |
| --- | --- | --- |
| app_server backend | 44.4 fps | 3.28 ± 0.058 |
| Skia | 7.7 fps | 2.59 ± 0.551 |
| Firefox 155 | -- | 8.34 ± 0.37 |

Skia is behind on both, and neither number is the point yet: this is the first
build, it paints on one thread, scroll copying is off, and none of the work that
made Skia worth adopting -- tiles, a worker pool, compositing through GL -- is
turned on. What the round establishes is that the engine's Skia port runs on
Haiku and draws real pages correctly, which is the thing stage three needs.

The Speedometer confidence interval is worth noting: ± 0.551 against ± 0.058 for
the app_server backend, on an equally quiet machine. Something in this
configuration is far less consistent run to run, and that is worth understanding
before reading much into the mean.

## Coordinated graphics: what it takes on Haiku (September 21, 2026)

Stage three is the one that matters for the cores: `SkiaPaintingEngine` paints
tiles on a `WTF::WorkerPool`, and it is compiled only with
`USE(COORDINATED_GRAPHICS) && USE(SKIA)`. The port's own comment said coordinated
graphics "must stay off ... its tile painting requires Skia or Cairo"; with Skia
that reason is gone, and the option now follows `USE_SKIA` whenever GL
compositing is built.

The configuration is `-DUSE_SKIA=ON -DUSE_HAIKU_GL_COMPOSITING=ON
-DENABLE_ASYNC_SCROLLING=ON`, built in its own directory (`SkiaCG`). What it took
to get WebCore through:

- **The portable render target.** `AcceleratedSurface` can back a frame with a
  DMA-BUF, a GL texture or a `ShareableBitmap`, and only the last is portable.
  Haiku selects `Type::SharedMemory`, which is the buffer the port already
  carries to the UI process. The surface's shared-memory paths were written
  `#if PLATFORM(GTK) || ENABLE(WPE_PLATFORM)`; Haiku is now in that condition,
  and the DMA-BUF includes that came with it are guarded by `USE(GBM)`.
- **Async scrolling and the scrolling thread.** The coordinated scrolling nodes
  only exist with `ENABLE(ASYNC_SCROLLING)`, as on GTK, and the tree they build
  runs on its own thread -- `PlatformEnableGlib.h` turns that on for every port
  that reaches coordinated graphics through GLib, and the same rule now applies
  to Haiku. The tree also asks wheel events which part of a gesture they are,
  which is `ENABLE(KINETIC_SCROLLING)`; the answers are inline in
  `PlatformWheelEvent.h`, so it costs the port nothing. That define has to be
  set before `PlatformEnable.h` reaches its own default, which is 0.
- **libepoxy.** The coordinated renderers include `<epoxy/egl.h>` directly. It
  is in HaikuPorts (`libepoxy_devel`). Haiku's build of it declares the Khronos
  enums itself instead of including `KHR/khrplatform.h`, so whichever of the two
  a translation unit sees first wins and the other conflicts; `GLContext.h` and
  the port's own two GL files now prefer epoxy when `USE(LIBEPOXY)` is set, as
  the rest of WebCore already did.
- **`RenderProcessInfo`.** Every coordinated renderer includes it, and it is a
  plain WTF struct that happens to live in `Shared/glib` beside the GLib
  argument coders. The port puts that directory on the search path; nothing else
  in there is picked up, because the rest is named for GLib.

WebCore builds and links in this configuration, which is the half that contains
the tile painting. The UI process half is where the work remains.

### It builds, runs and loads pages

The coordinated configuration now links -- `libWebKit.so`, `WebProcess`,
`NetworkProcess` -- and a bundle built from it starts, opens its window, loads
`http://example.com`, sets the tab title from the page and reports "Ready". The
content area is blank, and the reason is known rather than mysterious: nothing
constructs the `AcceleratedBackingStore`. The web process paints a frame into a
shared memory buffer and publishes it; in the UI process no one is listening,
because `AcceleratedBackingStore::create` has no caller and `updateSurfaceID`
is never called. Wiring it to the Haiku view, and implementing `presentFrame`
on top of the `BBitmap` path the port already has, is the next piece.

What it took to get from "WebCore compiles" to that:

- **The right drawing area.** The port had been given
  `DrawingAreaCoordinatedGraphics.cpp`, which is what Windows and PlayStation
  build. GTK and WPE build `DrawingAreaCoordinatedGraphicsGLib.cpp`, and that is
  the one that matches this `LayerTreeHost` -- despite the name it contains no
  GLib at all, only its own `#include`.
- **The port's own compositor stands down.** `LayerTreeHostHaiku` is built on
  `GraphicsLayerTextureMapper`, and coordinated graphics supplies
  `GraphicsLayerCoordinated` instead; only one of the two may define
  `GraphicsLayer::create`. `DrawingAreaHaiku` and `LayerTreeHostHaiku` are
  therefore not compiled in this configuration, and `DrawingArea::create`
  returns the coordinated one.
- **The DMA-BUF render target stays behind.** Haiku had been put in the same
  condition as GTK and WPE, which brought the texture target that exports the
  framebuffer as a DMA-BUF along with it. That one is now guarded to the
  platforms that have one; Haiku keeps only the shared memory target.
- **Smaller platform lists that predate a third coordinated port:** the
  `WebDisplayRefreshMonitor` override needs `HAVE(DISPLAY_LINK)` and without it
  the base returns nullptr and WebCore drives rendering from a timer;
  `dispatchAfterEnsuringDrawing` and its pending-callback partner are pure for
  GTK and WPE and now have defaults for the Haiku drawing area, whose own
  message for them is not declared either; and `ScrollbarsController::create` is
  defined both by the base and by the generic controller that
  `ScrollbarsControllerCoordinated` derives from, so the base stands down for
  this configuration.

### Coordinated graphics renders

The whole pipeline runs and the page appears:

```
web process display created      EGL, through the private Mesa's zink on NVK
renderer layer tree surface 1    the coordinated layer tree, not the fallback
layer tree updateRendering       the rendering update reaches the layer tree
compositor renderLayerTree       the compositor thread renders it
willRenderFrame 1276x711         the surface hands it a render target
buffer 1, buffer 2               shared memory buffers, published to the UI process
frame 1 bitmap=1                 a frame arrives and is presented
```

`AcceleratedSurface`'s counterpart in the UI process is
`AcceleratedBackingStore`, which GTK and WPE each implement. Haiku's
(`Source/WebKit/UIProcess/haiku/AcceleratedBackingStore.cpp`) keeps the shared
memory buffers by identifier, presents the committed one with
`presentBitmapHaiku` -- the hook the software drawing area already uses, so the
pixels reach the `BView` the way they always did -- and answers `FrameDone` so
the web process may paint again. The DMA-BUF message is guarded by `USE(GBM)`
so the generated receiver does not ask Haiku for Linux buffer types.

Three more things had to be true, and each was silently absent:

- **The view has to own one.** It is created when the drawing area reports a
  surface, through the page client's `enterAcceleratedCompositingMode`.
- **Accelerated compositing has to be on.** The port turned it off in the UI
  process, from when GL compositing was opt-in and slower. With it off the
  coordinated drawing area quietly chooses its non-composited renderer, which
  paints the whole page on one thread: no tiles, and so no `SkiaPaintingEngine`
  and no worker pool. That one line was holding up the point of the exercise.
- **The web process needs a shared `PlatformDisplay`.** The coordinated drawing
  area asks for one as soon as a page is created, and `sharedDisplay()` asserts
  rather than making one; `LayerTreeHostHaiku` used to create it on the way into
  GL compositing and is not built any more. The GLib and PlayStation ports set
  theirs in `platformInitializeWebProcess`, and Haiku now does too. There is no
  software fallback: without EGL the web process dies on that assertion, and on
  this machine EGL comes from the private Mesa, so `LIBRARY_PATH` must include
  `/boot/home/summit-mesa/prefix/lib`. It is `LIBRARY_PATH` on Haiku, not
  `LD_LIBRARY_PATH`.

And the one that hid longest: **`RunLoopObserver` has to do something.**
`RunLoopObserver::schedule()` is an empty stub on any port that is neither CF
nor GLib, and `isScheduled()` answers false forever. The coordinated renderers
drive *every* rendering update through that observer, so the layer tree, the
compositor and the surface all started correctly and then waited on something
that could not fire. Haiku's implementation dispatches to the run loop, which
runs the callback on the next turn rather than as the loop goes idle -- close
enough, and it keeps the update off the caller's stack. (That first
implementation was not enough on its own -- see "Rendering updates run" below,
where it had to learn which run loop it was asked for.)

The worker pool is running: the web process has eight `SkiaCPUWorker` threads,
which is `SkiaPaintingEngine` painting tiles on half the cores.

## Rendering updates run (September 22, 2026)

The coordinated build painted two or three frames and then froze. Both of the
things the previous round left open turned out to be the same kind of mistake --
work ending up on a thread that cannot do it -- and fixing them is what turned
the configuration from "renders once" into a browser that scrolls at the
refresh rate.

### The rendering update was running on the compositor thread

The symptom was that `RenderingUpdateScheduler::scheduleRenderingUpdate()` was
called 642 times and returned early every time, because its fallback timer said
it was already scheduled; the timer itself never fired. Instrumenting it showed
the timer `isActive()` and overdue by a growing margin, which a `WebCore::Timer`
can only be when it is in a `ThreadTimers` heap that nothing services -- that
is, when it was started on a thread other than the one it belongs to. Logging
`isMainThread()` in `startTimer()` confirmed it: the first two updates came from
the main thread, the third from a thread named `ThreadedCompositor`, and from
then on the page was dead.

The backtrace named the path exactly:

    WebCore::Page::scheduleRenderingUpdateInternal()
    WebCore::Page::renderingUpdateCompleted()
    WebKit::WebPage::finalizeRenderingUpdate(...)
    WebKit::LayerTreeHost::updateRendering()
    WTF::LoopHandler::MessageReceived(BMessage*)     <- compositor's run loop

`LayerTreeHost::updateRendering()` is reached from a `RunLoopObserver`, and
`ThreadedCompositor::renderLayerTree()` -- which runs on the compositor thread
-- schedules its did-composite observer like this:

    m_didCompositeRunLoopObserver->schedule(&RunLoop::mainSingleton());

The run loop is an argument because the observer is armed from one thread and
has to fire on another. Haiku's implementation ignored it and used
`RunLoop::currentSingleton()`, so the callback ran on the compositor thread, and
with it `didComposite`, `updateRendering`, the whole rendering update and every
timer it starts. The fix is to honour the argument: `PlatformRunLoop` is
`RunLoop*` on Haiku, and `schedule()` targets the loop it is given.

Two details matter in that implementation:

- **The flag, not the timer, answers `isScheduled()`.** The timer can fire on
  the target thread before `dispatchAfter()` has even returned, so a `RefPtr`
  member is not a reliable record of whether the observer is pending. A bool
  under a lock is, and the callback checks it before running.
- **Only the owning thread may arm a timer.** `RunLoop::TimerBase::start()`
  takes the target `BLooper`'s lock, and the thread being armed holds that lock
  for as long as it is dispatching a message -- including while it waits on the
  thread doing the arming. Cross-thread scheduling therefore uses
  `RunLoop::dispatch()`, which needs no lock. The timer exists only to dodge
  `breakToAllowRenderingUpdate()`'s one-cycle dispatch suspension, and that
  suspension lifts by itself, so a dispatched observer is at worst one turn
  late.

### Image decodes finish on the main thread

`AsyncImageDecoder` hops back to `RunLoop::currentSingleton()` as captured when
the decode was requested. Once tiles are painted on a worker pool, a decode
requested from a paint completes on that worker, and the completion walks into
`CachedResource::setDecodedSize` -> `MemoryCache::singleton()`, which is
`RELEASE_ASSERT(isMainThread())`. The reply run loop is now the current one only
when the request came from the main thread, and `RunLoop::mainSingleton()`
otherwise.

### Buffers were never returned to the swap chain

With frames actually flowing, the web process started dying with "Failed to
create handle for shared memory buffer". `AcceleratedSurface::SwapChain` hands
out a render target per frame and only gets one back when the UI process sends
`AcceleratedSurface::ReleaseBuffer`. Haiku's `AcceleratedBackingStore` presented
the frame and answered `FrameDone`, but never released the buffer, so the chain
allocated a new `ShareableBitmap` -- two file descriptors -- for every frame
until the process ran out. (The upstream guard is an `ASSERT` on
`s_maximumBuffers`, which is nothing in a release build.) `presentBitmapHaiku`
copies the pixels into a `BBitmap` before it returns, so the buffer is free the
moment the frame call is done and is released right there.

### A dropped wake-up could suspend dispatch for good

`RunLoop::wakeUp()` posted `'loop'` to the handler and ignored the result. That
is the one message that must not be lost: `performWork()` is what lifts
`suspendFunctionDispatchForCurrentCycle()`, and nothing posts again to a queue
that is already non-empty, so a single dropped wake-up stops every
`RunLoop::dispatch()` in the process permanently. It now retries from the timer
thread, the same way a dropped timer notification already did. Retrying on the
calling thread would deadlock when the caller is the looper itself.

### Where the coordinated build stands

Measured with `tools/bench/run-probe.py --page scroll.html` on the workstation,
1913x935 viewport:

| Workstation | Scrolling | Speedometer 3.1 |
| --- | --- | --- |
| app_server backend | 44.4 fps | 3.28 ± 0.058 |
| Skia, single-threaded, no compositor | 7.7 fps | 2.59 ± 0.551 |
| Skia + coordinated graphics | **62.3 fps** | not yet (see below) |
| ... with async scrolling off | 59.3 fps | 6.41 on the one suite measured |
| Firefox 155 | -- | 8.34 ± 0.37 |

The scroll run is 600 frames at 62.3 fps, mean 16.05 ms, p95 17 ms, p99 18 ms,
longest 19 ms, **zero frames over 33 ms**. Idle is 65.6 fps. That is the frame
pacing the rendering-update timer asks for, held for the whole burst on a page
with 400 cards and an 84,000 px document -- 1.4x the app_server backend and 8x
the first Skia build, with the jitter gone.

`AsyncImageDecoder`'s fix is verified on the page that first showed the
problem: `en.wikipedia.org` loads, lays out and draws its text, links, infobox
and images with the web process intact.

### Speedometer does not finish yet

Speedometer 3.1 runs but stops partway -- reproducibly inside
`Editor-CodeMirror`, at the same step -- and the process then sits at 0% CPU.
The watchdog added for this says the rendering pipeline is not the thing that is
stuck:

    Summit stall: 3.4 s with no rendering update; waitingForRenderer=0
      scheduledWhileWaiting=0 frozen=0 suspended=0 observerScheduled=0 updating=0
      | state=Idle reasons=0 waitingForTiles=no renderTimerActive=no
        didCompositeFn=none pendingTiles=no suspended=0
    Summit stall: page | pageScheduled=no remainingSteps=0 unfulfilled=0
      visible=yes throttling=0 updateInterval=15 rafDocuments=0 | ...

Everything is idle and consistent: the compositor has nothing pending, no
document has a `requestAnimationFrame` callback waiting, and nothing has asked
for a rendering update. Reporting the main thread's dispatch queues and timer
heap from the same watchdog said the same thing -- `dispatchSuspended=0
hasSuspended=0 currentQueue=0 nextQueue=0`, DOM timer throttling off, and the
timer heap's next entry a legitimate second away.

That reading was wrong, and the page said so. With `--progress-beacons` the
benchmark reports each test as it starts and finishes, and a two-second
`setInterval` reports that the page's own event loop is still turning. At the
hang the test beacons stop *and so does the interval* -- and because beacons
travel through the network process, the same tick also goes into
`document.title`, which the harness reads over the browser's own control
channel. That title froze at `[tick 4]` and stayed there while the control
channel kept answering in 50 microseconds.

So the page is not waiting on its own JavaScript: **its DOM timers have
stopped**. That is consistent with the engine dump after all -- a timer heap
whose front is a second away, a shared timer that says it is armed, and nothing
firing -- and it makes the notification that drives
`MainThreadSharedTimer` the place to look. Note the asymmetry worth keeping in
mind: the watchdog's own `RunLoop::Timer` kept firing every second throughout,
so it is not that Haiku's timers stopped, it is that one particular timer's
notification went missing.

One of the two causes is known. The suite that hangs, `Editor-CodeMirror`, has
a scroll step, and it completes with the scrolling thread out of the picture:

| Editor-CodeMirror alone, 1 iteration | Result |
| --- | --- |
| Skia, no compositor (`bundle-xknrxz_v`) | completes, 3.030 |
| coordinated, async scrolling on | **hangs**, same step every time |
| coordinated, `SUMMIT_DISABLE_ASYNC_SCROLLING=1` | completes, **6.410** |

So where the coordinated build finishes, it is 2.1x the build it replaces. The
switch is new and Haiku-only: upstream's `WEBKIT_DISABLE_ASYNC_SCROLLING` is
behind `ENABLE(DEVELOPER_MODE)`, which release builds do not set.

It costs about 5% of scroll frame rate and none of the pacing -- 59.3 fps,
p99 19 ms, still no frame over 33 ms -- so it is a usable default until the
scrolling tree is fixed.

With it set, the full ten-iteration run still stops, at a different suite
(`TodoMVC-Lit-Complex-DOM`, and in other runs the last test of the first
iteration) with the benchmark's iframe blank. Where it lands varies, which is
the signature of a race rather than of a particular test.

### Which timer stops, and where it stops

The beacons narrowed it to "DOM timers stop"; two rounds of instrumentation in
`RunLoopHaiku.cpp` then said exactly which timer and on which side of the port.

The deadline thread keeps its own time, so it can still speak when a looper has
gone quiet. Asked what it was holding at the hang, the web process answered:

    Summit sch: deadlines=1 front=JSRunLoopTimer::Manager::PerVMData::Timer in 3655939 ms
    Summit reg: handler 0xc92fa73aa0 holds 1 timers: JSRunLoopTimer::Manager::PerVMData::Timer -3655.9s

One deadline, an hour out, and **`MainThreadSharedTimer::timer` is not there at
all** -- not overdue, not waiting, not registered. It was stopped and never
armed again.

That rules out the two explanations that looked most likely. It is not a dropped
`'tmrf'`: a recovery pass that re-posts notifications for timers that are
registered, overdue and unknown to the scheduler was added, and it never fired
once, because there was nothing registered to recover. And it is not the
rejected notifications the log is full of -- those are the ordinary kind, a
stale identifier superseded by a newer arming.

So the loss is above the port, in WebCore. `ThreadTimers::updateSharedTimer()`
is the only thing that arms the shared timer, and it stops it here:

    if (m_firingTimers || m_timerHeap.isEmpty()) {
        m_pendingSharedTimerFireTime = MonotonicTime { };
        protect(m_sharedTimer)->stop();
    } else { ...arm... }

and `TimerBase::setNextFireTime()` only calls `updateSharedTimer()` again when
the heap's *front* changes:

    if (wasFirstTimerInHeap || isFirstTimerInHeap)
        threadGlobalDataSingleton().threadTimers().updateSharedTimer();

Those two together are the trap. The earlier engine dump caught the heap in
exactly the fatal shape -- seven timers, front about a second away -- with the
shared timer stopped. No DOM timer can run, so nothing can change the front, so
nothing ever calls `updateSharedTimer()` again. The process is wedged by
construction, and every DOM timer in it is dead.


### The fire loop never finishes, so no timer ever fires again

Tagging the trace with the process id -- several processes write to the same
log, which had made one earlier reading ambiguous -- finishes the story. These
are the last things the *web* process's `ThreadTimers` did:

    Summit tt[242096]: stop with 61 queued, firing=1, front in -1 ms
    Summit tt[242096]: stop with 60 queued, firing=1, front in -1 ms
    Summit tt[242096]: stop with 59 queued, firing=1, front in -0 ms

and then nothing. No `fire loop done`. `sharedTimerFiredInternal()` never
reached its end, so the main thread is still inside `item->timer().fired()`.

That single fact explains everything seen so far, and it is worse than a stuck
page:

    m_firingTimers = true;
    while (...) { ... item->timer().fired(); ... }
    m_firingTimers = false;          // <- plain assignment, no RAII

While the loop is running, `updateSharedTimer()` takes its first branch --
`if (m_firingTimers || m_timerHeap.isEmpty())` -- and *stops* the shared timer,
which is safe only because the loop arms it again on the way out. If the loop
never gets out, the shared timer stays stopped with 59 timers queued, and
`TimerBase::setNextFireTime()` only calls `updateSharedTimer()` when the heap's
front changes, which nothing can now do. **Every DOM timer in the process is
dead, and stays dead even if the blocking call later returns.** That is why the
page's `setInterval` stops, why `document.title` freezes, and why the engine
looks perfectly idle and healthy while it does.

It also explains why the Haiku-side recovery never fired: the shared timer is
not "registered but unnotified", it is deliberately stopped. Nothing below
WebCore is broken.

So the remaining question is a much smaller one than it looked: **what does the
timer callback block on?** The process sits at 0% CPU, so it is a wait, not a
spin, and the coordinated-graphics work supplies the obvious suspects -- a
synchronous wait on the compositor such as
`CoordinatedSceneState::waitUntilPaintingComplete()`, a `sendSync` to the UI
process, or one of the scrolling-tree waits. A backtrace of the web process's
main thread at the hang answers it outright, and the hang reproduces in about
seven minutes:

    Debugger --cli --thread <main thread id>      # then: thread <id>, bt

with no other debugger attached. Note that the control channel keeps answering
in microseconds throughout, because that is the UI process; it says nothing
about the web process's main thread.

Worth fixing on its own account, whatever the blocking call turns out to be:
`m_firingTimers` deserves a `SetForScope`, so that a callback that exits the
loop by any route cannot leave the process with no timers at all.


Two measurement traps were fixed along the way, both in the harness rather than
the browser:

- `guest.wake_display()` built its kill command with `awk "{print $2}"` inside a
  single-quoted Python string, so the remote login shell ate `$2` and the screen
  blanker was never killed. A blanked screen stops app_server drawing, the UI
  process stops acknowledging frames, and the browser looks frozen -- which is
  exactly what the first three scroll runs recorded as "timeout".
- `run-probe.py` woke the display once, before the run. It now does so every
  30 seconds, because the blanker comes back on every idle period.

### Real Reddit scroll, 2026-09-22

An opt-in `SUMMIT_FRAME_STATS=1` build reported UI-process coordinated frame delivery over
one-second windows, including median, p95, maximum inter-frame time, and the
number of gaps over 33 ms. The synthetic 400-card scroll probe measured 62.0
frames/s in the UI process, matching the page's 62.02 frames/s. Its 60 fps
result does not predict real Reddit scrolling.

On `https://www.reddit.com/r/popular/`, VNC wheel input moved the feed in both
directions. In `.vm/bench/reddit-vnc-down-up-20260922/`, the downward pass fell
from 32.9 and 28.4 frames/s to 2.4 frames/s, with a maximum 897 ms gap. The
upward pass over previously seen content began at 56.4 frames/s, then fell to
3.7 and 2.8 frames/s, with a maximum 638 ms gap. Screenshots in that directory
confirm page movement. These measurements implicate work beyond loading new
posts; the source of the stalls still needs isolation.

The follow-up `SUMMIT_COMPOSITOR_TIMING=1` trace measured compositor preparation,
scene flush, painting, and frame send. In
`.vm/bench/reddit-compositor-trace-20260922/`, the compositor's median frame
took 2.0 ms and p95 took 3.4 ms. The 590–867 ms gaps occurred between frames,
so most of that lost time was not spent inside the compositing stages. The
trace also exposed an uncontrolled redraw loop: Reddit produced 350–363
coordinated frames/s while no user input was being sent. Haiku's shared-memory
target does not wait for vertical sync during `swapBuffers()`.

An experiment scheduled Haiku compositor renders at least 1/60 second after
the last render started. Two fresh `/r/popular/` wheel passes used a title-bar
focus click and confirmed feed movement without navigating away. The unpaced
bundle (`.vm/bench/reddit-unpaced-titlebar-20260922/`) rendered 250–350
frames/s in most windows. The paced bundle
(`.vm/bench/reddit-paced-titlebar-20260922/`) rendered mostly 56–61 frames/s
with about 16.8 ms median spacing. It still had two approximately 775 ms
inter-frame gaps, so pacing removes excess redraw work but does not yet make
real Reddit scrolling reliably smooth. The temporary compositor trace logged
every frame and was removed after this measurement.

The paced build also completed the controlled 400-card probe at 59.79
frames/s over 120 scroll frames, with p95 19 ms, maximum 20 ms, and no gap
over 33 ms (`.vm/bench/probe-20260922-225543-summit-paced-scroll/`).

Additional temporary timing narrows the remaining long gaps. With
`SUMMIT_MEDIA_CANCEL_TIMING=1`, one media cancellation took 128 ms, including
119 ms in `BSoundPlayer::Stop()`
(`.vm/bench/reddit-media-cancel-trace-20260922/`). It occurred near a 798 ms
frame gap, but other gaps had no matching cancellation. With
`SUMMIT_RENDER_UPDATE_TIMING=1`, `LayerTreeHost::updateRendering()` took up to
1042 ms, 593 ms, and 304 ms during a wheel pass. Almost all of that time was
inside `Page::updateRendering()`, while scene flush took less than 0.1 ms
(`.vm/bench/reddit-render-update-trace-20260922/`). This located the stall
inside the WebCore page update.

`SUMMIT_PAGE_UPDATE_TIMING=1` performed that split in
`.vm/bench/reddit-page-update-trace-20260922/`. A 390 ms page update spent
360 ms in initial layout. A later 731 ms update spent 436 ms before animation
frame callbacks and 262 ms in a second layout. Another 467 ms update spent
186 ms before callbacks, 213 ms in animation frame callbacks, and 55 ms in
final layout. Thus both layout and page callback work cause the remaining
stalls; the compositor frame itself is comparatively cheap. The media
cancellation next to these updates spent 118 ms in `BSoundPlayer::Stop()`.
Haiku's local SoundPlayer implementation shows that blocking `Stop()` sleeps
for the audio output latency after stopping, whereas `Stop(false)` skips that
sleep. Summit now uses the nonblocking form during teardown.

The same 12-second H.264/AAC file played for one second and then had its
`video.src` cleared in `media-cancel.html`. With blocking stop, that JavaScript
operation took 154 ms, of which 142 ms was the sound stop. With nonblocking
stop it took 15 ms (`.vm/bench/probe-20260922-231754-summit-blocking-cancel/`
and `.vm/bench/probe-20260922-231832-summit-nonblocking-cancel/`). The new
bundle still played the Reddit CMAF H.264 video to its 12.7-second `ended`
event without a media error
(`.vm/bench/probe-20260922-231448-summit-nonblocking-media/`). A further
Reddit feed trace still had a 727 ms frame gap attributable to long page
updates, so the media stop fix removes one independent pause but does not
resolve the heavy page layout and script work.

The 60 Hz pacing experiment reduced Speedometer 3.1 from a 5.17 ± 0.24
unpaced control rerun to 3.96 ± 0.21 (ten iterations each, uncontended).
Pacing compositor-only animation frames scored 4.29 ± 0.19. Waiting until a
page had made no rendering update request for a second scored 4.87 ± 0.14,
but Reddit still rendered 200–250 frames/s while idle because it kept making
WebCore updates. Those results are in `.vm/bench/speedometer-20260922-233001-baseline-rerun/`,
`.vm/bench/speedometer-20260922-231944-paced-media/`,
`.vm/bench/speedometer-20260922-232702-animation-only-pacing/`, and
`.vm/bench/speedometer-20260922-233638-idle-animation-pacing/`. The pacing
policy was reverted: it did not remove the measured long page-update stalls
and it made the benchmark slower. Avoid reintroducing a broad 60 Hz timer
without a different rendering hand-off design.

The one-off frame, compositor, page, rendering-update, and media-cancel timing
hooks were subsequently removed from the production path. Their captured
logs remain in the bench directories above.

Removing those probes did not restore the 5.17 control score. The unpaced
build with all probes scored 4.40 ± 0.40; removing the Page probe scored
4.12 ± 0.19; removing the remaining rendering probes scored 4.20 ± 0.35;
and removing the frame statistics hook scored 4.01 ± 0.13. Each was a
completed, uncontended 10-iteration local Speedometer run. The cause of the
slow warmup in these newer bundles remains open, separate from the rejected
pacing policy.

An older bundle produced just after HTTP media repair and before the frame
probe scored 5.18 ± 0.26 on a fresh control run
(`.vm/bench/speedometer-20260923-000923-http-media-control/`). A later bundle
with the frame probe scored 4.42 ± 0.38
(`.vm/bench/speedometer-20260923-000635-post-media-control/`), and the current
probe-free bundle scored 4.01 ± 0.13. The JavaScriptCore library hashes are
identical. Disabling concurrent JIT on the current bundle scored 3.69 ± 0.14
(`.vm/bench/speedometer-20260923-001215-no-concurrent-jit/`). A temporary
rebuild with the old blocking media stop scored 4.11 ± 0.25
(`.vm/bench/speedometer-20260923-001802-blocking-stop-control/`), ruling out
the nonblocking stop as the cause. The current code retains `Stop(false)` and
the verified media cancellation improvement. The JavaScriptCore binaries are
identical; the newer WebKit builds have different `.text` contents from the
older 5.18-point bundle, though a rebuild of the same current source produced
identical `.text`. The remaining Speedometer warmup difference needs
investigation at runtime as well as the build level. The restored nonblocking
bundle is `bundle-viu0mveh` in the workstation's latest-bundle manifest.

### Skia worker count on the 32-logical-core workstation

WebKit obtains Haiku's online processor count through `sysconf` and normally
uses half of it, capped at eight, for CPU tile painting. With the same
`bundle-viu0mveh`, Mesa prefix, 800×600 Speedometer viewport, fresh profile,
and ten iterations per run, the worker-count sweep was:

| CPU paint workers | Speedometer 3.1 score | 400-card scroll fps | Gaps over 33 ms |
| --- | ---: | ---: | ---: |
| 1 | not run | 56.91 | 27 |
| 2 | 5.19 ± 0.22; 5.14 ± 0.27 | 60.43 | 0 |
| 4 | 5.20 ± 0.24; 4.32 ± 0.40; 3.01 ± 0.12 | 62.32 | 0 |
| 8 | 2.85 ± 0.12 | 60.78 | 0 |

All runs were classified uncontended by the harness. The four-worker scores
varied substantially on the *same bundle*, so the slow-bundle explanation
above is incomplete. No Summit/WebKit process was left running between tests;
`sysinfo` showed normal CPU frequencies. Two workers gave both stable
Speedometer scores and a scroll result without long gaps. Haiku now caps its
default at two CPU paint workers while still scaling down on smaller machines
and honoring `WEBKIT_SKIA_CPU_PAINTING_THREADS` for experiments. The paths are
`.vm/bench/speedometer-20260923-00*skia-workers*/` and
`.vm/bench/probe-20260923-00*skia-workers*/`. This controlled page does not
reproduce Reddit's layout and animation callback stalls.

The fast and slow four-worker result files also show a runtime-wide change,
not just a first-iteration warmup difference. Every iteration total in the
3.01-point run was 1.6–2.0 times the corresponding total in the 5.20-point
run. React Stockcharts panning grew from 73 to 329 ms; its synchronous and
asynchronous portions grew 4.4 and 4.5 times respectively. CodeMirror's long
edit grew from 65 to 162 ms, with its synchronous portion growing 3.1 times.
The same bundle and worker count produced both results. A future controlled
run should capture process CPU and clock state during the test before
attributing this variation to a WebKit code change.

The rebuilt default bundle `bundle-kou9exyv` scored 5.11 ± 0.21 in an
uncontended 10-iteration run with no worker override
(`.vm/bench/speedometer-20260923-004746-skia-two-default/`). Its 400-card
scroll probe reached 61.21 fps, p95 18 ms, maximum 23 ms, and no gaps over
33 ms (`.vm/bench/probe-20260923-005006-summit-skia-two-default/`).

Against Firefox's 8.34 ± 0.37 run at the same 800×600 viewport, Summit's
editor suites are among the largest remaining gaps: TipTap took 375.5 versus
124.8 ms (3.01×) and CodeMirror 120.2 versus 51.5 ms (2.33×). Chart.js was
2.25× slower. The sum of Summit's suite means was 4525 ms per iteration, with
36.3% in Speedometer's asynchronous portion, versus Firefox's 3050 ms and
28.9%. These are per-suite timings from the two completed local-copy runs,
not a function-level profile; allocation, text, and layout costs still need
separate measurement before attributing the gaps.

The `summitctl scroll` synthetic wheel burst now visibly moves a real Reddit
feed. In `.vm/bench/scroll-20260923-011441-reddit-synthetic-wheel/`, 100 wheel
notches took `/r/popular/` from its initial top post to several later posts;
`before.png` and `final.png` preserve the positions. This run used the
production bundle while an engine compile was active, so it establishes input
delivery and page movement only. No per-frame timing was emitted. The
real-site harness now labels such a capture `captured-uninstrumented` instead
of reporting an empty frame-rate result as a completed measurement.

The native scroll test interface now reports burst completion, delivered wheel
messages, elapsed delivery time, and the final send status through
`summitctl state`. The real-site harness waits for that completion before its
final capture. This matters on a busy page because each wheel send can block
for up to two seconds; the requested interval alone cannot prove that every
notch was delivered. A failed or incomplete burst is reported separately from
a completed frame-rate measurement. The browser and controller compile checks
passed.

The bundled runtime check passed on `/r/popular/`: both allocator variants
delivered all 300 requested wheel notches in about 4.81 seconds, and the
before/after screenshots show different posts. The opt-in native view counter
is reset at burst start so a long quiet period before scrolling is not counted
as a scroll stall. In the system-allocator capture
(`.vm/bench/scroll-20260923-023428-reddit-system-reset/`), the longest gap
between delivered frames was 1725 ms, with seven gaps over 33 ms. The mimalloc
capture (`.vm/bench/scroll-20260923-023806-reddit-mimalloc-awake/`) had a
650 ms longest gap and nine gaps over 33 ms. Different live feed posts loaded
in the two runs, so this is evidence of remaining real-site jank, not a
controlled allocator win. The view counted frame deliveries, which can exceed
the display refresh rate and should not be read as visible presentation fps.

An intervening mimalloc capture produced black before/after screenshots after
Haiku's screen blanker reactivated. Its frame numbers are excluded. The
real-site harness now wakes the display during settling and rejects a black
capture instead of recording it as a completed measurement.

### Mimalloc allocator comparison

Two alternating, uncontended ten-iteration Speedometer 3.1 pairs used the
same Summit source and native view, the same Mesa prefix and 800×600 benchmark
viewport, and separately bundled `SkiaCG` and `SkiaCGMi` engines. The system
allocator scored 5.08 ± 0.24 and 5.28 ± 0.23; mimalloc scored 6.26 ± 0.28
and 6.31 ± 0.31. The mean score rose from 5.18 to 6.28, about 21%. Results
are in `.vm/bench/speedometer-20260923-023954-skia-two-system-control/`,
`.vm/bench/speedometer-20260923-024212-skia-two-mimalloc/`,
`.vm/bench/speedometer-20260923-024414-skia-two-system-repeat/`, and
`.vm/bench/speedometer-20260923-024630-skia-two-mimalloc-repeat/`. The
previous same-bundle 4-worker variation is why the allocator was tested in
alternating pairs.

The matched 600-frame static scroll probes reached 61.81 fps with system
malloc and 62.18 fps with mimalloc, with no interval over 33 ms in either
(`.vm/bench/probe-20260923-024839-summit-system-scroll-control/` and
`.vm/bench/probe-20260923-024927-summit-mimalloc-scroll/`). The mimalloc
bundle also played the Reddit H.264 CMAF video to its 12.7-second `ended`
event without a media error
(`.vm/bench/probe-20260923-025013-summit-mimalloc-media/`). Haiku now
defaults to mimalloc while retaining CMake overrides for allocator experiments.
The 6.28 mean is still below Firefox's measured 8.34; the remaining editor,
layout, and callback costs need further work.

A fresh `AllocatorDefaultsCheck` CMake configuration, without allocator
override flags, resolved `USE_MIMALLOC=ON` and `USE_SYSTEM_MALLOC=OFF`
(`.vm/configure-allocator-defaults.log`). The temporary configure directory
was removed after checking its cache. The updated mimalloc engine rebuilt and
linked cleanly.

With `SUMMIT_MEDIA_CODEC_TRACE=1`, its WebProcess identified the selected
decoder as `H.264 on the graphics card (NVDEC)` while the direct Reddit CMAF
file played to `ended`
(`.vm/bench/probe-20260923-025529-summit-mimalloc-browser-codec/`). A live
`/r/popular/` feed scroll logged 18 NVDEC selections and showed video imagery
in its screenshots
(`.vm/bench/scroll-20260923-025633-reddit-mimalloc-codec-feed/`). The same
scroll still had a 663 ms maximum delivered-frame gap and 13 gaps over 33 ms.
Several Media Kit messages said it could not detect HLS from a nonstandard
extension and MIME type. Those messages warrant investigation for feed videos
that fail despite working H.264 MP4 decoding; a still screenshot does not
prove complete playback of every feed video.

With mimalloc, forcing four Skia CPU paint workers scored 6.30 ± 0.31 in an
uncontended ten-iteration Speedometer run
(`.vm/bench/speedometer-20260923-030034-mimalloc-skia-workers-4/`). That is
effectively the same as the two-worker scores, so the lower worker count stays
the default for now.

An active-process Haiku sampling profile of a five-iteration CodeMirror and
TipTap run is saved at
`.vm/bench/speedometer-20260923-030521-mimalloc-profile-late/profile.txt`.
Its inclusive stacks include `Page::updateRendering` (8455 samples),
request-animation-frame callbacks (about 6350), layout (about 2800), a Skia
tile replay worker (2141), and a separate DFG JIT compile worker (5751).
These counts overlap along call stacks and are not a CPU-time partition; JIT
generated code is also not fully symbolized. They support profiling the page
update, script callback, and layout path before investing in Skia GPU tile
painting. An earlier system-wide profile started before WebProcess launched and
did not include it; its result is excluded.

An exclusive Haiku sampler run over the same editor suites was dominated by
unsymbolized JIT addresses, so it cannot attribute the remaining cost to a
specific engine function
(`.vm/bench/speedometer-20260923-031114-mimalloc-editor-exclusive/`).
Disabling FTL JIT scored 6.37 ± 0.31 in ten Speedometer iterations, within
the variation of the mimalloc baseline; FTL remains enabled. The JSC
SamplingProfiler flag emitted no report on WebProcess shutdown in this setup.

An opt-in wheel-route trace on live `/r/popular/` recorded all 100 synthetic
wheel events on WebKit's scrolling tree: `tree=1 sync=0 blockingDOM=0
route=scrolling`. Delivery completed in 1.604 seconds with no send error.
The before and after screenshots show different Reddit posts
(`.vm/bench/scroll-20260923-032157-reddit-wheel-route-native/`). That run did
not request view frame statistics. It establishes that these wheel events
reach the scrolling thread; it does not identify whether the long frame gaps
seen in earlier captures occur during composition, the native bitmap copy,
or view-message delivery. The next instrumented capture timestamps the view
message after bitmap publication and measures its queue delay separately.

The timestamped follow-up
(`.vm/bench/scroll-20260923-032824-reddit-queue-attribution/`) delivered all
300 wheel events. During the burst, the native-view counter saw 603 frame
messages at 111.58/s, an interval as long as 790.3 ms, and 11 intervals over
33 ms. The longest delay between bitmap publication and the view handler was
only 3.6 ms, with no queue delay over 33 ms. The idle sample likewise saw
133.9 messages/s while its maximum queue delay was 0.2 ms. The UI message
queue is therefore not causing the long gaps; Reddit is also driving the
compositor well above the display rate and spending CPU on frames that cannot
all be shown.

A same-bundle follow-up compared the optional 60 fps compositor limit with an
uncapped control. The uncapped capture delivered 175.22 frame messages/s idle
and 109.79/s during the wheel burst, with a 1037.5 ms worst burst interval
(`.vm/bench/scroll-20260923-033507-reddit-compositor-uncapped-control/`). The
60 fps capture delivered 45.34/s idle and 34.17/s during its burst, with a
649.2 ms worst interval
(`.vm/bench/scroll-20260923-033411-reddit-compositor-60/`). Live feed content
differed and the capped run had one 630.9 ms UI queue outlier, so the gap
change is directional rather than a controlled frame-latency result. The cap
does clearly coalesce redundant composition work. It scored 6.269 ± 0.301 in
an uncontended ten-iteration Speedometer run, matching the uncapped mimalloc
baseline (`.vm/bench/speedometer-20260923-033627-mimalloc-compositor-60/`).
Haiku now defaults to the screen's nominal refresh rate, or 60 fps when that
rate is unavailable. `SUMMIT_COMPOSITOR_MAX_FPS=0` disables the limit and a
value from 30 through 240 overrides it for diagnosis or high-refresh displays.

The suspected Reddit HLS failure is reproducible independently of the feed.
The current completed-file HTTP path downloads
`https://v.redd.it/u5pu7ad5rcdh1/HLSPlaylist.m3u8` to an extensionless file;
Media Kit rejects it with media error 4 at time zero and reports that it cannot
detect HLS without a standard extension or MIME type
(`.vm/bench/probe-20260923-034125-summit-reddit-hls-downloaded-control/`).
Passing the original HTTPS URL directly also returns error 4
(`.vm/bench/probe-20260923-034501-summit-reddit-hls-direct/`). An experimental
local mirror downloaded the master playlist, child playlists, and CMAF assets.
Giving Media Kit the mirrored file URL removed its format-detection warning,
but still produced no tracks and error 4
(`.vm/bench/probe-20260923-040418-summit-reddit-hls-file-url/`). That experiment
was removed because it did not restore playback. Reddit's direct CMAF MP4 path
continues to play to `ended` with the NVDEC H.264 decoder, and live feed pages
select that decoder; complete HLS support needs a demux path that can retain
the playlist URL or supply its video and audio streams together.

The clean default-paced bundle `bundle-0xc87hol` passed both regressions. Its
600-frame 400-card scroll probe ran at 58.83 fps, p95 18 ms, maximum 31 ms,
with no interval over 33 ms
(`.vm/bench/probe-20260923-041115-summit-default-paced-scroll/`). The direct
Reddit 720p CMAF video selected `H.264 on the graphics card (NVDEC)` and reached
its 12.7-second `ended` event without a media error
(`.vm/bench/probe-20260923-041020-summit-reddit-cmaf-default-paced/`).

The same bundle reproduced its Speedometer result after the compiler was
stopped: 6.293 ± 0.302, with at most 0.05 foreign CPU cores
(`.vm/bench/speedometer-20260923-051716-mimalloc-generic-control-fresh/`).
Increasing JSC's global worklist limit from three to eight and setting four
Baseline, DFG, and FTL compiler threads scored 6.085 ± 0.307
(`.vm/bench/speedometer-20260923-051911-mimalloc-jsc-workers-8/`). The wider
pool provides no gain on this workload and is not enabled by default.

### Focused inline text layout result

`tools/bench/pages/layout-text.html` forces 50 width-dependent layouts over
2,000 inline blocks. It holds the box geometry constant while comparing empty,
repeated-text, unique-text, simplified-text, and fixed-width cases. Uncontended
runs on the workstation measured:

| Case, milliseconds per layout | Summit | Firefox 155 |
| --- | ---: | ---: |
| empty, automatic width | 10.80 | 5.90 |
| repeated text, automatic width | 39.92 | 6.56 |
| unique text, automatic width | 40.52 | 6.54 |
| repeated text, `optimizeSpeed` | 40.10 | 6.62 |
| unique text, `optimizeSpeed` | 40.80 | 6.66 |
| empty, fixed width | 9.76 | 6.64 |
| repeated text, fixed width | 33.66 | 6.80 |
| unique text, fixed width | 34.24 | 6.90 |

The artifacts are
`.vm/bench/probe-20260923-052334-summit-layout-text-fixed-generic/` and
`.vm/bench/probe-20260923-052420-firefox-layout-text-fixed-firefox/`.
Repeated and unique strings are effectively identical, and disabling kerning
and ligatures through `text-rendering: optimizeSpeed` does not move the result.
Fixed widths remove about 6 ms from Summit but leave about 24 ms of text work
above its empty-box case. This rules out the sampled string-width cache and
advanced shaping features as the main cause; the remaining gap is in repeated
inline text layout and display-content construction.

The probe harness can now run Summit under Haiku's inclusive sampling profiler
with `--haiku-profile`. Its focused profile records 15,323 samples in layout,
14,941 in `LineLayout::layout`, and 14,711 in
`InlineFormattingContext::layout`; width measurement itself has only 54
samples. These are inclusive stacks, not a CPU-time partition. The profile is
in `.vm/bench/probe-20260923-052551-summit-layout-text-profile/profile.txt`.
An exclusive profiler attempt hung Haiku during profiler shutdown and required
a NanoKVM power cycle, so the harness deliberately exposes only the verified
inclusive mode.

### Workstation-specific compiler result

A separate engine build targeted the workstation's first-generation Zen CPU
with `-march=znver1 -mtune=znver1`. Its clean bundle
`bundle-osto12r7` scored 6.279 ± 0.292 over ten uncontended Speedometer
iterations
(`.vm/bench/speedometer-20260923-061249-mimalloc-znver1-native-mesa/`). This is
indistinguishable from the generic bundle's 6.293 ± 0.302, so the native flags
are not enabled in the production build.

The focused text-layout probe did move: repeated automatic-width text fell
from 39.92 to 36.60 ms per layout, and repeated fixed-width text fell from
33.66 to 31.72 ms
(`.vm/bench/probe-20260923-061455-summit-summit-layout-text-znver1-native/`).
Those 8% and 6% improvements confirm that generated code affects the isolated
layout loop, but they did not improve the complete browser benchmark.

### Reddit HLS playback restored

The Haiku media backend now handles Reddit's finite, unencrypted byte-range
CMAF form of HLS. It parses the master playlist, selects the highest-bandwidth
variant and its audio group, verifies that each child playlist is a finite
same-resource byte-range playlist, then gives the completed video and audio
MP4 resources to separate Media Kit readers. Unsupported encrypted, live,
discontinuous, or multi-resource playlists still take the normal format-error
path rather than being partially decoded.

On `bundle-enb94pi0`, the previously failing Reddit master playlist loaded
metadata in 756 ms and played its 720×1280 video through to the 12.7-second
`ended` event with no media error. Codec tracing selected
`H.264 on the graphics card (NVDEC)` for video and `AAC` for audio
(`.vm/bench/probe-20260923-061923-summit-summit-reddit-hls-cmaf/`). The direct
CMAF MP4 regression also played to `ended` with NVDEC
(`.vm/bench/probe-20260923-062025-summit-summit-reddit-cmaf-hls-regression/`).
The same bundle's controlled 400-card scroll test reached 59.03 fps over 600
frames, p95 18 ms, maximum 23 ms, and no frame over 33 ms
(`.vm/bench/probe-20260923-062113-summit-summit-hls-scroll-regression/`).

A live `/r/popular/` follow-up visually moved from the first post to later feed
content after all 300 paced wheel events. Its trace selected NVDEC repeatedly
and exercised the new HLS path with separate NVDEC H.264 and AAC tracks. The
page still delivered only 26.30 native-view frames/s during the burst, with a
713.1 ms worst interval, while UI queue delay remained below 0.3 ms
(`.vm/bench/scroll-20260923-063455-reddit-hls-current/`). This confirms that
Reddit media playback is restored in the live feed and again locates the
remaining scroll stalls above the native event and presentation queue.

### Fixed-width inline layout reuse experiment

`SUMMIT_FIXED_INLINE_LAYOUT_REUSE=1` now enables a guarded Haiku experiment
that retains an already-laid-out inline block's contents when its containing
block changes width but the inline block has a fixed logical width. Replaced
content, relative sizes, percentage padding, dirty boxes, and block-level
boxes are excluded. The switch remains off by default.

On the same generic bundle, repeated fixed-width text fell from 33.18 to 4.70
ms per forced layout, and unique text fell from 34.12 to 4.70 ms. Automatic
width cases were unchanged
(`.vm/bench/probe-20260923-062552-summit-summit-fixed-inline-reuse-control/`
and
`.vm/bench/probe-20260923-062638-summit-summit-fixed-inline-reuse-enabled/`).
The complete Speedometer result remained neutral at 6.303 ± 0.312
(`.vm/bench/speedometer-20260923-062736-mimalloc-fixed-inline-reuse/`).

The dedicated correctness probe compares reused boxes with freshly created
equivalents through seven widths in both LTR and RTL. It found zero geometry
mismatches among the 3,360 boxes eligible for reuse, including nested
percentage-width content, relative offsets, and percentage margins
(`.vm/bench/probe-20260923-063228-summit-fixed-inline-correctness-candidates/`).
It also reproduced 144 nested-width mismatches in percentage-padding boxes in
both the disabled and enabled runs; those boxes are excluded from reuse, and
the mismatch is an existing layout behavior rather than a result of this
experiment. With the switch enabled, the controlled scrolling probe ran at
59.10 fps, p95 18 ms, maximum 23 ms, and no frame above 33 ms
(`.vm/bench/probe-20260923-063308-summit-fixed-inline-reuse-scroll/`).

`SUMMIT_FIXED_INLINE_LAYOUT_TRACE=1` reports one-second coverage counters
without changing layout decisions. The counters separate eligible fixed-width
inline boxes from dirty, block-level, replaced, relative-sized, automatic-width,
percentage-padding, and excluded descendants. `run-scroll.py` stores both the
per-second samples and an aggregate in `run.json`; this makes a live Reddit
wheel pass the gate for deciding whether the guarded reuse path has enough
coverage to warrant further work.

That gate found no coverage on live Reddit. During the refined wheel pass,
24,195 descendants were visited in full child relayouts. Of those, 21,115
were text or another non-box renderer and all 3,080 boxes were already dirty;
3,076 carried their own layout bit, often together with child-layout bits.
Zero boxes reached the fixed-width eligibility checks
(`.vm/bench/scroll-20260923-094303-reddit-dirty-inline-reasons/`). Broadening
the shortcut would therefore skip real Reddit style or content invalidation,
so the experiment stays disabled and is not a Reddit optimization.

The scroll harness now cuts its measured log before taking the final VNC
screenshot and flushes the initial capture before starting the wheel burst.
The old order could attribute app_server capture time to scrolling; one run
recorded a 659.3 ms UI queue delay from that final capture. With capture work
isolated and the layout trace disabled, the current bundle delivered 39.56
native-view frames/s, had a 702.9 ms worst page interval, and kept UI queue
delay below 0.3 ms. The same run selected NVDEC H.264 repeatedly and resolved
Reddit's finite HLS video and audio
(`.vm/bench/scroll-20260923-095232-reddit-current-capture-isolated/`). The
remaining visible pauses are still page work rather than wheel delivery or
the native presentation queue.

### Cross-suite TipTap slowdown isolation

The current HLS-capable bundle `bundle-jiito6mn` scored **6.526 ± 0.324** in
ten uncontended iterations
(`.vm/bench/speedometer-20260923-065031-current-full-after-hls/`). TipTap took
512 ms cold and 252–286 ms thereafter. The same bundle running only TipTap took
535 ms cold and 128–139 ms thereafter
(`.vm/bench/speedometer-20260923-064333-tiptap-alone-control/`). Running either
half of the preceding default application suites before TipTap preserved its
fast steady state; combining both halves reproduced the slowdown. This is a
cumulative working-set effect rather than one conflicting suite.

Controlled diagnostics ruled out several plausible global causes:

- Recreating the benchmark iframe for every suite did not improve TipTap.
- Disabling WebKit's back-forward cache reduced the overall score by about
  3.5% and left TipTap slow.
- A forced full JSC collection after every frame load and a ten-second idle
  before every TipTap pass both left its steady time at 269 ms or more.
- Reversing all 20 suites moved TipTap near the front but left its warm passes
  at 259 ms or more.
- Raising the inline text breaking cache from 500 KB to 4 MB and raising the
  shared shaped-text cache from 3,000 to 12,000 entries did not help. The latter
  scored 6.281 ± 0.769 over five iterations with 267 ms as its best TipTap pass
  (`.vm/bench/speedometer-20260923-080501-shaped-text-cache-12000/`). Both
  changes were reverted.

Repeating TipTap immediately after its normal full-suite pass changes the
picture: the normal passes took 498, 268, and 267 ms, while the duplicate took
149, 141, and 142 ms
(`.vm/bench/speedometer-20260923-080822-full-immediate-tiptap-repeat-diagnostic/`).
An isolated duplicate remained fast after a deliberate ten-second delay at
149, 146, and 145 ms
(`.vm/bench/speedometer-20260923-084238-tiptap-ten-second-repeat-diagnostic/`).
The loss therefore requires intervening application work; it is not a simple
ten-second expiry or persistent machine state.

JavaScriptCore cache reuse is beneficial but is not the source of the extra
full-suite cost. TipTap alone with `JSC_useCodeCache=0` settled at 163–178 ms
instead of 128–139 ms
(`.vm/bench/speedometer-20260923-074203-tiptap-no-jsc-code-cache-diagnostic/`).
The complete benchmark with that cache disabled fell to 5.504 ± 0.285 and
TipTap settled at 312 ms or more
(`.vm/bench/speedometer-20260923-074724-full-no-jsc-code-cache-diagnostic/`).
Disabling shared Baseline JIT code likewise made the isolated suite modestly
slower. Retaining those caches is the correct direction.

Increasing the unlinked-code working set to 60 seconds, 64 MB, and 8,000
entries scored 6.300 ± 0.754 and left TipTap at 268 ms or more. Lowering DFG
tier-up thresholds to 200 scored 6.279 ± 0.719 and left it at 266 ms or more.
Disabling JSC collection entirely for a bounded three-iteration diagnostic
scored 5.862 ± 1.780 and left the warm TipTap passes at 279 ms or more
(`.vm/bench/speedometer-20260923-085005-full-no-jsc-gc-diagnostic/`). These
tests rule out unlinked-code cache capacity, late tier-up, and full collection
as the state transition. All diagnostic settings were reverted.

Request logs later confirmed that both the isolated and full benchmarks fetch
TipTap's HTML and JavaScript exactly once, excluding resource eviction or a
second network load. JSC compile-time logging also found 467 of the 468
isolated compile signatures in the full run. For that shared subset, Baseline
compilation was effectively identical: 298 functions and 21.45 ms isolated,
versus 299 functions and 21.17 ms in the full run. The full benchmark did,
however, emit roughly 34 MB of Baseline, DFG, and FTL code over three
iterations, compared with roughly 1.5 MB for isolated TipTap. Raising the DFG
threshold to 10,000 scored 6.179 and left TipTap at 288.8 ms; forcing
synchronous JIT compilation scored 4.321 and made TipTap slower at 381.7 ms.
Increasing the JSC compiler worker pool had already scored 6.085 and left
TipTap unchanged. The remaining evidence points to the accumulated hardware
code and data working set, rather than repeat compilation or contention with a
background compiler. There is no validated setting change to retain
(`.vm/bench/speedometer-20260923-161248-tiptap-compile-times/`,
`.vm/bench/speedometer-20260923-161336-full-compile-times/`,
`.vm/bench/speedometer-20260923-161655-full-dfg-threshold-10000/`, and
`.vm/bench/speedometer-20260923-161901-full-no-concurrent-jit-tiptap-diagnostic/`).

Archived inclusive profiles remain useful for locating broad costs. A
50-iteration TipTap-only profile shows
`Document::updateLayout` on 26.8% of all one-millisecond samples, flex layout
on about 22%, selection canonical-position work on about 13%, intrinsic width
work on about 10%, and DFG compilation on a separate worker on about 20%.
Painting was about 1%. A complete three-iteration profile independently puts
layout near 9%, style near 14%, JIT work near 19%, and Skia tile work near 8%.
The percentages are inclusive and overlap; they identify the remaining work
above painting without forming an exclusive CPU-time partition. Artifacts are
`.vm/bench/speedometer-20260923-063838-tiptap-profile/` and
`.vm/bench/speedometer-20260923-065529-full-profile-cross-suite/`.

Haiku's profiler shutdown later hung the workstation after a second complete
inclusive run, just as the earlier exclusive attempt did. No profile was
written for that run. Both benchmark harnesses now reject `--haiku-profile` on
the workstation; it remains available in the disposable VM.

### Cache Skia system typeface matches

The full profile also put `SkFontMgr_fontconfig::onMatchFamilyStyle` on 5.15%
of samples and fontconfig's pattern filtering on 4.53%. WebCore already caches
complete platform font data, but that key includes size and other properties.
Creating another size of the same family and style therefore repeated the
expensive fontconfig search.

The Skia font cache now retains the matched system `SkTypeface` by family,
weight, width, and slant. Font size, OpenType features, synthesis, orientation,
and metrics are still computed for every `FontPlatformData`, and normal font
cache invalidation clears the new map. This cache stores both successful and
failed matches so fallback family lists do not repeatedly query a missing
face.

The first ten-iteration result was 6.238 ± 0.229 with steady iterations from
6.28 to 6.38
(`.vm/bench/speedometer-20260923-090242-skia-typeface-match-cache/`). That is
neutral against the long-term generic control of 6.293 ± 0.302 and below the
unusually fast 6.526 run above. The cold TipTap pass fell from the usual
roughly 510 ms to 362 ms in this run, while steady full-suite passes remained
270–321 ms.

A recovered-host A-B-A comparison then produced 6.438 ± 0.240 with the cache,
6.243 ± 0.321 on the pre-cache HLS bundle, and 6.431 ± 0.220 after returning
to the cached bundle. The corresponding sums of suite means were 3,676,
3,804, and 3,682 ms per iteration
(`.vm/bench/speedometer-20260923-094406-inline-coverage-trace-off/`,
`.vm/bench/speedometer-20260923-094604-pre-font-cache-alternating-control/`,
and
`.vm/bench/speedometer-20260923-094800-post-font-cache-alternating-repeat/`).
This is a reproducible roughly 3% gain in the alternating comparison, while
the absolute score remains below Firefox 155's 8.338 ± 0.374 baseline.

### Live Reddit page-update phase trace

`SUMMIT_PAGE_UPDATE_TRACE=1` now records only rendering updates slower than
33 ms and splits them across each HTML rendering step. On Haiku it also emits
slow `Document::runScrollSteps()` dispatches, JavaScript scroll listeners, and
layouts so the nested work can be correlated. The disabled path reads the
environment switch once and does not read the clock. `run-scroll.py` stores
the page-update and scroll-step samples plus aggregate phase totals in
`run.json`.

The first capture-isolated `/r/popular/` pass found five slow updates totaling
1,741.5 ms. The scroll step accounted for 1,084.8 ms, ahead of second layout
at 204.2 ms, animation-frame callbacks at 186.6 ms, and initial layout at
185.5 ms
(`.vm/bench/scroll-20260923-100942-reddit-page-update-phases/`). A refined
repeat found that three slow scroll steps spent all 1,523.2 ms dispatching one
document scroll event each. Native scroll animation service, scroll anchoring,
target lookup, and visual-viewport delivery rounded to 0.0 ms. The individual
event dispatches took 455.9, 595.3, and 472.0 ms
(`.vm/bench/scroll-20260923-101608-reddit-scroll-step-phases/`).

A third pass traced the listener itself. Every slow dispatch invoked the same
single bubbling JavaScript listener; the slow calls took 64.1, 180.6, 270.9,
and 241.7 ms. Layouts nested in those calls accounted for only part of their
duration. The 270.9 ms call also emitted two failed Media Kit stream-cookie
allocations, so media creation remains a candidate for some of that handler's
non-layout time, but the log does not establish how much. The measured wheel
pass still moved the feed, delivered all 300 events, kept the native queue
below 0.4 ms, and reached 41.74 native-view frames/s with a 533.4 ms worst
interval (`.vm/bench/scroll-20260923-102211-reddit-listener-layout/`).

These runs move the primary Reddit stall from generic rendering or input
delivery to synchronous work initiated by Reddit's document scroll listener.
Skipping or delaying the standards event would change page behavior; the next
useful split is inside its JavaScript and synchronous media work.

`SUMMIT_MEDIA_LIFECYCLE_TRACE=1` now records media loads and cancellations
that block for at least 10 ms. Cancellation is divided into loader-thread join,
video-thread join, audio stop, and cleanup; the disabled path caches the
environment check and takes no clock readings. `run-scroll.py` stores the
individual samples and per-operation totals in `run.json`.
Set the value to `2` to record every call for a focused probe.

A correlated live pass found one 342.2 ms Reddit scroll listener that destroyed
a media player while its download was still active. The synchronous
`cancelLoad()` spent 51.2 ms waiting for the loader thread, and a 94.7 ms layout
also ran before the listener returned. The loader wait therefore explains a
measurable part of the pause, while roughly 196 ms remains in other page work.
The later unsupported stream-2 Media Kit diagnostic was outside this listener;
NVDEC H.264 and AAC initialization still succeeded. The complete pass reached
43.55 native-view frames/s, had a 633.2 ms worst interval, and kept native queue
delay below 0.9 ms
(`.vm/bench/scroll-20260923-125311-reddit-media-lifecycle/`).

The completed-file downloader now uses libcurl's multi interface. While a
download is active, `cancelLoad()` calls the thread-safe `curl_multi_wakeup()`
before joining the loader, with a lock protecting the multi handle's lifetime.
On the same direct Reddit CMAF file, clearing the source after 50 ms blocked
JavaScript for 31 ms in the old bundle; the lifecycle trace attributed all
30.9 ms to the loader join
(`.vm/bench/probe-20260923-130753-summit-media-active-cancel50-control/`).
The wakeable downloader returned in 2 ms, and the all-call lifecycle trace
confirmed that its active loader join took only 0.6 ms
(`.vm/bench/probe-20260923-131324-summit-media-active-cancel50-wakeup-all/`).

A live `/r/popular/` pass with the new downloader had no media lifecycle call
above the 10 ms threshold. It resolved Reddit HLS video and audio and selected
NVDEC H.264 and AAC successfully
(`.vm/bench/scroll-20260923-130308-reddit-wakeable-media-cancel/`). A separate
direct HLS probe played the complete 12.7-second clip to `ended` at 720 by
1280 with no media error, again using NVDEC H.264 and AAC
(`.vm/bench/probe-20260923-130915-summit-reddit-hls-wakeup/`).

The page-update trace now also divides each layout pass into style and
preparation, render-tree layout, view sizing, and post-layout work.
`run-scroll.py` stores the individual passes and aggregate phase totals. In a
live measured burst, 48 slow layout passes took 2,730.2 ms: render-tree layout
accounted for 2,698.9 ms (98.9%), while preparation took 8.9 ms, view sizing
0.4 ms, and post-layout work 20.4 ms. The worst 593.3 ms Reddit scroll
listener forced three full layouts of 78.3, 134.1, and 100.4 ms; about 300 ms
of their combined time was the render-tree walk. Style resolution is therefore
not the source of these forced-layout pauses
(`.vm/bench/scroll-20260923-132029-reddit-layout-phases/`).

The same opt-in trace now records dirty renderer layouts that take at least
10 ms. These timings are inclusive: a parent includes the children it lays
out, so their totals must not be added. A follow-up live pass found one stable
hot hierarchy across the burst: 47 slow `RenderFlexibleBox` calls contained
47 slow calls on the same `RenderGrid`, and that grid laid out the same
`RenderBlock` twice on nearly every pass (92 slow block calls). The flex and
grid calls each covered about 2.2 seconds of the 2.28 seconds spent in slow
render-tree passes. This narrows the next investigation to the grid's intrinsic
and final item sizing passes
(`.vm/bench/scroll-20260923-132721-reddit-renderer-layout/`).

Focused grid call-site timing confirms that both layouts of the hot block are
real work on the same grid/item pair. Of 88 slow child layouts, 46 intrinsic
row-sizing calls took 1,184.3 ms and 42 final placement calls took 765.5 ms.
All intrinsic calls were the first row-sizing iteration. The grid formatting
context rejected the hot grid because its template columns are outside the
currently supported subset, so it stayed on the legacy algorithm. The measured
burst reached 43.87 native-view frames/s, with a 601.9 ms worst interval and a
0.3 ms maximum native queue delay
(`.vm/bench/scroll-20260923-133824-reddit-grid-phases/`). The next change needs
to preserve the intrinsic and stretched sizes while avoiding one of these
full child layouts; their overlapping renderer totals cannot be treated as
independent savings.

The intrinsic-height cache cannot be reused for this case. On every hot-grid
invalidation the block had dirty normal-flow descendants, so its content height
could have changed. A second trace also classified the grid formatting context
rejection: the hot grid uses `minmax()` columns. An opt-in experiment admitted
that supported sizing primitive through the remaining coverage checks, which
then correctly rejected the same multi-column grid for
`justify-content: space-between`. The modern grid path does not yet apply those
between-track distribution offsets. The experiment therefore never selected
the modern path and was removed rather than weakening layout correctness
(`.vm/bench/scroll-20260923-134538-reddit-grid-invalidation/`,
`.vm/bench/scroll-20260923-135547-reddit-grid-minmax-justify/`, and
`.vm/bench/scroll-20260923-140008-reddit-grid-gfc-single-column/`). A useful
modern-grid optimization now requires implementing content-distribution
positions and corresponding layout coverage, rather than bypassing its guard.

The modern grid formatter now implements `space-between` track distribution
on both axes. It carries the start and between-track offsets with the used
track sizes, includes distributed gutters in grid-area sizes, and applies the
same offsets to final item positions. Haiku's additional coverage admits the
exact features observed on Reddit: `minmax()` columns, horizontal and vertical
`space-between`, percentage grid-item widths, and the validated hidden-X / auto-Y
overflow pair. `SUMMIT_EXTENDED_GRID_INTEGRATION=0` restores the previous
coverage for comparison or rollback.

`tools/bench/pages/grid-space-between.html` compares deterministic legacy and
modern geometry for positive and negative free space on each axis and for a
50% item width. All five cases matched exactly, and the trace confirmed that
the candidate fixture grids used the modern formatter
(`.vm/bench/probe-20260923-144209-summit-grid-percent-legacy/` and
`.vm/bench/probe-20260923-144232-summit-grid-percent-gfc/`). Successive live
Reddit probes then advanced the hot grid through the `minmax()`, justify,
align, and percentage-width guards. Its next rejection is reason 32, a grid
item with non-visible overflow, so the hot hierarchy still uses the legacy
algorithm and the observed frame-rate differences are not attributable to the
new formatter. The latest complete pass reached 36.19 native-view frames/s,
with a 569.8 ms worst interval and 0.3 ms maximum native queue delay; it also
selected NVDEC H.264 and AAC successfully
(`.vm/bench/scroll-20260923-144313-reddit-grid-gfc-percent-width/`).

The hot item resolves to `overflow-x: hidden` and `overflow-y: auto`. A first
guarded attempt to admit that pair exposed why the existing coverage check was
needed: the modern path initially produced a 302 px item where legacy produced
52 px, and changed its 150 px scroll width to 300 px
(`.vm/bench/probe-20260923-145340-summit-grid-overflow-legacy/` and
`.vm/bench/probe-20260923-145403-summit-grid-overflow-gfc/`). The integration
layer set the grid area's 100 px containing-block width only during the
immediate item layout, then cleared it. A later overflow-layer relayout
therefore resolved the item's 50% width against the 600 px grid container.

Final modern grid layout now keeps each in-flow item's grid-area width on its
renderer, as the legacy grid does. If coverage later rejects that grid, the
fallback path clears the retained sizes before rebuilding the legacy grid.
With this lifetime fix, the modern and legacy paths match every fixture
rectangle, client size, 150 by 60 scroll extent, and overflowing child
rectangle. A dynamic test also switches an initially modern grid to an
unsupported border-box item; its before and after geometry matches legacy and
the trace records fallback reason 26, validating the cleanup path
(`.vm/bench/probe-20260923-150654-summit-grid-dynamic-fallback-legacy/` and
`.vm/bench/probe-20260923-150718-summit-grid-dynamic-fallback-gfc/`).

The corrected guarded path moves Reddit's hot grid to the modern formatter.
In an A-B-A sequence, modern runs reached 57.79 and 55.25 native-view frames/s
with 207.9 and 261.6 ms worst intervals, while the intervening legacy control
reached 41.51 frames/s with a 565.3 ms worst interval. Native queue delay
stayed at 0.3 ms in all three runs. The tradeoff is front-loaded layout work:
the modern traces recorded 5.14 and 4.43 seconds of slow layout versus 1.79
seconds in the control. During the measured scroll, however, Reddit's repeated
slow scroll-listener layouts disappeared and only two frame intervals exceeded
33 ms in each modern run, compared with eight in the control. The rendered
feed remained visually coherent, and the first modern run selected NVDEC
H.264 and AAC successfully
(`.vm/bench/scroll-20260923-150154-reddit-grid-gfc-overflow-fix/`,
`.vm/bench/scroll-20260923-150340-reddit-grid-legacy-overflow-control/`, and
`.vm/bench/scroll-20260923-150443-reddit-grid-gfc-overflow-repeat/`).

Two uncontended ten-iteration Speedometer 3.1 runs with the extended coverage
scored **6.315 ± 0.249** and **6.346 ± 0.320**, both with a 3,748 ms sum of
suite means. The intervening exact opt-out control scored **6.178 ± 0.185**
with a 3,827 ms suite sum. This is a neutral-to-positive result against
Summit's longer-term 6.3–6.4 range and gives no benchmark reason to withhold
the Reddit improvement
(`.vm/bench/speedometer-20260923-150923-extended-grid-speedometer/`,
`.vm/bench/speedometer-20260923-151131-extended-grid-speedometer-control/`, and
`.vm/bench/speedometer-20260923-151340-extended-grid-speedometer-repeat/`).

The coverage is therefore enabled by default on Haiku. A final no-flag
geometry run matched the exact `0` opt-out result, including overflow metrics
and the dynamic fallback transition
(`.vm/bench/probe-20260923-151859-summit-grid-default-optout-legacy/` and
`.vm/bench/probe-20260923-151924-summit-grid-default-modern/`). The final
default `/r/popular/` validation reached 55.27 native-view frames/s with a
261.8 ms worst interval, two intervals over 33 ms, and a 1.6 ms maximum native
queue delay. The hot grid had no fallback reason, and media tracing selected
NVDEC H.264 and AAC successfully
(`.vm/bench/scroll-20260923-152006-reddit-grid-default-modern/`).

### libstdc++ assertion experiment

The normal Release configuration still enables libstdc++ container assertions.
A clean `SkiaCGMiNoStdAssert` engine build tested
`-DUSE_CXX_STDLIB_ASSERTIONS=OFF` while retaining Skia, coordinated graphics,
mimalloc, the same engine patch, and the private Mesa runtime. This rebuilds
JavaScriptCore and WebCore, so it directly covers the JIT, DOM, style, and
layout paths that dominate Speedometer rather than changing only the browser
shell.

The assertion-free bundle scored **6.363 ± 0.226** over ten uncontended
Speedometer 3.1 iterations, with a 3,732 ms sum of suite means
(`.vm/bench/speedometer-20260923-123625-no-stdlib-assertions/`). An immediate
ten-iteration assertion-enabled control scored **6.471 ± 0.245** with a
3,652 ms suite sum
(`.vm/bench/speedometer-20260923-123829-stdlib-assertions-control-post/`). Both
runs began with 0.05 foreign CPU cores of load. Disabling the assertions was
1.7% slower by score and 2.2% slower by aggregate suite time, consistent with
the two earlier assertion-enabled controls at 6.438 and 6.431. The production
build therefore keeps its current setting.

The candidate also completed a live 300-notch `/r/popular/` pass at 47.94
native-view frames/s. Its native UI queue stayed below 0.3 ms while the worst
visual interval reached 598.7 ms, again matching the document-listener stall
rather than an input queue delay. Media tracing selected NVDEC H.264 six times
and AAC once, all with status 0
(`.vm/bench/scroll-20260923-124028-reddit-no-stdlib-assertions/`). This confirms
that the clean candidate retained accelerated Reddit playback, but provides no
reason to adopt the slower assertion setting.

### Canvas 2D acceleration control

The remaining Speedometer gap is concentrated rather than uniform. Against the
same-workstation Firefox baseline, Chart.js costs about 206 ms more per full
iteration, TipTap about 167 ms more, and Perf Dashboard about 106 ms more;
several news and jQuery suites already match or beat Firefox. Chart.js draws
5,366 scatter points, making it a useful focused Canvas 2D workload.

Haiku now accepts `SUMMIT_CANVAS_ACCELERATION=0` to disable the generated
`CanvasUsesAcceleratedDrawing` preference, or any other value to enable it.
Leaving the variable unset preserves WebKit's Skia default. This makes CPU/GPU
comparisons repeatable without maintaining separate engine builds.

Twenty-iteration Chart.js runs measured 369.0 and 384.1 ms with the default
accelerated path around an intervening 376.2 ms CPU run. The per-test split
moved in the expected direction in the first comparison (for example, scatter
draw sync time was 140.3 ms accelerated and 142.1 ms CPU), but the default
repeat drifted beyond that difference. Canvas acceleration is therefore not
the cause of the roughly twofold Firefox gap, and remains enabled by default
(`.vm/bench/speedometer-20260923-153400-chartjs-canvas-default/`,
`.vm/bench/speedometer-20260923-153449-chartjs-canvas-cpu/`, and
`.vm/bench/speedometer-20260923-153542-chartjs-canvas-default-repeat/`).

### Canvas damage coalescing

The larger Chart.js cost was coordinated layer invalidation. Each canvas fill
or stroke sent a dirty rectangle through the canvas element, renderer,
compositing layer, and graphics layer. The 5,366-point scatter plot therefore
sent more than ten thousand notifications per draw even though they all belong
to one frame.

Haiku now unions HTML canvas damage within a frame and skips a notification
when the accumulated rectangle already contains the new damage. Drawing and
cached-image invalidation still happen for every operation. Offscreen canvases
retain the existing behavior, and `SUMMIT_CANVAS_DAMAGE_COALESCING=0` provides
an exact opt-out. This trades some extra pixels in the bounding damage area for
far fewer main-thread calls into the renderer and layer tree.

A 30-iteration Chart.js control averaged 356.7 ms. Two coalesced candidates
averaged 250.1 and 247.3 ms, and the final default implementation averaged
244.3 ms. In the first comparison, translucent scatter drawing fell from
136.6 to 99.9 ms, tooltip redraw from 111.0 to 77.2 ms, and opaque scatter
drawing from 109.1 to 72.9 ms
(`.vm/bench/speedometer-20260923-154323-chartjs-damage-control/`,
`.vm/bench/speedometer-20260923-154417-chartjs-damage-coalesced/`,
`.vm/bench/speedometer-20260923-154518-chartjs-damage-coalesced-repeat/`, and
`.vm/bench/speedometer-20260923-160041-chartjs-damage-default-final/`).

On the full benchmark, the coalesced candidate scored **6.605 ± 0.237** with a
3,509 ms sum of suite means. The immediate same-bundle opt-out control scored
**6.366 ± 0.222** with a 3,717 ms sum. Chart.js fell from 391.9 to 255.3 ms and
Perf Dashboard from 362.3 to 312.4 ms, while TipTap was unchanged at about
289 ms. This is a 3.8% score increase and a 5.6% reduction in aggregate suite
time. A final default-on build repeated the improvement at **6.508 ± 0.235**
and a 3,561 ms suite sum
(`.vm/bench/speedometer-20260923-154611-canvas-damage-coalesced-full/` and
`.vm/bench/speedometer-20260923-154847-canvas-damage-full-control/`, and
`.vm/bench/speedometer-20260923-160224-canvas-damage-default-final/`).

The deterministic `canvas-damage.html` fixture draws 5,000 dispersed marks and
four corner blocks across two display cycles. The default candidate rendered
all marks, returned the exact expected RGBA values at all four corners, and
produced a visually complete screenshot
(`.vm/bench/probe-20260923-160145-summit-canvas-damage-default-final/`). A live
Reddit pass retained the grid improvement at 54.64 native-view frames/s, a
257.2 ms worst interval, two intervals over 33 ms, and 0.3 ms maximum native
queue delay. Its media log selected NVDEC H.264 twice and AAC once with status
0 (`.vm/bench/scroll-20260923-155049-reddit-canvas-damage-coalesced/`).

### Fast full-circle Canvas fills

Perf Dashboard's range-selection test remained about 55 ms slower than
Firefox in its synchronous phase. Application-level timers isolated most of
that phase to Canvas chart redraws. A warmed range selection submitted 6,530
`fill()` calls and 252 `stroke()` calls; its time-series rendering consumed
about 68 ms in Summit and 59 ms in Firefox. Each plotted point uses the common
`beginPath(); arc(..., 0, 2 * Math.PI); fill()` sequence.

WebCore already recognizes single arcs in the Skia backend, but sent a complete
arc without an explicit `closePath()` through `SkCanvas::drawArc`. Canvas fill
semantics implicitly close the subpath, so a complete filled arc is exactly an
oval. The Skia path now uses `drawOval` for that case. Partial arcs and all
unclosed strokes keep the existing path so cap, join, and chord behavior stay
unchanged.

The deterministic `canvas-paths.html` probe submits 5,000 circles per round.
The old bundle took a 21 ms median for open full-circle fills, versus 18 ms for
the already optimized explicitly closed form. The new bundle reduced the open
form to 19–20 ms while leaving closed fills and open strokes essentially
unchanged. A complete 64 by 64 pixel comparison between the two fill forms was
exact. Firefox measured an 18 ms median for the open form on the same machine
(`.vm/bench/probe-20260923-164634-summit-canvas-fill-oval-control/`,
`.vm/bench/probe-20260923-164728-summit-canvas-fill-oval-final/`, and
`.vm/bench/probe-20260923-165007-firefox-canvas-paths-baseline/`).

In matched ten-iteration Perf Dashboard runs, the candidate reduced the suite
from 315.5 to 308.0 ms. Render fell from 52.7 to 48.8 ms and point selection
from 122.6 to 118.8 ms; range selection was unchanged at about 140 ms, leaving
its non-Canvas event and drag work for later investigation
(`.vm/bench/speedometer-20260923-164354-perf-dashboard-fill-oval-control/` and
`.vm/bench/speedometer-20260923-164251-perf-dashboard-fill-oval/`). The full
benchmark scored **6.419 ± 0.217**. Its aggregate suite time was neutral within
normal run variance, while Perf Dashboard improved from the previous final
bundle's 314.7 to 303.7 ms
(`.vm/bench/speedometer-20260923-164803-canvas-fill-oval-full/`).

### Cached Haiku window geometry

Speedometer's Perf Dashboard driver synthesizes 23 mouse events per iteration.
For every event it reads `screenX` and `screenY` while constructing the DOM
event. Each property used to call `WebChromeClient::windowRect()`, which made a
synchronous IPC request to the UI process. The Haiku UI client did not supply a
window frame, so the two round trips also returned the wrong zero origin.

Stage timing around those 23 events attributed 11--14 ms per iteration to
event construction before the change. Haiku now records `BWindow::Frame()` in
the view geometry snapshot and sends it to the WebProcess whenever that
snapshot changes. `windowRect()` reads the cached value there. With the cache,
the benchmark observed the correct `(4, 1)` origin from its requested window
frame, and event construction fell to 1--2 ms. Total measured mouse-event work
fell from 36--37 to 24--25 ms
(`.vm/bench/speedometer-20260923-165401-perf-dashboard-mouse-event-timers/`
and
`.vm/bench/speedometer-20260923-172205-perf-dashboard-window-frame-cache-timers/`).

In clean ten-iteration focused runs, Perf Dashboard fell from 308.0 to
287.0 ms. Its range-selection test, which contains most of these synthesized
events, fell from 140.4 to 124.4 ms. The full ten-iteration candidate scored
**6.582 ± 0.239**, with 3,498 ms of aggregate suite time and a 289.5 ms Perf
Dashboard mean. The previous final bundle scored **6.508 ± 0.235**, with
3,561 ms aggregate time and a 314.7 ms Perf Dashboard mean
(`.vm/bench/speedometer-20260923-172405-perf-dashboard-window-frame-cache/`
and
`.vm/bench/speedometer-20260923-172723-window-frame-cache-full/`).

A live `/r/popular/` check delivered all 48 paced wheel events at 60.7 native
view frames/s. Its worst interval was 26.6 ms, no interval exceeded 33 ms, and
the native queue delay stayed below 0.1 ms. Before and after screenshots confirm
that the feed moved
(`.vm/bench/scroll-20260923-172917-window-frame-cache-reddit/`). The same
candidate played the direct Reddit 720x1280 CMAF regression stream through its
12.7-second `ended` event with no media error. The browser trace selected
`H.264 on the graphics card (NVDEC)` with status 0
(`.vm/bench/probe-20260923-173116-summit-window-frame-cache-reddit-nvdec/`).

### Perf Dashboard layout investigation

After removing the mouse-event IPC cost, 23 `getBoundingClientRect()` calls in
Perf Dashboard still spent about 18--20 ms per iteration in layout. Phase
timing showed that virtually all of this time was inside
`RenderBlockFlow::layout()`; coordinate conversion itself took about 0.006 ms
per call. A trial that allowed a clean canvas leaf through WebKit's partial
layout eligibility check did not improve the focused result (285.4 ms versus a
287.0 ms control).

A one-run dirty-renderer trace explained why. Before the canvas rectangle is
read, the application changes normal-flow text in its heading, navigation,
status view, and table cells. The canvas is clean, but these nodes share its
ancestor chain and carry real `selfNeedsLayout` and `normalChildNeedsLayout`
state. The relevant ancestor also has an out-of-flow child awaiting layout.
Skipping layout for left/top geometry in this state could return stale canvas
coordinates, so the trial shortcut and all tracing were removed
(`.vm/bench/speedometer-20260923-174935-perf-dashboard-layout-phase-split/`,
`.vm/bench/speedometer-20260923-180135-perf-dashboard-clean-replaced-geometry/`,
and
`.vm/bench/speedometer-20260923-182839-perf-dashboard-dirty-renderers/`).

### Fixed-repeat grid coverage trial

A fresh `/r/popular/` pass on the clean production bundle selected NVDEC H.264
for direct video and for an HLS video with separate AAC audio. Its 180-notch
burst delivered 51.14 native-view frames/s with a 256.0 ms worst interval and
0.3 ms maximum UI queue delay. The current bundle also played a direct Reddit
HLS playlist to its 12.7-second `ended` event at 720 by 1280, with no media
error and NVDEC H.264 plus AAC selected
(`.vm/bench/scroll-20260923-183852-reddit-media-current/` and
`.vm/bench/probe-20260923-184005-summit-current-reddit-hls-nvdec/`).

The remaining burst gap coincided with a 212.8 ms page update, including
203.6 ms of final layout. The slow render-tree calls again involved a grid and
its flex item. This grid's first modern-coverage rejection was a fixed
`repeat(...)` column list. A guarded trial admitted fixed repeats whose tracks
were supported breadth or `minmax()` values and whose line-name entries were
empty. WebKit already expands those tracks into `GridTemplateList::sizes` for
the modern formatter. The new `grid-space-between.html` cases confirmed exact
legacy geometry for a three-column repeat; a repeat with named lines stayed on
the legacy path.

On Reddit, that change advanced the hot grid to rejection reason 25, an item
margin, without improving scrolling: 50.75 frames/s and a 280.6 ms worst
interval (`.vm/bench/scroll-20260923-185138-reddit-fixed-repeat-candidate/`).
The modern formatter also has code for fixed margins, so a second guarded
trial admitted fixed margins while leaving auto and other unresolved margins
on the legacy path. The fixed-margin fixture matched legacy geometry and its
auto-margin counterpart still fell back. Reddit's hot item continued to
reject on a nonfixed margin, while its burst reached 35.69 frames/s with an
894.2 ms worst interval. Live feed content varied, so this scroll comparison
alone does not quantify the candidate's cost
(`.vm/bench/scroll-20260923-185606-reddit-repeat-margin-candidate/`).

The full ten-iteration Speedometer comparison settled the decision. Production
scored **6.557 ± 0.243**, fixed-repeat-only scored **6.589 ± 0.226**, and the
fixed-repeat plus fixed-margin build scored **6.236 ± 0.140**. Aggregate suite
time was 3,513, 3,506, and 3,680 ms respectively. These were uncontended
runs on the same workstation and Mesa runtime; several application suites
slowed under the margin candidate. Both engine coverage changes were removed.
The geometry fixture remains for a later implementation that can demonstrate
a real improvement
(`.vm/bench/speedometer-20260923-185904-fixed-repeat-margin-production-control/`,
`.vm/bench/speedometer-20260923-190055-fixed-repeat-only-full/`, and
`.vm/bench/speedometer-20260923-185710-fixed-repeat-margin-full/`).

### Why the legacy grid lays out the item twice

The hot Reddit grid runs its flex item once to measure intrinsic row height and
again for final placement. A focused `RenderGrid` trace found that the grid
area width remained 1121 px across those passes. Row sizing temporarily
cleared the area's height, and final placement restored a definite height.
The item has percentage-height descendants; WebKit's stretch requirement
therefore requested another layout even when the requested outer height
matched its current height. Those descendants can resolve differently between
the indefinite intrinsic pass and the definite final pass. Skipping the second
layout based only on unchanged outer dimensions would be unsafe.

The temporary area and stretch logs were removed after the check. They
produced tens of thousands of lines and severely distorted frame timing, so
their scroll fps is excluded from performance comparisons. The dependency is
visible in `.vm/bench/scroll-20260923-190821-reddit-grid-area-sizes/` and
`.vm/bench/scroll-20260923-191117-reddit-grid-stretch-values/`.

### TipTap selection canonicalization phases

The earlier TipTap sampler put `VisiblePosition::canonicalPosition` on 13.4%
of inclusive samples. A temporary Haiku timer separated its mandatory
`Document::updateLayoutIgnorePendingStylesheets()` call from the subsequent
upstream/downstream candidate search. In a 15-iteration TipTap-only run, the
first 160 recorded canonicalizations spent 656.6 ms in layout and only 1.35 ms
in candidate search. The suite averaged 153.5 ms including its cold pass
(`.vm/bench/speedometer-20260923-192145-tiptap-canonical-position-phases/`).

A three-iteration full-suite trace recorded 420 canonicalizations across all
suites. Their cumulative layout time was 270.7 ms and candidate search was
0.81 ms; almost all layout time arrived in two large bursts after calls 130
and 280. The full run's TipTap mean was 321.3 ms, but this aggregate trace
does not tag individual calls by suite, so it cannot assign those bursts or
the entire cross-suite regression to TipTap
(`.vm/bench/speedometer-20260923-192259-full-canonical-position-phase-trace/`).

Canonicalization's search is already cheap. The useful target is the forced
layout of the editor's flex and inline content after DOM changes, subject to
selection correctness. The diagnostic timer was removed and the production
WebProcess and NetworkProcess rebuilt from the committed engine source.

The production bundle's existing page-update trace confirms the layout shape
in five focused TipTap iterations. Each warm pass produced two slow
render-tree layouts of about 31--37 ms, nested in animation-frame callbacks
lasting about 59--72 ms. A `RenderFlexibleBox` contained a 16--21 ms
`RenderBlock` layout in each pass; the rest of the render-tree time is outside
that inclusive subtree. The cold pass had two roughly 100 ms layouts. These
samples point to repeated flex and inline layout of the editor view, not a
slow candidate-position walk
(`.vm/bench/speedometer-20260923-192710-tiptap-layout-renderers/`).

### Why TipTap repeats its nested flex layout

An opt-in phase trace split the outer one-item flex box's warm layout into
roughly 15 ms of base sizing and 16--22 ms of content sizing. Its nested
two-item flex box consumed nearly all of each phase; alignment and final rect
placement were under 0.1 ms. The first cold pass took about 100 ms per layout
(`.vm/bench/speedometer-20260923-193145-tiptap-flex-phase-trace/`).

The expensive final child layout reported an unchanged row main size,
percentage-height descendants, and a request to relayout. A narrow Haiku
trial suppressed that percentage-height request for row items, since their
unchanged inline-size override does not itself change block-size definiteness.
The five-step `flex-row-percent-height.html` fixture matched the opt-out
exactly for auto-height, definite-height, flex-start, and column controls
(`.vm/bench/probe-20260923-194213-summit-flex-row-percent-control/` and
`.vm/bench/probe-20260923-194244-summit-flex-row-percent-candidate/`).

The trial did not save a layout. In matched 20-iteration TipTap runs on the
same bundle, the opt-out averaged 146.5 ms (132.8 ms warm) and the candidate
147.1 ms (134.0 ms warm). The candidate's layout trace retained the same
16--22 ms nested flex calls. A final dirty-state trace found that the child
already had `needsLayout` on entry to final sizing, before either the
unchanged-width override or the percentage-height rule. Intrinsic-width
measurement in base sizing is the likely source of that invalidation, so
clearing only the percentage-height relayout request cannot avoid the final
pass. The ineffective engine change and all temporary traces were removed;
the workstation production engine was rebuilt clean
(`.vm/bench/speedometer-20260923-194320-tiptap-row-flex-control/`,
`.vm/bench/speedometer-20260923-194406-tiptap-row-flex-candidate/`,
`.vm/bench/speedometer-20260923-194502-tiptap-row-flex-candidate-layout/`, and
`.vm/bench/speedometer-20260923-194803-tiptap-flex-candidate-dirty/`).

A narrower cross-size trace ruled out one proposed cause. On each warm TipTap
pass, the nested flex item's intrinsic-width measurement entered its
cross-size scope clean and remained clean after both setting the temporary
size and invalidating cached content widths. Its prior block-size override
was present. The dirty state seen at final row sizing therefore does not
originate in that particular cache-invalidation call
(`.vm/bench/speedometer-20260923-195422-tiptap-flex-cross-size-invalidation/`).

A second trace around flex-base and min/max measurement found the hot row
child already dirty at the start of base sizing and still dirty after both
measurements. Its measured base was zero while its current content width was
790 px. This establishes why merely suppressing the percentage-height
request had no effect: base sizing has not completed a final layout for that
row child. A faster implementation would have to reuse intrinsic work while
still laying out real dirty content, or optimize that layout itself
(`.vm/bench/speedometer-20260923-195744-tiptap-flex-base-dirty/`).

### Live Reddit CPU paint-worker check

The production bundle was run through a live `/r/popular/` 180-notch A/B/A
sequence with two, four, then two Skia CPU painting threads. The measured
native-view scroll windows reached 55.54, 58.05, and 58.07 frames/s; their
worst intervals were 68.5, 61.9, and 47.6 ms, with maximum native queue
delays of 0.5, 1.0, and 0.1 ms. The harness captured two seconds in the first
two scroll windows and three seconds in the last, while the live feed and idle
load also varied. These runs do not establish a four-worker advantage, so the
two-worker production default remains unchanged. They do show that native
queueing is still short during these bursts
(`.vm/bench/scroll-20260923-200020-reddit-workers2-control/`,
`.vm/bench/scroll-20260923-200104-reddit-workers4-candidate/`, and
`.vm/bench/scroll-20260923-200144-reddit-workers2-repeat/`).

### Complete native-view burst snapshots

The prior `SUMMIT_UI_FRAME_STATS` log reported a window only when another
frame arrived. If Reddit stopped presenting near the end of a wheel burst,
`run-scroll.py` could cut its log before the ongoing gap was reported. The
browser's opt-in native view now keeps cumulative counts from the exact moment
the burst starts. `summitctl framestats` reads the count, elapsed time,
completed gaps, pending gap, and UI queue delay on the window thread. The
harness prefers that snapshot and also records one 1.1 seconds later. Older
bundles still use the periodic logs; that fallback completed on
`bundle-h0em65eq`
(`.vm/bench/scroll-20260923-201311-reddit-old-bundle-frame-fallback/`).

Two uninstrumented 300-notch `/r/popular/` passes on the snapshot bundle
delivered 147 frames in 5.21 seconds and 165 frames in 5.04 seconds: **28.2**
and **32.8 native-view frames/s** across the full bursts. At burst end they
had pending presentation gaps of 2.62 and 2.05 seconds, with UI queue delays
below 0.4 ms. In the second pass no new frame arrived during the following
1.1 seconds, so its pending gap grew to at least 3.28 seconds. Before/after
screenshots confirm that the feed moved. Earlier periodic-window scroll rates
can overstate long-burst smoothness when their reported windows cover only a
small part of the burst
(`.vm/bench/scroll-20260923-201123-reddit-frame-snapshot-check/` and
`.vm/bench/scroll-20260923-201529-reddit-frame-snapshot-uninstrumented-repeat/`).

An opt-in page trace during another 300-notch run saw one 1.71-second
document-scroll event dispatch, with repeated flex, grid, and block layouts
inside it. Its 15,000 renderer lines materially affect timing, so its frame
rate and 4.08-second completed gap are diagnostic rather than production
comparisons. The uninstrumented snapshots independently prove a multi-second
presentation stall above the native queue
(`.vm/bench/scroll-20260923-201421-reddit-frame-snapshot-page-phases/`).

`SUMMIT_PAGE_UPDATE_TRACE=2` keeps the page, scroll-dispatch, listener, and
layout-phase records but leaves the renderer and grid-item records disabled.
A 300-notch pass produced 82 browser log lines and caught one 1,582.0 ms
JavaScript document-scroll listener. Its containing rendering update took
1,867.3 ms, including 1,620.6 ms in scroll steps. The slow layout calls
included 225.3, 157.1, and 108.1 ms in their post-layout slices; nested
calls must not be summed as independent work. The native-view snapshot saw
118 frames in 5.19 seconds (22.7 frames/s), with a 2.81-second pending gap
and 0.1 ms maximum UI queue delay. On the same bundle with tracing off, a
separate live-feed pass saw 131 frames in 5.07 seconds (25.8 frames/s), a
2.77-second pending gap, and 0.1 ms queue delay. Feed variation prevents an
overhead estimate, but the long stall remains with tracing off
(`.vm/bench/scroll-20260923-202313-reddit-page-phases-low-volume/` and
`.vm/bench/scroll-20260923-202403-reddit-page-phases-off-control/`).

The post-layout trace is now split into `postDidLayout`, `postTasks`, and
`postOther`. Another low-volume pass caught a 1,521.3 ms document-scroll
listener and 115 frames in 5.19 seconds (22.2 frames/s), with a 3.12-second
pending presentation gap. Its three large outer post-layout slices were
222.2, 154.9, and 105.2 ms. The corresponding task slices were 217.8,
152.2, and 103.5 ms; each contained a synchronous follow-up render-tree
layout of 217.7, 152.0, and 103.4 ms. `didLayout` itself took only 4.4,
2.7, and 1.7 ms. The reported 490.3 ms total of slow post-layout slices is
therefore mostly nested layout, not an independent layer-position walk.
Avoiding that work requires understanding why the page repeatedly dirties
the render tree during scroll, while preserving its synchronous geometry
queries (`.vm/bench/scroll-20260923-202951-reddit-post-layout-split/`).

The trace now reports the first post-layout callback that changes the layout
state from clean to dirty. In two further 300-notch Reddit passes it found
four such transitions during the viewport section, then four specifically in
`updateLayoutViewport()`; `viewportContentsChanged()` did not dirty layout.
The latter pass delivered 123 frames in 5.07 seconds (24.3 frames/s) but had
a 2.90-second pending gap. `updateLayoutViewport()` can change the layout
viewport origin, which calls `setViewportConstrainedObjectsNeedLayout()` for
fixed or sticky content. This is the likely route to the observed follow-up
layouts, although the trace does not yet separate that call from visual
viewport notification. Skipping these layouts without a fixed-element
correctness check would risk visible position errors
(`.vm/bench/scroll-20260923-203515-reddit-post-task-dirty/` and
`.vm/bench/scroll-20260923-203856-reddit-viewport-dirty/`).

An opt-in trial skipped layout invalidation when the layout viewport origin
changed. The new `viewport-origin-fixed.html` probe recorded exactly the same
fixed and sticky rectangles at five scroll positions in control and trial,
and final screenshots differed only in a small bottom-right desktop region.
On live Reddit, traced A/B/A 300-notch passes reduced the slow document-scroll
dispatch from 1,223.7 ms in the control to 857.3 and 896.1 ms in the two
trial runs; slow post-layout slices fell from 349.9 ms to 12.9 and 15.6 ms.
However full-burst native-view rates were 26.4 frames/s in the control versus
22.4 and 22.6 in the trial, and all three retained 2.7--3.0 second gaps.
Live-feed variation limits that comparison. The trial did not demonstrate
smoother scrolling, and the fixture covers only a small fixed/sticky case, so
the production layout invalidation remains in place
(`.vm/bench/probe-20260923-204405-summit-viewport-origin-control/`,
`.vm/bench/probe-20260923-204718-summit-viewport-origin-candidate/`,
`.vm/bench/scroll-20260923-204754-reddit-viewport-origin-candidate-trace/`,
`.vm/bench/scroll-20260923-204841-reddit-viewport-origin-control-trace/`, and
`.vm/bench/scroll-20260923-204926-reddit-viewport-origin-candidate-repeat/`).

Media remains verified for the finite Reddit CMAF case: the current bundle
loaded its HLS metadata in 468 ms, played 720×1280 video to `ended` at 12.7
seconds with no media error, and selected NVDEC H.264 plus AAC. A live feed
trace also selected those decoders with status 0, though Media Kit emitted a
stream-2 decoder warning; decoder selection alone does not prove that every
feed post completes playback
(`.vm/bench/probe-20260923-200527-summit-reddit-current-direct-hls-media-check/`
and `.vm/bench/scroll-20260923-200427-reddit-live-media-codec-lifecycle/`).

### Advertise the finite Reddit HLS playback path

The Haiku media backend could already play a direct finite Reddit HLS URL,
but `video.canPlayType("application/vnd.apple.mpegurl")` and the alternate
`application/x-mpegURL` both returned empty. A typed `<source>` fixture
therefore skipped a working Reddit HLS playlist and selected an unusable MP4
fallback. The baseline stayed at time zero with no video dimensions
(`.vm/bench/probe-20260923-205355-summit-reddit-media-capabilities/` and
`.vm/bench/probe-20260923-205459-summit-hls-source-selection-control/`).

The Haiku engine now includes both HLS MIME types in its supported list and
returns `maybe` for them. `Maybe` is deliberate because the implementation
handles finite, unencrypted, same-resource byte-range CMAF playlists rather
than every HLS form. The candidate selected the typed HLS source, loaded
metadata in 480 ms, and reached `ended` at 12.7 seconds with no media error;
the chosen decoders were NVDEC H.264 and AAC. `MediaSource` remains unavailable
(`.vm/bench/probe-20260923-205751-summit-hls-capabilities-candidate/` and
`.vm/bench/probe-20260923-205821-summit-hls-source-selection-candidate/`).

A live `/r/popular/` 180-notch pass on the candidate selected NVDEC H.264
and resolved separate HLS video and AAC audio twice, with no logged media
lifecycle stall over 10 ms. It still delivered only 36.15 native-view
frames/s during its 3.29-second burst and had a 1.15-second pending frame
gap. This verifies live feed decoder selection, not completion of every feed
post; the scrolling bottleneck remains
(`.vm/bench/scroll-20260923-205917-reddit-hls-capability-live/`).

### Matched-viewport Speedometer and timer investigation

On the current HLS-capable bundle, a fresh full-screen, 1913×945 content-view
Speedometer run scored 6.591 ± 0.240. The saved Firefox 155 baseline used a
1280×887 content view, so Summit was rerun at exactly that size and scored
6.779 ± 0.258 against Firefox's 8.338 ± 0.374. TipTap took 282.8 ms versus
Firefox's 124.8 ms; Preact and Svelte took 70.6 and 64.1 ms versus 43.4 and
39.6 ms. `compare-runs.py` now warns if the two content viewports differ
(`.vm/bench/speedometer-20260923-210142-hls-capability-current/` and
`.vm/bench/speedometer-20260923-210422-matched-firefox-viewport/`).

An A/B/A 20-iteration Preact/Svelte/Lit check at the matched viewport scored
14.828 with display-rate compositor pacing, 14.281 with its cap disabled,
then 13.747 after restoring the cap. The downward drift makes the pacing
comparison inconclusive; no compositor setting changed
(`.vm/bench/speedometer-20260923-210831-dom-async-pacing-control/`,
`.vm/bench/speedometer-20260923-210941-dom-async-pacing-uncapped/`, and
`.vm/bench/speedometer-20260923-211053-dom-async-pacing-repeat/`).

The platform probe's 50 forced layouts over 2,000 inline blocks took 1,966 ms
in Summit versus 361 ms in Firefox, and nested zero-delay timers had an 8 ms
versus 4 ms median. The existing inline-reuse coverage trace in a 20-pass
isolated TipTap benchmark found zero eligible inline boxes: its hot layout is
flex content, so extending that guarded inline shortcut would miss TipTap
(`.vm/bench/probe-20260923-211312-summit-current-async-window/`,
`.vm/bench/probe-20260923-211353-firefox-current-async-window-firefox/`, and
`.vm/bench/speedometer-20260923-211557-tiptap-inline-coverage/`).
The Preact/Svelte/Lit focused run likewise found only 18 eligible fixed inline
boxes across 3,597 renderer visits; 2,970 were non-box renderers and 609
were dirty. The large synthetic inline-layout gap is real, but extending the
current clean-box shortcut has too little coverage to close these suite gaps
(`.vm/bench/speedometer-20260923-214643-dom-inline-coverage/`).

A temporary run-loop trace showed that the main shared timer's 8 ms samples
were mostly deadlines armed at about 7.87 ms; median delivery lateness was
roughly 0.06 ms. Raising the timer thread's priority did not change the
8 ms nested-timer median. An opt-in trial removed only the extra 4 ms
alignment for visible, maximally nested Haiku timers while retaining WebKit's
4 ms minimum and hidden-page throttling. It moved the nested-timer median to
4 ms and the shared-timer deadline to about 3.97 ms. The benchmark's small
asynchronous-window probe was unchanged, however. Full matched-viewport
Speedometer A/B/A runs scored 6.513, 6.447, and 6.676, with TipTap around
287–289 ms in all three. The change did not improve the requested benchmark;
the trial and trace were removed and the production workstation engine was
rebuilt with the committed timer behavior
(`.vm/bench/probe-20260923-211857-summit-timer-priority-control/`,
`.vm/bench/probe-20260923-211936-summit-timer-priority-candidate/`,
`.vm/bench/probe-20260923-212957-summit-named-timer-latency/`,
`.vm/bench/probe-20260923-213454-summit-timer-no-alignment-probe/`,
`.vm/bench/speedometer-20260923-*-no-alignment-*/`).

### Skia GL tile painting trial on the GTX 1070

The current Haiku coordinated renderer composites through Mesa Zink/NVK, but
`PlatformDisplay::skiaGLContext()` returns null on Haiku. That keeps Skia's
page tiles on its two CPU painting threads. A temporary opt-in trial allowed
Haiku to create Skia GL contexts; an engine diagnostic confirmed `GPU` tile
painting in the WebProcess. The trial was built and tested, then removed.

At the 1913×945 content viewport, the 400-card, 300-frame scroll probe ran at
59.25 fps with GPU tile painting and 58.96 fps with CPU painting. Both had no
frame over 33 ms, so this probe showed no useful gain
(`.vm/bench/probe-20260923-215242-summit-skia-gl-trial/` and
`.vm/bench/probe-20260923-215321-summit-skia-cpu-control/`).

On the live `/r/popular/` 300-notch burst, GPU painting delivered 19.61
native-view frames/s with a 3.35-second pending presentation gap, versus
23.21 frames/s and a 3.01-second gap with CPU painting. Reddit load varied
even before the bursts, so these two runs do not establish a precise
regression, but they do not support enabling GPU tile painting. Mesa logged
`ZINK: vkCreateImage failed (VK_ERROR_UNKNOWN)` in the GPU run, but not the
CPU control. Both runs visibly scrolled
(`.vm/bench/scroll-20260923-215402-reddit-skia-gl/` and
`.vm/bench/scroll-20260923-215446-reddit-skia-cpu/`).

At the matched 1280×887 Speedometer viewport, GPU tile painting scored
6.888 ± 0.280 over ten uncontended iterations, compared with the existing
CPU-painted 6.779 ± 0.258. The confidence ranges overlap; TipTap remained
278.1 ms versus the CPU baseline's 282.8 ms. No Speedometer improvement is
established. The explicit diagnostic run logged `GPU` from the
`SkiaPaintingEngine` constructor, verifying the tested path
(`.vm/bench/speedometer-20260923-215530-skia-gl-trial/` and
`.vm/bench/probe-20260923-215908-summit-skia-gl-diagnostic/`).

The workstation engine was rebuilt with the original CPU tile path after the
trial. Vulkan through Zink is already exercised for compositing; a native
Skia Vulkan backend would require separate buffer sharing and synchronization
work rather than a backend switch alone.

### Reject media containers with no usable tracks

The Haiku media loader treated a recognized container as successfully loaded
even when Media Kit could not decode any of its tracks. A 775-byte MP4 with
only a `mov_text` subtitle track reproduced this: Media Kit rejected stream
zero, while the page emitted `loadedmetadata`, `canplay`, and `playing` with
zero dimensions and time stuck at zero. A following Reddit HLS `<source>` was
never tried (`.vm/bench/probe-20260923-220915-summit-unsupported-track-control/`
and `.vm/bench/probe-20260923-221045-summit-unsupported-track-fallback-control/`).

The loader now requires at least one decoded video or audio track before it
reports `Loaded`. With the same source list, Summit skipped the unsupported
MP4, selected Reddit's HLS playlist, decoded 381 H.264 frames through NVDEC
with AAC audio, and reached `ended` at 12.7 seconds without a media error.
The fixture is `tools/bench/pages/unsupported-track.mp4`, generated from a
single subtitle cue with `ffmpeg -f srt -i unsupported-track.srt -c:s mov_text`;
the browser probe is `tools/bench/pages/media-track-fallback.html`
(`.vm/bench/probe-20260923-221321-summit-unsupported-track-fallback-candidate/`).
The final rebuilt bundle repeated that `ended` result with the diagnostic
trace disabled (`.vm/bench/probe-20260923-221823-summit-unsupported-track-fallback-final/`).

The new opt-in `SUMMIT_MEDIA_PLAYBACK_TRACE=1` records each player's usable
track count, first decoded video frame, five-second progress, and decode end.
It is disabled by default. A direct Reddit HLS control reached its 12.7-second
`ended` event with 381 decoded NVDEC frames. Live `/r/popular/` passes before
and after the track fix each loaded NVDEC video plus AAC for HLS posts and
decoded a visible video past five seconds. No loaded container without usable
tracks appeared in those two feed samples, so the fix is verified for the
reproduced source-fallback failure, not as an explanation for all Reddit
posts. Scrolling still had 2.78- and 2.97-second presentation gaps in those
passes (`.vm/bench/probe-20260923-220638-summit-playback-trace-hls/`,
`.vm/bench/scroll-20260923-220732-reddit-playback-trace/`, and
`.vm/bench/scroll-20260923-221449-reddit-playback-track-fix/`).

### Async wheel scrolling and coordinated composition

The workstation build enables `ENABLE_ASYNC_SCROLLING`, Haiku's scrolling
thread, and Summit's threaded-scrolling preference. A 300-notch live Reddit
trace found a scrolling tree for every event and routed all 300 to that
thread with no blocking DOM-dispatch step. The expanded result trace showed
132 events handled there and 168 unhandled, with none requesting main-thread
wheel processing. The burst still delivered 26.77 native-view frames/s and
had a 2.76-second pending presentation gap with a 0.6 ms maximum UI queue
delay (`.vm/bench/scroll-20260923-223047-reddit-wheel-result/`).

An opt-in compositor timing trace measured 367 composed frames in a separate
run. Its median frame time was 4.7 ms, p95 was 9.9 ms, and the maximum was
108.6 ms; there was only one explicit async-scrolling composition request.
That run's longest completed-frame gap was 0.81 seconds and its burst snapshot
had a 0.81-second pending gap, while other live passes still had multi-second
gaps. The timing trace therefore points to missing or delayed composition
requests and page content production more than cost inside an ordinary
compositor frame (`.vm/bench/scroll-20260923-222641-reddit-compositor-timing/`).

Haiku does not define `HAVE_DISPLAY_LINK`; WebKit normally uses its display
refresh callback to advance `ThreadedScrollingTree` layer positions. A
temporary opt-in trial pulsed that tree after handled wheel events. It raised
explicit async-scrolling composition requests from one to 136 in the traced
300-notch pass. In an uninstrumented A/B/A sequence on the same bundle,
native-view rates were 23.32, 48.50, and 21.45 frames/s, with pending gaps
of 3.05, 0.94, and 2.85 seconds. However, both trial screenshots showed a
blank Reddit feed at its footer where the controls showed posts. The trial
may have reached the end of currently loaded posts before Reddit supplied
more; the captures do not distinguish lazy loading from missing paint. The
pulse was removed. A future refresh source needs a content-visible result
under sustained scrolling before enabling it by default
(`.vm/bench/scroll-20260923-223705-reddit-wheel-refresh-candidate/`,
`.vm/bench/scroll-20260923-223819-reddit-refresh-control/`,
`.vm/bench/scroll-20260923-223905-reddit-refresh-candidate/`, and
`.vm/bench/scroll-20260923-223955-reddit-refresh-control-repeat/`).

A shorter 80-notch follow-up rendered feed posts with and without the pulse.
The trial yielded 48.28 frames/s versus 54.85 for the safe build, both with
no substantial pending presentation gap. This pass does not establish a
benefit for ordinary-length bursts. An initial follow-up run was invalid:
the private Mesa path was omitted and the WebProcess exited before scrolling.
The two reported runs both used `SUMMIT_LIBRARY_PATH_PREFIX` to select the
workstation's Mesa (`.vm/bench/scroll-20260923-224807-reddit-refresh-80-valid/`
and `.vm/bench/scroll-20260923-224905-reddit-control-80/`).

`SUMMIT_SCROLL_POSITION_TRACE=1` now records the main-frame scrolling tree's
position, current maximum, content height, and wheel handling result under
the tree lock. `run-scroll.py` summarizes the first event at the current
maximum, subsequent content-size changes, and the first event at the final
maximum. This distinguishes a long pause while a finite feed is at its
boundary from a scrolling tree that is still advancing; frame rate alone
cannot make that distinction.

In a 300-notch safe-build run, the first 3,038-px Reddit document reached its
scroll maximum on event 21. The document expanded to 17,736 px on event 126,
and the tree reached its new maximum on event 248. Overall 158 of 300 wheel
events found the tree at its then-current maximum, 144 were handled, and the
native view delivered 24.47 frames/s with a 2.94-second pending gap. The final
image still showed a post rather than the new maximum; this confirms that
scrolling-tree position and visible layer position can diverge without display
refresh pulses (`.vm/bench/scroll-20260923-225629-reddit-position-300/`).

An additional 180-notch pair illustrates the unresolved tradeoff. With refresh
pulses, the native view delivered 54.85 frames/s but the final feed was blank.
Without pulses, it delivered 34.97 frames/s and showed a post. In that safe
run the first maximum arrived at event 21, content expanded at event 131, and
the tree advanced to 8,488 px by event 180. Live content differed between the
two passes, so their frame rates do not isolate the pulse's cost. The pulse
remains disabled until scrolling can advance visibly without leaving a blank
feed (`.vm/bench/scroll-20260923-225828-reddit-refresh-180/` and
`.vm/bench/scroll-20260923-225929-reddit-safe-180/`).

### Periodic Haiku scrolling refresh trial (September 24)

The current text-width build still produced a 1.48-second pending frame gap in
a 300-notch `/r/popular/` burst. Its scrolling tree advanced to 16,256 px of
a 16,652-px maximum while the final image showed an earlier post. A Haiku-only
trial calls `ThreadedScrollingTree::displayDidRefresh()` on the event queue at
`SUMMIT_SCROLL_REFRESH_TIMER=16` (or `=1`) during recent wheel activity; `=33`
tests a slower cadence. The timer stops when the trees become inactive and is
**disabled by default**. Haiku has no system display-link callback for this
path.

On one trial bundle, a 300-notch timer run delivered 55.96 native-view
frames/s with a 10.3 ms pending gap, versus 36.29 frames/s and a 1.42-second
gap with the timer off. The timer screenshot showed a post and Reddit's loading
spinner at the then-current scroll maximum. An 180-notch pair delivered 54.45
versus 47.51 frames/s, with visible feed posts in the timer screenshot. These
passes establish that missing refreshes account for much of the long frame
gap (`.vm/bench/scroll-20260924-071341-width-cache-reddit-long/`,
`.vm/bench/scroll-20260924-072114-reddit-refresh-timer-candidate/`,
`.vm/bench/scroll-20260924-072229-reddit-refresh-timer-control/`,
`.vm/bench/scroll-20260924-072340-reddit-refresh-timer-180-candidate/`, and
`.vm/bench/scroll-20260924-072422-reddit-refresh-timer-180-control/`).

The timer is not ready for general use. A packaged default-on 16 ms run
delivered 54.02 frames/s but ended with a blank feed at 13,084 px, still 2,337
px short of the current document maximum. A 33 ms trial also ended blank and
delivered only 37.02 frames/s with an 826 ms gap. A later 16 ms run reproduced
the blank feed. Opt-in `SUMMIT_COMPOSITOR_TIMING_TRACE=1` now includes the
scene's pending-tile count: 69 of 70 async composition requests in that blank
run reported zero pending tiles. The count does not prove the page had painted
content at the newly exposed position; it rules out a simple wait on the
compositor's counted tile jobs as the full explanation. Follow-up work needs
to distinguish Reddit's content production from tile invalidation and
composition at the tree's current position
(`.vm/bench/scroll-20260924-073417-reddit-refresh-default-300/`,
`.vm/bench/scroll-20260924-073758-reddit-refresh-33ms-300/`, and
`.vm/bench/scroll-20260924-074350-reddit-refresh-pending-tiles/`).

Matched-viewport ten-iteration Speedometer scores with the opt-in timer were
6.214 and 6.967, bracketing a 6.727 timer-off control. A complete traced
Speedometer iteration routed zero wheel events, so the refresh timer was not
started by that benchmark; the score spread should not be attributed to the
timer without further evidence
(`.vm/bench/speedometer-20260924-072513-scroll-refresh-timer-speedometer-candidate/`,
`.vm/bench/speedometer-20260924-072705-scroll-refresh-timer-speedometer-control/`,
`.vm/bench/speedometer-20260924-072906-scroll-refresh-timer-speedometer-candidate-repeat/`,
and `.vm/bench/speedometer-20260924-073108-scroll-refresh-timer-wheel-count/`).

The packaged diagnostic build, with the timer left off, completed an
80-notch Reddit smoke at 56.54 native-view frames/s with a 5.9 ms pending gap;
its final screenshot showed rendered posts
(`.vm/bench/scroll-20260924-074549-reddit-refresh-diagnostic-safe/`).

An opt-in `SUMMIT_SCROLL_DOM_TRACE=1` probe now hit-tests two points in the
WebCore viewport when a timed scroll burst stops. It logs the element ancestry
and bounds, without page text. During a reverse 80-notch burst after a 300-notch
forward burst, the final screenshot showed a blank Reddit feed at scrolling-tree
position 4,674 px (maximum 26,630 px). The corresponding DOM trace found an
image inside `SHREDDIT-POST` at viewport y=200 and a link inside another
`SHREDDIT-POST` at y=400, with rectangles covering those points. A later
screenshot, without additional input, showed the post image at that position.
Thus Reddit had produced the post elements while the feed was blank; this is
strong evidence of delayed painting or layer presentation after asynchronous
scrolling, rather than an empty DOM. The trace and screenshot are close in time
but are not atomic. The next investigation should compare the committed layer
state and tile damage with the scrolling tree's position
(`.vm/bench/scroll-20260924-080715-reddit-refresh-dom-reverse/`).

The follow-up ruled out two direct flush requests. An opt-in final rendering
update after wheel activity ended still produced a blank reverse-scroll feed on
repeat (`.vm/bench/scroll-20260924-081518-reddit-final-render-update-repeat/`).
Scheduling a flush when `GraphicsLayerCoordinated::syncPosition()` dirtied tile
coverage also failed on repeat and had no consistent frame-pacing gain
(`.vm/bench/scroll-20260924-082030-reddit-coverage-flush-repeat/`). Both
experiments were removed.

Opt-in `SUMMIT_TILE_COVERAGE_TRACE=1` logs the visible and cover rectangles,
tile count, and creation state for large coordinated backing stores.
`SUMMIT_LAYER_UPDATE_TRACE=1` logs scroll-update notification delivery and
the layer host's schedule, render, and composite handoff. A blank capture at
scrolling-tree y=4,474 had the large content layer's most recent visible
rectangle at y=14,074. During the reverse burst the compositor kept presenting
asynchronous frames, but no main-thread rendering update or new tile coverage
reached the log before the screenshot. The UI frame trace recorded a 648 ms
queue delay. Three seconds later, the queued scroll notifications ran, the
layer's visible rectangle changed to y=4,474, and a settled screenshot showed
the post image. This identifies delayed main-thread scroll reconciliation and
backing-store coverage as the immediate source of that blank interval. The
next trial should reduce or coalesce redundant main-thread scroll-update
notifications while preserving wheel-event test deferrals
(`.vm/bench/scroll-20260924-083224-reddit-layer-handoff/`).
An opt-in notification-coalescing trial kept those deferrals but reduced
forward-burst main-thread notifications from 115 to 13 in same-bundle runs.
The 300-notch candidate and control delivered 47.02 and 47.69 native-view
frames/s, with 699 and 726 ms pending gaps respectively. Both reverse-scroll
screenshots still contained blank feed regions. Fewer notifications did not
make the renderer catch up, so the trial was removed
(`.vm/bench/scroll-20260924-084114-reddit-notification-coalesce/` and
`.vm/bench/scroll-20260924-084235-reddit-notification-control/`).
With both new traces disabled and the refresh timer left off, the same bundle
completed an 80-notch Reddit smoke at 56.02 native-view frames/s with a 9.5 ms
pending gap and visible feed posts; it emitted no layer or tile trace lines
(`.vm/bench/scroll-20260924-083607-reddit-layer-diagnostics-safe/`).

### TipTap intrinsic-width rebuilding

An opt-in `SUMMIT_FLEX_WIDTH_TRACE=1` probe narrowed TipTap's flex sizing
cost. In eight isolated iterations, 17 `min-content` contributions exceeded
0.5 ms and totaled about 404 ms. The two cold calls took roughly 98 and 88
ms; warm calls on the editor's flex item took about 14--16 ms. Every slow
call entered with invalid intrinsic widths and a dirty renderer, then returned
the same 598.8-px minimum width. Equality of the answer does not make the
cached value reusable after the editor's content changes. The nearby explicit
max-content and table min-content helpers produced no calls over 0.5 ms; the
cost is in `computeMainAxisExtentForFlexItem`'s ordinary minimum-size branch
(`.vm/bench/speedometer-20260923-230957-tiptap-minimum-width-trace/` and
`.vm/bench/speedometer-20260923-231515-tiptap-intrinsic-phase-trace/`).

`SUMMIT_INTRINSIC_WIDTH_TRACE=1` split nested inline width work. Among 119
`minimumMaximumContentSize` calls over 0.5 ms in the same eight-iteration
run, 130.6 of 131.6 ms was rebuilding inline-item lists, including lists of
over 3,000 items. These are inclusive nested timings with a reporting
threshold, so they cannot be subtracted from the outer flex call total. The
119 slow records identified 118 distinct inline roots; most expensive lists
were fresh, which also limits the value of reserving from a previous list.

A guarded trial reserved a fresh inline-item vector from the previous list's
length, capped at 4,096 entries. In same-bundle, uncontended 20-iteration
TipTap control/candidate/control runs, suite means were 148.2, 150.4, and
147.6 ms. The hint did not improve the editor and was removed. Future work
needs to reduce per-item rebuilding or reuse unaffected inline content while
preserving edits, instead of changing vector capacity
(`.vm/bench/speedometer-20260923-231939-tiptap-inline-reserve-control/`,
`.vm/bench/speedometer-20260923-232026-tiptap-inline-reserve-candidate/`,
and `.vm/bench/speedometer-20260923-232114-tiptap-inline-reserve-control-repeat/`).

### Reusing measured text widths

The archived TipTap profile assigned 8.65% of samples to rebuilding text items
from cached breaking positions. Disabling that cache increased isolated
20-iteration TipTap time from 149.8 to 161.6 ms; a repeated cached control
was 149.7 ms. The break-position cache is useful even though most expensive
inline roots are fresh
(`.vm/bench/speedometer-20260923-232953-tiptap-break-cache-control/`,
`.vm/bench/speedometer-20260923-233041-tiptap-break-cache-disabled/`, and
`.vm/bench/speedometer-20260923-233131-tiptap-break-cache-control-repeat/`).

`SUMMIT_TEXT_BREAK_STATS=1` showed 9,600 hits in 10,835 TipTap lookups, but
zero width-cache hits. All 9,600 hit text boxes lacked the simplified
measuring flag, while none had glyph overflow, content adjustment, letter or
word spacing, first-line font mismatch, or position-dependent content. The
Haiku width-cache rule now also accepts simple-font-code-path text when these
conditions hold, the text is not a synthesized glyph or combined text, and
it has no bidi override. Existing font-generation invalidation and the
breaking-position cache's text, context, origin, and font-cascade keys remain
in force. `SUMMIT_VERIFY_TEXT_WIDTH_CACHE=1` remeasures each cache hit and
reports mismatches; an eight-iteration TipTap run reused widths on 3,454 of
3,700 break-cache hits with no mismatch. A three-iteration full Speedometer
run also reported no mismatch across all suites
(`.vm/bench/speedometer-20260923-234457-tiptap-text-width-verify/` and
`.vm/bench/speedometer-20260923-234824-full-text-width-verify/`).

On the same trial bundle, uncontended isolated TipTap control/candidate/control
means were 147.3, 130.8, and 153.4 ms over 20 iterations each. Full
Speedometer at the Firefox-matched 1280×887 content viewport scored
5.891 ± 0.172 with the existing rule and 6.787 ± 0.286 with width reuse,
both over ten iterations on that bundle. The clean default bundle scored
6.923 ± 0.333 in ten iterations; it remains below Firefox's 8.338 ± 0.374.
Its TipTap mean was 148.2 ms versus 307.0 ms in the same-bundle control.
The isolated result is a more direct measure of the editor's cache benefit;
the full runs also reflect interactions among suites
(`.vm/bench/speedometer-20260923-234550-tiptap-text-width-control/`,
`.vm/bench/speedometer-20260923-234637-tiptap-text-width-candidate/`,
`.vm/bench/speedometer-20260923-234725-tiptap-text-width-control-repeat/`,
`.vm/bench/speedometer-20260923-235421-text-width-full-control-matched/`,
`.vm/bench/speedometer-20260923-235613-text-width-full-candidate-matched/`,
and `.vm/bench/speedometer-20260923-235224-text-width-default-matched/`).

On the packaged default bundle, a live `/r/popular/` 80-notch smoke delivered
53.68 native-view frames/s over 1.34 seconds, with a 16.5 ms pending frame
gap. Before and after screenshots show different, rendered posts. This short
burst does not establish sustained Reddit scrolling performance. The typed
Reddit HLS fixture loaded 720×1280 metadata, played to `ended` at 12.7 seconds
without a media error, and selected NVDEC H.264 plus AAC with status 0. The
decoder still logged its usual last-buffer warning at end of stream
(`.vm/bench/scroll-20260923-235918-text-width-reddit-smoke/` and
`.vm/bench/probe-20260924-000155-summit-text-width-hls-codec-smoke/`).

On the current September 24 diagnostic bundle, a fresh live `/r/popular/`
pass selected NVDEC H.264 for six videos. An HLS player with separate video
and audio decoded its first frame, but the scrolling pass moved past it before
five seconds of playback. A second 60-notch pass again selected NVDEC for six
live videos; its visible endpoint was an image post. These feed samples show
successful decoder selection and at least one decoded frame, not sustained
playback of a stationary Reddit post
(`.vm/bench/scroll-20260924-085039-reddit-live-media-current/` and
`.vm/bench/scroll-20260924-085150-reddit-live-video-stationary/`).
The same bundle completed the typed Reddit HLS fixture at 720×1280 through
its 12.7-second `ended` event with no media error. The backend decoded 381
frames using NVDEC H.264 plus AAC and reported five- and ten-second progress
(`.vm/bench/probe-20260924-085410-summit-reddit-hls-current/`).

The current diagnostic bundle also completed a fresh, uncontended ten-iteration
Speedometer 3.1 run at the Firefox-matched 1280×887 content viewport. It scored
6.760 ± 0.286 against the saved Firefox 155 result of 8.338 ± 0.374. Svelte,
CodeMirror, and Preact took 1.68×, 1.66×, and 1.66× Firefox's time. Preact's
`Adding100Items` and `CompletingAllItems` async steps took 30.0 and 24.5 ms
versus Firefox's 16.9 and 13.0 ms; Svelte's took 27.2 and 21.0 ms versus
14.4 and 10.4 ms. CodeMirror's `Long` step took 26.2 ms sync and 22.7 ms
async, versus 15.5 and 11.1 ms. This identifies the steps to investigate;
it does not by itself assign the delay to the frame scheduler, JavaScript,
or layout (`.vm/bench/speedometer-20260924-085646-current-diagnostics-matched/`).

A 20-iteration focused run of those three suites with
`SUMMIT_PAGE_UPDATE_TRACE=2` scored 15.080 ± 0.520 and recorded 22 page
updates over 33 ms. Their 850.9 ms of traced time included 519.1 ms inside
animation-frame callbacks and 293.1 ms in intersection observers; 40 layouts
crossed the separate 10 ms layout-trace threshold. These are aggregate page
logs, without a suite identifier, and the trace omits shorter updates. They
are useful leads but cannot establish which part of each measured async step
is spent in a callback or waiting for the next frame
(`.vm/bench/speedometer-20260924-085957-focused-page-update-trace/`).

Suite-only traced repeats clarified that attribution. Preact and Svelte each
had just one page update over 33 ms across 20 iterations; each also had 20
roughly 72 ms layouts, longer than a whole scored iteration and therefore
apparently in suite preparation outside its measured steps. CodeMirror had
21 slow page updates across 20 iterations, totaling 844.1 ms, including
519.7 ms in animation-frame callbacks and 302.6 ms in intersection-observer
processing. No CodeMirror layout crossed the 10 ms trace threshold. This
supports investigating CodeMirror's frame callback and observer work before
altering global frame pacing. The current opt-in `SUMMIT_PAGE_UPDATE_TRACE=2`
additionally splits intersection-observer update and notification time to
identify which part is expensive
(`.vm/bench/speedometer-20260924-090259-preact-page-update-trace/`,
`.vm/bench/speedometer-20260924-090531-svelte-page-update-trace/`, and
`.vm/bench/speedometer-20260924-090359-codemirror-page-update-trace/`).
For the next isolated CodeMirror diagnostic, `run-speedometer.py
--suites Editor-CodeMirror --intersection-trace` serves an instrumented iframe
that records JavaScript IntersectionObserver callback duration at unload in
`progress.jsonl`. The source benchmark checkout stays unchanged and the
result is marked instrumented. Comparing those callback times with the
native observer-phase trace should separate JavaScript notification work
from geometry updates before changing WebCore's observer path.

The first instrumented 20-iteration CodeMirror run uploaded one iframe record
per iteration. Its 60 constructed observers made 60 callbacks in total; the
callbacks accumulated 304 ms, while the browser's page-update trace recorded
305.0 ms in the intersection-observation phase. The third constructed
observer contributed almost all callback time, usually 14–15 ms per
iteration (22 ms at most). The two totals are independently measured and
close enough to make JavaScript callback work the leading explanation for
CodeMirror's observer-phase cost; optimizing native observer geometry alone
is unlikely to close this gap. The instrumented suite scored 13.471 ± 0.780,
similar to the earlier traced CodeMirror-only 13.447 ± 0.760, but instrumented
scores are diagnostic rather than benchmark baselines
(`.vm/bench/speedometer-20260924-092520-codemirror-observer-callbacks/`).
The CodeMirror bundle has observers for tooltips, the editor content, and
virtualized document gaps. Its gap-observer callback calls `onScrollChanged()`,
which can synchronously call `view.measure()`. The slow third observer is
consistent with that path, but the existing run did not label targets. The
opt-in trace now records a few observed target tags and classes so a follow-up
can establish that mapping before optimizing the measurement path.

### Reddit HLS seek range regression

On the current workstation bundle, `tools/bench/pages/media-seek.html` loaded a
12.7-second Reddit HLS clip but exposed an empty `video.seekable` range at
metadata, then `[0, currentTime]` while playing. Setting `currentTime = 8` at
metadata briefly reported 8 seconds, but playback started from zero
(`.vm/bench/probe-20260924-093312-summit-reddit-hls-seek-control/`).
WebCore's seek task aborts when `seekable` is empty. The Haiku backend reported
`maxTimeSeekable()` as `currentTime()` even though its finite HLS source has
been downloaded and its duration is known. Report the duration as the end of
the seekable range, as the other finite-file backends do. The first rebuilt
bundle exposed `[0, 12.7]` and fired `seeked`, but Media Kit returned the
preceding keyframe at 5.97 seconds for an 8-second seek
(`.vm/bench/probe-20260924-*/` with label `reddit-hls-seek-fixed`).

Keep the requested audio and video seek times separate, then decode video from
its keyframe to the requested time. The final workstation bundle
`bundle-1soo54tz` reached 8.0 seconds after decoding 61 preroll frames from
the 6-second keyframe, reached `ended` at 12.7 seconds, and selected NVDEC
H.264 plus AAC (`.vm/bench/probe-20260924-*/` with label
`reddit-hls-seek-preroll`). The test sets `currentTime` through HTMLMediaElement; the user's
original Adobe ad and its pointer interaction were not available to retest.

### Guardian article video check

The inline self-hosted clip on the reported Guardian article is 480×384 H.264
and AAC at 25 fps, 19.56 seconds long. The standalone page
`tools/bench/pages/media-guardian-loop.html` completed on both the previous
bundle and `bundle-1soo54tz`: each decoded 489 frames through NVDEC and had
no playback-clock stalls before the end. The new bundle releases the media
lock before the video thread waits for its next frame, so audio callbacks and
seeks no longer queue behind that intentional wait. This check does not prove
that every frame was presented smoothly.

On the full article, an idle five-second window with the inline video visible
reported 59.6 native-view frames/s with no interval over 33 ms. A cookie
consent layer covered the video in the screenshot, so this run cannot establish
visual playback quality (`.vm/bench/scroll-20260924-100927-guardian-article-video/`).
The standalone clip also stopped at 19.56 seconds despite its `loop` attribute.
The Haiku backend discarded its media tracks at end-of-stream, leaving no
tracks for WebCore's loop seek. Keep the tracks and mark each ended instead.
In `bundle-7dosgek7`, the clip reached 19.56 seconds, fired `seeked`, and
continued playing from zero without pausing; NVDEC remained selected
(`.vm/bench/probe-20260924-101812-summit-guardian-loop-candidate/`). The
same bundle still sought a non-looping Reddit HLS clip to exactly 8 seconds
and fired `ended` at 12.7 seconds
(`.vm/bench/probe-20260924-101909-summit-reddit-seek-loop-candidate/`).
The X399 Desktop launcher was initially moved to the tested `bundle-7dosgek7`.

### CodeMirror observer measurement comparison

The opt-in CodeMirror callback trace identified the third constructed
IntersectionObserver as the virtualized gap observer: its construction stack
points to CodeMirror's `DOMObserver` at asset line 8919. The tooltip observer
had no targets, the editor intersection observer had one fast callback per
iteration, and the gap observer ran twice per iteration. At the matched
1280×887 viewport, the gap callbacks accumulated 180 ms across ten Summit
iterations. A Firefox 155 run of the same instrumented suite and viewport
recorded 15 gap callbacks totaling 1 ms. These are diagnostic timings, not
standard Speedometer scores (`.vm/bench/speedometer-20260924-103121-codemirror-measure-matched/`
and `.vm/bench/firefox-speedometer-20260924-102344-codemirror-targets/`).
The construction stacks are in
`.vm/bench/speedometer-20260924-103457-codemirror-observer-origin/`.

Within Summit's 180 ms, `view.measure()` accounted for 179 ms. Instrumented
stages summed to 28 ms in `viewState.measure`, 10 ms in measurement reads,
20 ms in plugin updates, and 20 ms in document-view updates. A repeat that
also timed measurement writes found only 2 ms in those writes, with 198 ms
total callback time. The remaining time is spread through the rest of the
measurement loop, and timer resolution prevents exact subtraction. The gap
observer's synchronous CodeMirror measurement path is substantial
(`.vm/bench/speedometer-20260924-103246-codemirror-write-phase/`).

A later loop and timeline trace showed why the callbacks differ. Summit's
first gap callback starts before CodeMirror's initial animation-frame measure,
with `viewState.contentDOMHeight` still zero. It runs four measurement loops,
two updates, and one redraw. Firefox's first animation-frame measure runs
first; its subsequent gap callback finds a stable content height and runs one
loop without an update. The observer's entry geometry was effectively the
same in both browsers. See `.vm/bench/speedometer-20260924-104723-codemirror-schedule-order/`
and `.vm/bench/firefox-speedometer-20260924-104808-codemirror-schedule-order/`.

An opt-in diagnostic deferred only the CodeMirror gap callback to the next
animation frame. This eliminated its duplicate measurement (one loop, no
updates), but the ten-iteration suite mean rose to 186 ms. The initial layout
moved into a later frame inside the measured portion of the test. This is a
diagnostic score, not a standard Speedometer result, and does not support
changing observer dispatch as a performance fix
(`.vm/bench/speedometer-20260924-105328-codemirror-defer-gap/`).

### Guardian frame presentation diagnosis

The standalone Guardian playback probe now serializes
`getVideoPlaybackQuality()` fields explicitly and attempts
`requestVideoFrameCallback()`. On `bundle-7dosgek7`, the clip still reached
19.56 seconds and looped, but the quality fields stayed zero and the frame
callback API was unavailable. Neither browser API measures presentation on
this build. An opt-in backend trace records decode time, the delay before
the main-thread repaint request, and the age of the frame when `paint()` runs
(`.vm/bench/probe-20260924-105512-summit-guardian-frame-quality/`).

On the standalone clip, the trace logged 683 decoded frames and 681 painted
frames over the observed run. Median decode time was 2.3 ms, the repaint
callback's 95th-percentile queue delay was 0.2 ms, and median frame age at
paint was 15.4 ms. On the full article after dismissing the cookie overlay
and scrolling the inline video into view, NVDEC remained selected, but the
first 306 decoded frames produced only 235 paints. The repaint callback
waited over 40 ms on the main thread 43 times, with a 152.9 ms maximum.
Across a longer article window, the painted/decoded ratio stayed near 77%.
These observations point to page/main-thread contention, not slow NVDEC
decoding of the 480×384 clip. The article run was interactive and is not a
repeatable benchmark (`.vm/bench/probe-20260924-110000-summit-guardian-frame-handoff/`
and `.vm/bench/scroll-20260924-110339-guardian-interactive-frame-handoff/`).

An opt-in trial coalesced video repaint requests while one was waiting on
the main thread. The standalone clip still painted 681 of 683 frames and
looped. On the full article, callbacks fell from one per decode to 846 per
999 decoded frames, but only 764 frames painted and callback queue delays
still exceeded 40 ms frequently. This did not improve the observed choppiness,
so the trial was removed. Its diagnostic bundle was `bundle-eg5ddl7f`; the
production launcher was then moved to the tested trace-only
`bundle-2t4h8ozv`.

Further tracing on the full article with `SUMMIT_PAGE_UPDATE_TRACE=2` found
nine rendering updates over 33 ms during playback. Their initial-layout
phases totaled 702 ms of 731 ms. With trace level 1, more frequent layouts
were visible: 127 over 10 ms during an inline-video window, totaling 4.33 s;
4.26 s was render-tree work. They were full-tree layouts, usually around
34 ms, with no single renderer above 10 ms accounting for most of the cost.
When the video scrolled out of view and stopped, repeated layouts and frame
handoffs stopped as well
(`.vm/bench/scroll-20260924-111641-guardian-page-update/` and
`.vm/bench/scroll-20260924-111844-guardian-renderer-layout/`).

An opt-in `SUMMIT_LAYOUT_INVALIDATION_TRACE=1` now records the renderer and
element that requests each full layout. On the same article it showed a
positioned `span` changing style repeatedly; roughly every quarter second,
the root `html` renderer and a positioned link and span were invalidated
together. Those bursts coincide with video repaint queue waits above 40 ms.
The trace identifies renderers but does not identify the JavaScript call site
or prove which invalidation is necessary, so no layout behavior was changed
(`.vm/bench/scroll-20260924-112843-guardian-invalidation-source/`).
The current workstation launcher uses `bundle-3yuxtup5`, which adds only this
disabled-by-default diagnostic to the same playback code.

Document-aware tracing narrowed the recurring full layouts to the Guardian
article's main document. After video playback began, 139 layouts over 10 ms
totaled 4.85 s; each belonged to that document, and each coincided with
invalidation of its `html` renderer, skip link, and screen-reader span.
Earlier JavaScript stack traces showing PubMatic user-sync activity were from
other documents and did not explain these main-document layouts. A sampled
native stack instead shows WebCore applying a newly resolved style to the
`html` renderer and classifying the difference as requiring full layout.
This still does not establish which computed property changed, so layout
invalidation remains intact
(`.vm/bench/scroll-20260924-114522-guardian-document-layout/` and
`.vm/bench/scroll-20260924-115151-guardian-layout-native-stack/`).

Further opt-in style probes on the same inline video found that every
sampled root layout difference was in inherited font data. Line height,
letter spacing, word spacing, and the font description remained unchanged;
the cached font-set pointer changed. The document's font selector version
advanced by 41 between most root updates. A stack sampled during those
updates shows `CSSStyleSheet::insertRule()` repeatedly clearing and rebuilding
the document style resolver. This replays approximately 40 font-face rules,
advances the selector version, and creates a fresh font-set cache entry for
the root. The rule being inserted and whether these resolver rebuilds can be
handled incrementally remain under investigation
(`.vm/bench/scroll-20260924-131952-guardian-font-cache-entries/` and
`.vm/bench/scroll-20260924-132556-guardian-resolver-reset/`).

CSSOM tracing identified the repeated insertions as generated class rules
setting the video progress-bar width. The stylesheet sits in the middle of
the active author sheets. An opt-in trial rebuilt author rules in their
original order while preserving the unchanged font registrations. In a
same-bundle interactive comparison, decoded frames 101–300 produced 162/198
paints in the baseline and 195/200 in the trial. Repaint handoffs above
40 ms fell from 24 to zero, and their 95th percentile fell from 57.2 ms to
16.3 ms. The trial was restricted to the Guardian progress-bar rules and is
evidence for a general simple-rule insertion optimization; the runs are not
automated performance benchmarks
(`.vm/bench/scroll-20260924-135017-guardian-preserve-font-trial/` and
`.vm/bench/scroll-20260924-135201-guardian-preserve-font-baseline/`).

### General CSS rule insertion and Guardian playback

The production change handles a top-level ordinary `CSSStyleSheet::insertRule()`
when the document has an active, unshared resolver and no pending style update.
It rebuilds the author rule set in stylesheet order and invalidates style, while
keeping unchanged font-face and other resolver registrations. Nested, mutating,
and unsupported rule changes retain the existing resolver rebuild path. A CSSOM
probe verified insertion at the end of an earlier stylesheet, insertion in the
middle of a later stylesheet, ordinary width application, and deletion order
(`.vm/bench/probe-20260924-150230-summit-css-insert-order-egl/`).

On the reported Guardian article, three consecutive 200-frame decoded windows
painted 194, 193, and 196 frames. None had a repaint handoff over 40 ms;
their 95th percentile queue delays were 17.8, 15.4, and 11.8 ms. The earlier
baseline painted 162 of 198 frames in its first window, with 24 handoffs over
40 ms. These are interactive page traces, not an automated playback benchmark
(`.vm/bench/scroll-20260924-150322-guardian-general-css-insert/`).

The existing Reddit HLS fixture sought to 8 seconds, emitted `seeking` and
`seeked`, resumed playback, and finished its 12.7-second clip with no media
error. It tests programmatic seeking rather than dragging the Adobe ad's custom
progress control (`.vm/bench/probe-20260924-150450-summit-reddit-hls-seek-general-css/`).

A later interactive probe used an HTML range progress control with the same
Reddit HLS clip on the installed `bundle-wzdplo3u`. A VNC pointer drag moved
the range from 0 to 8.2 seconds, generated seven `input` events and the
corresponding `seeking`/`seeked` events, and ended with `video.currentTime` at
8.2 seconds and no media error. This verifies pointer delivery and repeated
seeks through a generic custom control; it cannot verify Adobe's ad player
without that ad's page URL
(`.vm/bench/probe-20260924-181521-summit-reddit-hls-pointer-drag-verified/`).

The first general implementation rebuilt the author rule set immediately on
every simple insertion. Its bundle accidentally used the cached `SkiaCG`
configuration with Haiku system malloc, which explained most of an apparent
absolute Speedometer drop. Paired ten-iteration runs on that configuration
scored 5.652 ± 0.211 for the candidate and 5.469 ± 0.257 for the installed
bundle at 1913×945; at 1280×887, they scored 5.645 ± 0.285 and
5.554 ± 0.254. The engine build wrapper now passes mimalloc defaults
explicitly so a reused cache cannot retain system malloc, and bundle manifests
record the actual allocator and graphics flags
(`.vm/bench/speedometer-20260924-150558-general-css-insert-matched/`,
`.vm/bench/speedometer-20260924-150830-css-insert-control-matched/`,
`.vm/bench/speedometer-20260924-151112-general-css-insert-1280x887/`, and
`.vm/bench/speedometer-20260924-151331-css-insert-control-1280x887/`).

The mimalloc rebuild exposed a cost hidden by the allocator mismatch: the
eager CSS path scored 6.334 ± 0.195 against 6.938 ± 0.292 for the older
mimalloc bundle at 1280×887. Disabling just the simple insertion path on the
new engine scored 6.813 ± 0.310. Consecutive simple insertions now schedule
one pending author rule set rebuild at WebKit's ordinary style flush. A later
active-sheet or non-simple content change upgrades that pending work to the
existing full rebuild. Forty insertions in one script task produced the correct
final width and preserved precedence from a later stylesheet. Adding a sheet
and deleting a rule during a pending insertion also produced the expected
computed widths. The batched
build scored 6.896 ± 0.293 in ten uncontended iterations at 1280×887
(`.vm/bench/speedometer-20260924-161217-guardian-css-mimalloc-1280x887/`,
`.vm/bench/speedometer-20260924-161437-guardian-css-mimalloc-control-1280x887/`,
`.vm/bench/speedometer-20260924-162121-simple-css-insert-disabled-1280x887/`,
`.vm/bench/probe-20260924-164559-summit-css-insert-mixed-mutations/`, and
`.vm/bench/speedometer-20260924-164011-batched-css-insert-mimalloc-1280x887/`).

On the full Guardian article with the batched mimalloc build, frames 101–700
painted 196/200, 196/200, and 194/200 across three windows, with one handoff
over 40 ms in all 600 frames and no long layout phase. The Reddit HLS fixture
still sought to 8 seconds, emitted `seeked`, and finished at 12.7 seconds
without a media error (`.vm/bench/scroll-20260924-164212-guardian-batched-css-mimalloc/`
and `.vm/bench/probe-20260924-164401-summit-reddit-hls-seek-batched-css/`).

On the 400-card scrolling fixture, the final bundle produced 58.88 fps with
an 18 ms 95th percentile frame interval and no interval over 33 ms. An older
mimalloc bundle in the same session produced 58.82 fps, also with no interval
over 33 ms. A live `/r/popular/` 80-notch pass delivered all 80 scroll events,
but Reddit returned a JavaScript challenge URL instead of the feed, so that
pass cannot verify live Reddit smoothness
(`.vm/bench/probe-20260924-164813-summit-scroll-batched-css-mimalloc/`,
`.vm/bench/probe-20260924-164904-summit-scroll-mimalloc-control/`, and
`.vm/bench/scroll-20260924-164723-reddit-batched-css-mimalloc/`).

### Speedometer asynchronous style-resolution trace

The same ten-iteration 1280×887 Speedometer run scored 6.896 ± 0.293 for
Summit and 8.338 ± 0.374 for Firefox. Preact and Svelte complex DOM suites
are among the largest gaps. An optional `--raf-phase-trace` mode now times
each benchmark step's animation-frame callbacks and zero-delay timer by
patching only the served runner response. It marks these runs as instrumented;
their scores are diagnostic and are not production benchmark results. The
generic 100-node mutation probe found a Summit/Firefox timer wait of 4/3 ms,
so the large suite-specific gap is not explained by timer dispatch alone
(`.vm/bench/probe-20260924-165402-summit-raf-timer-summit/` and
`.vm/bench/probe-20260924-165439-firefox-raf-timer-firefox/`).

In five focused iterations, Preact and Svelte add/complete steps spent
approximately 19–20 ms between the second animation callback and the timer
in Summit, versus 8–10 ms in Firefox. The delete steps spent 4–5 ms versus
3 ms. The native trace places the Summit timer wait inside WebKit's rendering
update, with roughly 13–15 ms in `Page::layoutIfNeeded()` after animation
callbacks for add/complete steps. Its `Document::updateLayout()` split shows
7–14 ms of that in `updateStyleIfNeeded()` and 1–8 ms in layout, with no
material compositing time. A deeper style trace of the steady 8–20 ms style
updates shows median 10.4 ms in `Style::TreeResolver::resolve()` and 3.2 ms
in render-tree commit. This points the next optimization work at style
resolution and its invalidation scope; Skia/GPU paint changes will not close
this measured Preact/Svelte gap
(`.vm/bench/speedometer-20260924-165801-raf-phases-summit/`,
`.vm/bench/firefox-speedometer-20260924-165857-raf-phases-firefox/`,
`.vm/bench/speedometer-20260924-172854-document-layout-phases/`, and
`.vm/bench/speedometer-20260924-173419-style-detail-phases/`).

The X399 workstation reports 32 logical CPUs (`sysinfo -cpu`). WebKit's
`WTF::numberOfProcessorCores()` already uses Haiku's online-processor count,
and its parallel work queue creates one fewer worker than that count.
`navigator.hardwareConcurrency` reports 8 because WebKit deliberately caps
the web-visible value for fingerprinting. The traced style resolution happens
on the page's main thread, so adding drawing workers alone does not divide
this particular 10 ms step across the workstation's cores.

### Preact/Svelte style-work split

A later opt-in diagnostic engine measured the native style tree on the same
five-iteration, pinned Preact/Svelte runs. Each slow complete-items update
restyled 422 elements. Preact visited 631 nodes and Svelte visited 1,038;
roughly 9.5 ms of the 10 ms tree-resolution phase was inside per-element
resolution, leaving about 0.5–0.7 ms in traversal. Across 100 changed
elements, the measured batches spent about 1.1 ms in style construction,
0.8–0.9 ms in animation/change handling, and 0.2 ms resolving pseudo-elements.
The rule-matching and property-application subparts of style construction
accounted for about 0.4–0.5 ms and 0.7–0.8 ms per 100 elements. Animation
handling had about 0.4–0.5 ms in transition checks and 0.2 ms applying
animations. These are diagnostic timings with instrumentation overhead, and
they identify no single large operation that can safely be skipped
(`.vm/bench/speedometer-20260924-174909-style-match-batch/`,
`.vm/bench/speedometer-20260924-175359-tree-resolve-detail/`,
`.vm/bench/speedometer-20260924-175852-element-resolve-detail/`, and
`.vm/bench/speedometer-20260924-180547-animated-phase-detail/`). The per-element
native hooks were removed after this measurement so they add no cost in
production.

The benchmark runners also accept `--source` for controlled diagnostic copies
of Speedometer. Removing the large shared stylesheet from Preact and Svelte
improved Summit's focused run, but increased Firefox's delete-step times
substantially because it changed page layout. That experiment does not isolate
selector matching or support a standard score comparison
(`.vm/bench/speedometer-20260924-174336-no-large-css/` and
`.vm/bench/firefox-speedometer-20260924-174422-no-large-css/`).

### Chart.js callback and Skia oval cost

Chart.js is the largest remaining single-suite gap in the latest standard
full run: 258.0 ms per iteration in Summit versus 189.1 ms in Firefox. In
matched eight-iteration focused traces, Summit's scatter draw callback took
96 ms versus Firefox's 75.5 ms, its opaque draw took 70.5 versus 58 ms, and
the tooltip's animation-frame work took 74 versus 56 ms. WebKit's page trace
placed the tooltip delay in `requestAnimationFrame` JavaScript; second layout,
intersection observers, and after-rendering work were effectively zero in
that update. The timer after the callback took about 1 ms. These runs are
instrumented and their focused scores are not standard Speedometer results
(`.vm/bench/speedometer-20260924-181934-chartjs-frame-phases/` and
`.vm/bench/firefox-speedometer-20260924-182102-chartjs-frame-phases/`).

A temporary native trace measured a median 27.6 ms per 5,000 Canvas `fill()`
calls in the same Chart.js suite. Damage notifications took about 0.5 ms and
the Skia drawing call about 26.4 ms. A second trace measured a median 22.3 ms
inside `SkCanvas::drawOval()` per 5,000 ovals. The oval batches also include
other Canvas path calls, so these figures are not an exact additive partition.
Per-call instrumentation increased the measured time and was removed after
collection. The result points at actual Skia oval rasterization and the
JavaScript work around drawing, rather than further canvas-damage coalescing
(`.vm/bench/speedometer-20260924-182602-chartjs-fill-native/` and
`.vm/bench/speedometer-20260924-183308-chartjs-oval-native/`).

### Opt-in Skia GL Canvas on Haiku

`SUMMIT_SKIA_GL_CONTEXT=1` now permits a Skia GL context on Haiku for Canvas.
The default remains CPU Canvas. Tile painting and tile sizing remain on the
CPU path even when the switch is set: the earlier all-GL tile experiment
produced Mesa Zink image-creation errors and did not improve scrolling.
The same bundle (`bundle-zk7kro3v`) was tested at a 1280×887 viewport with
ten standard Speedometer 3.1 iterations and no competing CPU load. With the
switch off it scored 6.730 ± 0.302; with the switch on it scored 7.178 ±
0.346. Chart.js fell from 277.9 to 150.3 ms per iteration and Perf Dashboard
from 296.1 to 205.3 ms. A Canvas probe with the switch on rendered 5,000
circles correctly and measured roughly 3 ms per fill batch, versus about
27.6 ms in the prior instrumented CPU trace. The full score remains below
the matched Firefox result of 8.338 ± 0.374
(`.vm/bench/speedometer-20260924-185726-skia-gl-canvas-cpu-tiles-control/`,
`.vm/bench/speedometer-20260924-185330-skia-gl-canvas-cpu-tiles-full/`, and
`.vm/bench/probe-20260924-184600-summit-skia-gl-canvas-paths/`).

On the 400-card scroll fixture at the same viewport, GL Canvas ran at
57.85 fps versus 59.02 fps with the switch off; neither run had a frame over
33 ms. The GL run still logged two `ZINK: vkCreateImage failed` messages.
For that reason the switch stays opt-in and is not in the workstation launcher.
The standalone Guardian clip visibly played with the switch on and off and
completed a loop without a media error. Its Web video-quality counters were
zero in both modes, so that probe cannot establish presentation frame rate
(`.vm/bench/probe-20260924-185540-summit-skia-gl-canvas-cpu-tiles-scroll/`,
`.vm/bench/probe-20260924-185637-summit-skia-gl-canvas-cpu-tiles-scroll-control/`,
`.vm/bench/probe-20260924-185938-summit-skia-gl-guardian-loop/`, and
`.vm/bench/probe-20260924-190056-summit-skia-gl-guardian-loop-control/`).

### Live Guardian playback on the installed build

The installed `bundle-zk7kro3v` was revisited on the reported Guardian
article. After dismissing consent and scrolling the inline video into view,
native frame tracing found 191–196 paints in five consecutive 200-frame
decoded windows. The latest window painted 196/200, with a 15.9 ms
95th-percentile repaint handoff and no handoff above 40 ms. One earlier
window had two handoffs above 40 ms, the worst 81.5 ms. A second article run
selected `H.264 on the graphics card (NVDEC)` with status 0 and painted
198/200 in its latest decoded window. The video was visible in the captured
screenshots. This confirms the current build's hardware decoder selection
and a much better paint ratio than the original roughly 77% trace; it does
not prove that every frame is presented on time
(`.vm/bench/scroll-20260924-190747-guardian-current-live/after-video.log`
and `.vm/bench/scroll-20260924-191305-guardian-current-codec/after-video.log`).

On the same live article with opt-in GL Canvas, three 200-frame windows
painted 191, 195, and 193 frames. NVDEC remained selected and the video was
visible, but Mesa again logged two Zink image-creation errors. The GL switch
did not improve this playback check, so it remains off in the workstation
launcher (`.vm/bench/scroll-20260924-191457-guardian-gl-live/after-video.log`).

### Matched-declarations cache check

The opt-in GL Canvas build improves Chart.js and Perf Dashboard beyond the
saved Firefox 155 times, but the remaining Speedometer gap spans TodoMVC
frameworks, CodeMirror, and Observable Plot. Against Firefox at 1280×887,
the GL Canvas full run was 39.4 ms slower in Observable Plot, 31.7 ms in
CodeMirror, and about 29 ms in each of Preact and Svelte. A broad style or
DOM improvement is needed to reach the Firefox score; another Canvas-only
change cannot close these gaps
(`.vm/bench/speedometer-20260924-185330-skia-gl-canvas-cpu-tiles-full/`
and `.vm/bench/firefox-speedometer-20260921-102926-ws/`).

A temporary matched-declarations cache trace counted 500 applications per
report, including setup and measured work. Across five focused iterations,
Preact and Svelte each reported 27,500 applications with usable cache entries
on 93.5% and 93.1%, respectively. Observable Plot reported 23,500 with
98.3% usable entries. CodeMirror reported only 2,000 applications with 62.1%
usable entries; its previously measured gap lies mainly in synchronous
editor measurement from an IntersectionObserver callback. A second temporary
Preact trace spent 0.010, 0.013, and 0.012 ms per 100 applications in cache
hashing, lookup, and cacheability checks. Timing hooks add overhead, but these
costs are far below the roughly 10 ms tree-resolution phase. Raising cache
hit rate or optimizing its lookup is therefore not the next Speedometer fix.
Both temporary traces were removed and the engine patch returned to its
committed digest
(`.vm/bench/speedometer-20260924-192514-preact-style-cache/`,
`.vm/bench/speedometer-20260924-192622-svelte-style-cache/`,
`.vm/bench/speedometer-20260924-193005-codemirror-style-cache/`,
`.vm/bench/speedometer-20260924-193110-observable-style-cache/`, and
`.vm/bench/speedometer-20260924-193712-preact-style-cache-timing/`).

### Accelerated Canvas cutoff and Guardian Zink errors

A temporary setting varied the minimum accelerated 2D Canvas area while
keeping the same GL-enabled bundle, viewport (1280×887), and ten focused
Speedometer 3.1 iterations. Chart.js / Perf Dashboard mean times were
148.3 / 203.3 ms at the WebKit default of 16,512 pixels, 152.1 / 195.6 ms
at 65,536 pixels, and 155.7 / 289.0 ms at 262,144 pixels. The high cutoff
removed most of the Dashboard GPU benefit; the middle cutoff retained it
(`.vm/bench/speedometer-20260924-194857-canvas-area-default/`,
`.vm/bench/speedometer-20260924-194950-canvas-area-65536/`, and
`.vm/bench/speedometer-20260924-195047-canvas-area-262144/`).

On the reported Guardian article, GL-enabled runs at 65,536, 262,144, and
1,000,000,000 pixels each logged two
`ZINK: vkCreateImage failed (VK_ERROR_UNKNOWN)` errors. A temporary stack
trace at Skia GL-context creation, with the one-billion-pixel cutoff, located
both first requests in `ScrollerCoordinated::updateValues()`, reached while
WebKit finalized rendering. Thus reducing accelerated Canvas use does not
address the observed Zink errors; coordinated scrollbar painting creates GL
contexts even without an accelerated Canvas on that page. The cutoff and
trace hooks were removed. GL Canvas remains opt-in pending a scrollbar/driver
fix (`.vm/bench/scroll-20260924-195146-guardian-canvas-area-65536/`,
`.vm/bench/scroll-20260924-195226-guardian-canvas-area-262144/`,
`.vm/bench/scroll-20260924-195341-guardian-canvas-area-high/`, and
`.vm/bench/scroll-20260924-195802-guardian-gl-callsite/browser.log`).

### Raster coordinated scrollbars on Haiku

Haiku's coordinated scrollbar painter now draws into a raster Skia surface and
passes its immutable image through the existing native-image layer upload.
The prior texture-mapper path requested a Skia GL context on every scrollbar
update, even with GL Canvas disabled, then returned without painting when
that context was unavailable. Raster painting removes that dependency and
makes the scrollbar visible with the normal launcher configuration. On the
reported Guardian article with GL Canvas enabled, the rebuilt candidate logged
no Zink image-creation error, versus two in the previous build
(`.vm/bench/scroll-20260924-200436-guardian-cpu-scrollbar/`).

Matched, uncontended 600-frame 400-card scroll probes at 1913×945 ran at
58.92 fps with raster scrollbars and 58.93 fps on the installed control, both
with zero frames over 33 ms. With GL Canvas enabled, the raster-scrollbar
candidate ran at 58.94 fps with zero frames over 33 ms and no Zink error
(`.vm/bench/probe-20260924-200814-summit-cpu-scrollbar-candidate/`,
`.vm/bench/probe-20260924-200859-summit-cpu-scrollbar-control/`, and
`.vm/bench/probe-20260924-201313-summit-cpu-scrollbar-gl-scroll/`).

At 1280×887, a full ten-iteration Speedometer 3.1 run with GL Canvas enabled
scored 7.288 ± 0.359, close to the previous 7.178 ± 0.346. It remains below
Firefox's 8.338 ± 0.374. At this stage the workstation launcher was moved to
`bundle-s8a06mrs` with GL Canvas still off, pending the live media check
(`.vm/bench/speedometer-20260924-201118-cpu-scrollbar-gl-full/`).

### Guardian playback after raster scrollbar fix

Matched standalone runs of the reported article's 480×384 H.264 clip used
NVDEC with status 0 and completed a loop without a media error. With GL
Canvas off and on, each decoded 683 frames and painted 681; both had a
roughly 15.8 ms 95th-percentile frame age at paint. Neither run logged a
Zink error (`.vm/bench/probe-20260924-201640-summit-guardian-cpu-scrollbar-gl-off/`
and `.vm/bench/probe-20260924-201734-summit-guardian-cpu-scrollbar-gl-on/`).

On the full reported Guardian article, after dismissing consent and bringing
the inline video into view, GL Canvas on painted 195 of the latest 200
decoded frames. GL Canvas off painted 196 of 200 in a separate article run.
Both selected NVDEC with status 0 and had no repaint queue delay over 40 ms
in those windows. The 95th-percentile queue delay was 17.4 ms with GL on and
15.2 ms with GL off. The video was visible in both screenshots. These native
traces count decode and paint, not exact presentation timing, and the live
page can change between runs. They do not show a material playback regression
from GL Canvas after the scrollbar fix
(`.vm/guardian-gl-raster-live-after-video.log`,
`.vm/guardian-cpu-raster-live-after-video.log`).

The workstation desktop launcher now sets `SUMMIT_SKIA_GL_CONTEXT=1` by
default to use the measured Canvas gain. `SUMMIT_SKIA_GL_CONTEXT=0` overrides
it for comparison. The underlying bundle is still `bundle-s8a06mrs`; no
engine rebuild was needed for this launcher change.

### GPU tile painting revisited after raster scrollbars

An opt-in Haiku trial enabled Skia GPU tile painting while retaining GL
Canvas and the new raster scrollbar. This isolates the previous scrollbar
GL-context failure from tile painting. On the 600-frame 400-card fixture,
two GPU paint workers reached 59.59 fps and one reached 59.68 fps, versus
58.94 fps for CPU tiles, with no frame over 33 ms in any run. Mesa still
logged two Zink image-creation errors with two GPU workers and one with a
single worker
(`.vm/bench/probe-20260924-203040-summit-gpu-tiles-raster-scroll/` and
`.vm/bench/probe-20260924-203339-summit-gpu-tiles-one-worker-scroll/`).

At 1280×887, ten uncontended Speedometer 3.1 iterations scored
7.172 ± 0.341 with two GPU paint workers and 7.096 ± 0.360 with one,
against the matched CPU-tile 7.288 ± 0.359. The confidence ranges overlap,
so this does not establish a precise slowdown, but it provides no benchmark
gain and the GPU runs logged five and three Zink errors, respectively. The
trial was removed and the workstation engine restored to the committed
CPU-tile path. The installed desktop launcher was unchanged
(`.vm/bench/speedometer-20260924-203129-gpu-tiles-raster-full/` and
`.vm/bench/speedometer-20260924-203438-gpu-tiles-one-worker-full/`).

### Zen 1 compiler target trial

The workstation's Threadripper 1950X has 16 Zen 1 cores and 32 hardware
threads. The installed engine uses release `-O3` without a CPU-specific
target. An isolated `SkiaCGMiZen1` engine was built with
`-march=znver1` and the same Skia, coordinated graphics, asynchronous
scrolling, and mimalloc options as the installed engine. The build uses its
own directory and did not change the installed launcher. The build finished
successfully with release `-O3`; its separate bundle is `bundle-o19kil6q`.
At the matched 1280×887 viewport, its uncontended ten-iteration Speedometer
3.1 run scored 7.178 ± 0.335 versus 7.288 ± 0.359 for the installed build.
The 600-frame 400-card scroll probe ran at 58.79 fps with no frame over
33 ms, versus 58.94 fps for the installed build. Both differences are small
and provide no evidence of a CPU-target gain, so the workstation launcher
stays on `bundle-s8a06mrs` (`.vm/zen1-engine-build.log`,
`.vm/bench/speedometer-20260924-220312-zen1-full/`, and
`.vm/bench/probe-20260924-220530-summit-zen1-scroll/`).

### Repeated video seeking under build load

`media-seek-sweep.html` now makes five rapid forward/backward `currentTime`
changes, like a custom video progress control. On the installed workstation
bundle, both the reported Guardian clip and the Reddit HLS fixture loaded and
reached the final seek target without a media error. The Guardian clip's
three completed seeks decoded 74, 116, and 43 preroll frames, respectively;
the Reddit fixture decoded 58, 14, and 27. Several intermediate requests
were coalesced. JS property setters returned immediately, while seek events
arrived roughly 100–440 ms later. These runs overlapped 23–24 active compiler
teams and are explicitly marked contended
(`.vm/bench/probe-20260924-213829-summit-guardian-seek-sweep-build-load/`
and `.vm/bench/probe-20260924-213907-summit-reddit-seek-sweep-build-load/`).

The same two remote sources stalled before metadata in the VM. Those probe
files contain no seek and cannot validate the seek path there
(`.vm/bench/probe-20260924-213630-summit-reddit-seek-sweep-vm/` and
`.vm/bench/probe-20260924-213707-summit-guardian-seek-sweep-vm/`).

After the build stopped, the same probe on both the Zen 1 and installed
bundles used NVDEC H.264 with status 0 and reached the final Guardian seek
target without a media error. It also recorded page-thread heartbeat gaps
of 210–265 ms in each run, coinciding with synchronous preroll of up to
119 frames. The installed bundle's Reddit HLS run likewise used NVDEC,
reached its final target, and recorded a 265 ms gap while prerolling 58
frames for the first seek. This provides an engine-level cause for sluggish
progress-bar dragging, although it does not identify the Adobe ad player's
own behavior. This measurement led to moving preroll off the page thread
while keeping the seek target and paused-frame behavior correct
(`.vm/bench/probe-20260924-220617-summit-guardian-seek-sweep-zen1/`,
`.vm/bench/probe-20260924-220651-summit-guardian-seek-sweep-control/`, and
`.vm/bench/probe-20260924-220738-summit-reddit-seek-sweep-control/`).

### Asynchronous Media Kit seek preroll

The Haiku media backend now seeks tracks on the page thread, then decodes
video keyframe preroll on its existing decoder thread. It takes and releases
the media lock for each decoded frame, allowing a new drag position to cancel
an old seek. The seek promise settles when the requested frame is ready;
paused video also runs this preroll and repaints that frame. Load cancellation
rejects a pending promise, and a seek generation prevents a stale completion
from settling a newer request.

On the idle workstation, the repeated-seek probe recorded **no heartbeat gap
over 80 ms** with the new bundle on either the Guardian or Reddit clip. The
prior build recorded gaps of 210–265 ms on Guardian and up to 265 ms on
Reddit. Both new runs used NVDEC H.264 with status 0, reached their
last target, and reported no media error. A separate paused Guardian run
remained paused at 13.692 seconds, emitted `seeked`, had no gap over 80 ms,
and showed the sought frame in its screenshot. A 22-second Guardian playback
probe completed a loop with 684 decoded frames, 682 painted frames, no media
error, and no Zink error. These are controlled clips; the Adobe ad player
itself remains unverified because its URL is unavailable. The workstation
launcher now points to the verified `bundle-w9ti9d76`; its uncontended
Speedometer result is 7.298 ± 0.333, and the 600-frame scroll probe is
58.88 fps with no frame over 33 ms. The media change did not measurably
change these two performance results. Evidence:
(`.vm/bench/probe-20260924-221508-summit-guardian-seek-sweep-async/`,
`.vm/bench/probe-20260924-221557-summit-reddit-seek-sweep-async/`,
`.vm/bench/probe-20260924-221654-summit-guardian-paused-seek-async/`, and
`.vm/bench/probe-20260924-221753-summit-guardian-loop-async/`).

### Preact and Svelte style invalidation scope

An opt-in diagnostic counted why elements entered style resolution in focused
Preact and Svelte complex DOM runs. Each suite repeated three large passes per
iteration. The largest resolved 3,672 elements and took about 34–36 ms after
warmup; 3,666 elements were still individually marked valid, but six
subtree-invalid roots caused full descendant resolution. The benchmark page's
`html.spectrum` root was among those invalidated roots. The add-items pass
resolved about 515 elements in 4–5 ms, nearly all newly subtree-invalid.
The complete-items pass resolved 422 elements in 10–11 ms: 181 were directly
invalid, 240 were individually valid but reached through parent changes, and
80 of those used WebKit's fast inheritance path. The pattern was stable across
both frameworks and repeated iterations. These counts explain why adding
tile paint workers cannot divide this main-thread style cost; they do not
prove that any particular descendant can be skipped. The diagnostic hooks
were removed and the production engine source rebuilt from the committed
patch (`.vm/bench/speedometer-20260924-223104-style-reasons/`,
`.vm/bench/speedometer-20260924-223632-style-reasons-url/`,
`.vm/bench/speedometer-20260924-224117-style-root/`, and
`.vm/style-reason-restore-build.log`).

### Current live Reddit scroll check

The installed async-seek bundle loaded a visible `/r/popular/` feed. An
80-notch, 25-ms wheel burst moved it at 57.22 native-view frames/s with a
47.3 ms worst frame interval. A 300-notch burst delivered all 300 wheel
events but recorded only 16.26 frames/s and a 5,719 ms presentation gap;
the before/after screenshots still showed feed posts. A second 300-notch
run with page-update and media-lifecycle traces reached 23.47 frames/s and
had a 4,519 ms worst interval. In that trace, one bubbling JavaScript scroll
listener occupied 2,140.6 ms. Across the run, 32 slow layouts totaled
5,368.2 ms, including 4,579.8 ms in render-tree layout and 641.7 ms in
post-layout tasks; these totals include layouts nested in the listener and
must not be added to its time. Media cancellation accounted for only 12.6
ms. Native frame queue delay stayed below 0.3 ms in both long runs. The tab
URL carried Reddit's `js_challenge=1` query even while the feed was visible,
so site challenge activity may have affected these measurements. The current
bottleneck is still synchronous page work and layout, not wheel delivery or
the native view queue (`.vm/bench/scroll-20260924-224550-reddit-current-live/`,
`.vm/bench/scroll-20260924-224651-reddit-current-long/`, and
`.vm/bench/scroll-20260924-224809-reddit-current-long-trace/`).

### Profile-guided build trial

GCC 13.3 on the X399 workstation successfully compiled a small C++ program
with `-fprofile-generate`, wrote its `.gcda` file on normal exit, and then
accepted that profile with `-fprofile-use -fprofile-correction` at the same
output path. The compiler reports `single` as its default profile-counter
update mode, so the browser build explicitly uses `-fprofile-update=atomic`
for its multiple threads. The initial non-atomic build was stopped before
profiling, and the isolated `SkiaCGMiPGO` build is being rebuilt with that
flag. Training will include Speedometer 3.1 and the 400-card scroll fixture.
The optimized pass will use `-fprofile-partial-training`, which tells
[GCC 13.3](https://gcc.gnu.org/onlinedocs/gcc-13.3.0/gcc.pdf) to optimize
untrained functions normally instead of favoring size. This matters because
two workloads cannot cover every browser path. The installed launcher stays
unchanged until a matching full benchmark and scroll probe show a gain; the
earlier Zen 1 target alone did not.

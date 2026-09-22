# Summit performance: Speedometer 3.1 baseline, where the time goes, stress test

## Current coordinated Skia result (September 22, 2026)

The workstation's coordinated graphics build now completes all 580 steps of
Speedometer 3.1 after fixing a lock inversion in `BitmapTexturePool`.
`acquireTexture()` held the pool lock while querying a timer on the main Haiku
`BLooper`; a firing timer held that looper lock while waiting for the pool lock.
Timer operations are now dispatched to their owning run loop. The fix is in
`7340592`; the ten-iteration run is recorded in
`.vm/bench/speedometer-20260922-215228-pool-timer-clean/`.

| Browser | Speedometer 3.1, 10 iterations, 1913x935 |
| --- | ---: |
| Summit, Skia + coordinated graphics | **5.067 ± 0.215** |
| Firefox 155 on the same workstation | **8.338 ± 0.374** |

The Summit run was uncontended (at most 0.05 foreign CPU cores). A single
diagnostic iteration scored 3.405 because its cold first iteration is slower;
the ten individual scores rose from 4.35 to 5.34. The new score is about 54%
higher than the previous 3.28 Summit result, although that result used the old
app_server rendering path. The gap to Firefox remains 1.65x. Most of the gap is
in suites such as Editor-TipTap and Charts-chartjs; TodoMVC-jQuery is level
with Firefox. `tools/bench/compare-runs.py` gives the per-suite sync and async
split from the saved results.

The workstation reports 32 logical processors (16 physical cores) through
both Haiku's system information and `sysconf`. WebKit's Skia CPU painter uses
its upstream default of eight workers here: half the reported cores, capped
at eight. The frame rate on the 400-card scrolling probe was 60.69 fps over
600 frames after the media fixes, with p95 18 ms, p99 19 ms, a 22 ms maximum,
and no frame above 33 ms
(`.vm/bench/probe-20260922-212230-summit-media-fix-scroll/`). This establishes
smooth scrolling on that synthetic feed; real Reddit scrolling still needs a
direct measurement.

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

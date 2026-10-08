# October 7 issue work

## Follow-up issues #43–#49

Seven additional issues appeared after the original deployment. #43 and #44
are verified in `bundle-a7988kyv`; #49 is verified in `bundle-a3u9gkoe`.
#45 is verified in `bundle-3ulccsni`; #46 is verified in `bundle-zf9_3v7d`.
#48 is verified in `bundle-ralgs2gr`, which was installed as the combined
issue-fix deployment. All issues are closed. The normal desktop launcher passed
all 48 OffscreenCanvas/image-codec checks, loaded its application, helpers,
WebKit and JavaScriptCore from that bundle, and quit without crashes or helpers
left behind. Its screenshot was inspected. The user's browser and other apps
were preserved during that installation. Evidence is in `.vm/issues43-49/{install.log,
installed-results.json,installed.png}`.

The requested six-hour performance session started afterward, at 02:14:10 UTC
on October 8 (13:14 Hobart). Excluding 1,275 seconds of workstation outage and
user-requested pause moved its minimum finish to 08:35:25 UTC (19:35 Hobart).
Its measurements and changes are recorded in [performance notes](performance.md).

The retained optimizations are now installed in `bundle-tmeeg8cb`, built from
`b4006ff` with engine patch `b178f34d…`. The normal desktop launcher again passed
all 48 checks, with the expected application/helper/library paths and clean
teardown. The graphics restoration regression passed another 68 checks.
Evidence: `.vm/optimization-2026-10-08/final-installed-1791444384/` and
`.vm/bench/presentation-lifetime-20261008-182633-opt1008-final-retained/`.

The performance session met its six-hour minimum at 08:35:25 UTC, excluding
the outage/pause. Final verification also passed 1,200 stress actions and the
56-case streaming suite (1,692 DOM, 50 PCM seek and five video-timing checks),
with no crashes or helpers left behind. Repeated media testing exposed a
recurring Haiku HD Audio service stall; recovery and its unresolved status are
documented in the performance notes. The final media pass used restored
services and does not establish that the platform stall is fixed.

**#47 is complete:** MediaSource accepts MP4, WebM, raw ADTS AAC, MPEG audio
and MPEG-TS with the codec matrix in [streaming media](streaming-media.md).
Transport validation, clock rollover and discontinuity handling preserve
existing NVDEC H.264 selection. Final parser samples drain before playback
ends, and settings changed during finalization apply to the following operation.

The verified bundle is `bundle-9_wz1wsy`, engine patch
`fdb082ac31077936391914e40fa2ad688d20fc49c2182c4345c6ceee8c852f32`.
All 56 streaming scenarios pass (1,692 DOM checks), with 50 PCM seek checks,
five video-timing checks and exact Opus trimming. AAC (252 DOM checks),
Clear Key (207) and DASH (82) regressions pass on the same bundle. All five
host suites pass. Tests leave no new crash reports or helper processes.

Portable TS checks cover 10 fixtures, 988 packets at nine append sizes,
26 structural cases and 128 sanitizer mutations. The native TS probe passes
21 checks across 12 files. Earlier broad codec checks pass 43 checks across
29 WebM/raw-audio/MP4 files; the final browser suite repeats their playback
cases. Evidence: `.vm/issues43-49/{streaming-verified-x399,ts-regressions,
mpegts-parser-complete,ts-native-complete,streaming-codecs-timing-x399}/`.

- **#43:** Fullscreen uses air/OS's `get_display_frame(Frame(), true, ...)`,
  the same display-selection policy as native window maximization. On older
  Haiku versions without that symbol it falls back to `BScreen::Frame()`.
  On X399's two displays, all 49 native checks pass: fullscreen on each display
  with the setting enabled, the combined desktop with it disabled, and exact
  frame/DOM restoration on Escape. The original enabled setting was restored.
- **#44:** The certificate button is now a child of the address control, on
  the left inside its border. Address text reserves space for the lock and
  the reader button independently, including when either disappears.
  Certificate checks (72) and reader checks (79) pass on X399, and the native
  screenshot confirms the lock's placement. Tests quit their own browser
  instances without leaving helpers or new crash reports.
- **#45:** Added Media Kit-backed `audio/aac`, `audio/x-aac`, `audio/m4a`
  and `audio/x-m4a` types for AAC-LC and MPEG-2 AAC-LC (`mp4a.67`). Native
  decoding already existed, but MIME aliases and audio-only timing were
  broken. FFmpeg's stream timestamps made half a second of decoded MP4 AAC
  appear as 0.04 seconds and ADTS AAC as 13.9 seconds. The player now uses
  decoded PCM frame counts for its audio clock and resume synchronization.
  It also accepts raw PCM tracks, which the track selector previously dropped.
  Nine AAC source/container combinations plus MP3, Vorbis and WAV pass all
  252 DOM checks on X399: actual source selection, smooth timing, audio enabled,
  pause, seek, ended events and unsupported-codec rejection. The native
  driver exits normally with no helpers or new crashes. The fixtures disable
  MPEG-4 PNS when generating MPEG-2-compatible LC samples.
  Engine patch SHA-256:
  `31be58cf6bae4fb0c138df8a772a4bc80c9a98ee3ee5f96594f6b0af43c08fff`.
- **#46:** Fixed two independent post-paste delays. Parsing pasted content can
  create an `EmptyDisplayRefreshMonitor`; its rejected callback request left
  the live page marked as scheduled even after its fallback timer fired.
  Later frames then waited for the two-second liveness watchdog. Failed
  requests now clear that flag. Caret lookup also repeatedly scanned all
  preceding line boxes in large fields. A sparse index owned and invalidated
  by the layout cache now starts those searches near the requested offset;
  visually reordered text keeps the existing traversal. Marker-free edits
  skip the unnecessary preceding-character lookup.
  All 1,253 native checks pass for actual clipboard paste, two typing phases
  at 30 ms per key, caret scrolling, exact text/selection, reflow, Unicode,
  bidirectional text and contenteditable fields. The full Summit window also
  passes all six paste/typing phases. At 1 MB, final steady-state p95 key/frame
  delays are 1/17 ms in Summit (3/20 ms in the isolated view), versus roughly
  5–6 seconds of sustained input backlog before the fix. The initial 1 MB
  paste still costs about 350 ms; 1 KB and 100 KB fields have 1–2 ms p95 key
  delay. Every key reaches a frame, both processes exit cleanly, and final
  runs have no new crashes or remaining helpers. All five host suites pass.
  Engine patch SHA-256:
  `86392baaf25d333f7a9d5aa4d6262da5e62aa72d82ef9cf9159698798c8634c4`.
- **#48:** Implemented canvas hit regions with path snapshots, transforms,
  clipping, overlap/replacement, partial clearing and native `MouseEvent.region`.
  All 54 native click sequences pass 560 DOM checks. Twenty inspector checks
  confirm destruction of 40 canvases with self, ancestor, descendant and mutual
  control references, valid drawing recordings and normal teardown. Control
  references are weak to avoid retaining the canvas's DOM subtree.
  Platform pages pass all 120 checks; grid/reflow geometry, WebGL pixels and
  animation, and canvas path/damage pixels also pass. Tests leave no helpers,
  new crashes or crash-related syslog events. [Details and reproduction](canvas-hit-regions.md).
- **#49:** Native pages enable `rel=prefetch` by default, with
  `SUMMIT_PREFETCH=0` and extension privacy overrides honored. Link completion
  delivers load/error events, HTTP error responses complete the fetch, and
  the temporary cross-origin cache respects `Cache-Control: no-store`.
  All 96 native checks in `tools/bench/test-prefetch.py` pass: actual requests,
  same/cross-origin cache reuse, no-store reloads, dynamic href, CSP, transport
  and HTTP failures, default credentials, disabling and process cleanup.
  The extension privacy regression passes five HTTPS rounds and all twelve
  API commands, with no new crash reports or leftover helpers. Its test
  accepts both null (WebKit) and undefined (Chromium) for an absent lastError.
  Engine patch SHA-256:
  `11d81674a41dbd78749a5e36a6a5d2f804e75a3b85538bdcffd217432d37b614`.

Evidence: `.vm/issues43-49/{certificates,reader}.json`,
`fullscreen-checks.json`, `fullscreen.log`,
`prefetch-final-credentials/results.json`, `privacy-results.json` and
`.vm/issue41-viewer.png`. Audio evidence is in `aac-results.json`,
`aac-native.log`, `aac-browser.log`, `aac-media/result-*.json` and
`aac-clock.log`. Reproduce with `tools/bench/aac-fixture.py` and the native
`tests/ModernAACTests.cpp` driver, passing an owned browser team, its exact
executable path and the fixture's base URL. Typing evidence is in
`typing-final/{result.json,native.log}`, `typing-browser-final.json`,
`typing-browser-final-native.log`, `typing-refresh-before-fix.log` and
`host-tests.log`. Reproduce the isolated typing checks with
`SUMMIT_BENCH_HOST=workstation python3 tools/bench/test-typing.py --bundle BUNDLE`.
The typing fixtures reproduce the native paste failure locally; they do not
submit content to GitHub or automate the user's signed-in session.

## Original issues #31–#42

The requested scope was every open issue in `jmgasper/summit` (#31–#42),
followed by a verified installation on X399. Implementation and verification
are complete. That deployment selected `bundle-4bl4v7jv`.

| Issue | Current evidence / remaining work |
| --- | --- |
| #39 New icons | Supplied toolbar SVGs render at three densities; the app uses the updated mountain-and-flag HVIF. Native bundle `bundle-uofxmys5` builds. Screenshot `.vm/issue39-native.png` verifies the toolbar in the preceding icon bundle. Commit `a2a9512` is pushed. |
| #40 Extension stores | Implemented and verified against both live stores; [workflow and native evidence](extension-stores.md). |
| #41 Certificate information | Implemented: supplied lock icon and native details from the committed connection. Verified/public and exception states, tab/navigation cleanup, and resumed TLS peer certificate retention pass 72 native checks; [details](certificate-warnings.md). |
| #38 PDF viewing | Implemented with bundled PDF.js, original-response downloads, navigation/search/zoom, passwords, and error handling. 118 native checks pass; [details](pdf-viewer.md). |
| #37 README | Updated for air/OS, the current icon and verified features, with a latest-release download link, capability limits and current build instructions. The CI-maintained release table is preserved. |
| #36 Date/time input types | Implemented for all five types with BCalendar and canonical editable values. 171 native checks pass, including leap dates, ISO weeks, range/step constraints, events and cleanup; [details](native-form-pickers.md). |
| #35 Color input | Implemented with BColorControl, Done/Cancel, script updates and datalist swatches. 65 native checks pass, including DOM events and navigation/tab/quit cleanup; [details](native-form-pickers.md). |
| #34 Reader mode | Implemented: address-field button, isolated Readability extraction, DOMPurify/CSP, in-memory article view, original-URL history and private browsing. 79 native checks pass; [details](reader-mode.md). |
| #33 DASH | Implemented direct H.264/AAC DASH with adaptive quality, byte ranges, seeking and live refresh. 82 native checks and portable ASan/UBSan tests pass; [scope and evidence](dash-playback.md). |
| #32 EME | Implemented temporary Clear Key CENC for H.264/AAC MSE and direct DASH, secure-context and permissions-policy checks, key lifecycle and media-origin protections. 207 native checks and portable ASan/UBSan tests pass; [scope and evidence](encrypted-media.md). |
| #31 Web Bluetooth | Implemented: 213 browser API checks, 34 full-app picker/GATT checks and 17 isolated native picker checks pass. Portable ATT/GATT, native scans and the physical busy-device test pass. Clear Key (207) and DASH (82) regressions pass on the same bundle. The user requested that the ProtoArc mouse remain connected; successful physical GATT reads are deferred. [Scope and evidence](bluetooth-transport.md). |
| #42 Latest WebKit | Rebased, compiled, tested and installed the October 7 upstream revision `fb054d09146b113aeeabee5f67f4c8b6d9379809` (06:34:32 UTC). Native graphics/grid integration and extension API adaptations are verified below. |

The installed bundle uses Mesa `prefix-20261002`; its engine patch SHA-256 is
`abba809b9d133d115b0287bcaaab7ca49f00795ec3c6fa618f78fa26e833e494`.
It contains application source from `5b344c3`; subsequent changes update tests
and documentation only. The previous desktop launcher is backed up under
`/boot/home/summit/launcher-backups`, and previous bundles remain available.

Native browser build: `SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_REMOTE_TAG=ws
SUMMIT_ENGINE_BUILD_NAME=SkiaCGMiPGO bash tools/build-modern-browser-in-vm.sh
--browser --bundle --modern-extensions`. Engine build flags are recorded in
`.vm/issue42/build.sh`; inspect live processes before starting another
engine build, and preserve those PGO/Skia settings for incremental work.

The October 7 engine has compiled and linked JavaScriptCore, WebCore, WebKit,
Skia, libwebrtc and the browser helper processes. Native adaptations
cover typed strings, image sizes, cancellable timers, frame registry lookup,
picker callbacks and extension event/string ownership changes. Four inherited
PGO data files reproducibly crashed GCC; the same objects compiled successfully
with those files saved aside and all source/flags unchanged. The original files
remain backed up.

## October engine verification

| Area | Result |
| --- | --- |
| Bluetooth | 213 API and 34 full-browser checks pass on the final bundle using the explicit simulated ATT backend. The earlier physical ProtoArc busy-device check passes with its input link preserved. Successful physical GATT reads remain deferred at the user's request. |
| Media | Clear Key 207 and DASH 82 checks pass on the final bundle. |
| Native features | Reader 79, color picker 65, date/time pickers 171, PDF API 86, full-browser PDF 32 and TLS/certificate details 72 checks pass on the final bundle. |
| Extension scripting | 585 checks pass on the final bundle, including frame targeting, navigation identity, serialization and teardown. |
| Extension icons | 51 cases and 57 observations pass at both 1× and 2× display density, 538 native checks per run. Both SDK image sizes and actual desktop pixels are checked. |
| Published extensions | uBlock Origin passes 90 installation plus 7 restart checks; Dark Reader passes 79 checks, including actual page colors and enable/disable behavior. Both use the final bundle. |
| JavaScriptCore | 31 JIT/interpreter/WebAssembly smoke checks pass against the updated native engine. The historical full Test262 corpus has not been rerun for this pin. |
| Listener persistence | 46 checks pass with declarative request rules disabled and enabled, 92 total. |
| Platform pages | OffscreenCanvas 28, image codecs 20, WebUSB 54 and protocol handlers 18 checks pass on the updated engine. |
| Layout and graphics | All 11 grid geometry values and initial/final/all 20 reflow samples match the preceding engine. WebGL 1/2 render the expected pixels with no GL errors; the animated scene renders successfully. Timing runs overlapped compilation, so they are not clean performance comparisons. |

The platform/layout runs used `bundle-hm0nz25k`, before the final isolated
extension ImageData encoding fix. The final bundle repeats the feature and
extension checks above; its normal installed launcher additionally passes all
48 OffscreenCanvas/image-codec checks. Test processes exit cleanly without new
crash reports or leftover helpers.

Icon verification found and fixed intrinsic SVG scaling, full-source bitmap
resizing, and avoidable precision loss when encoding straight-alpha ImageData.
The test fixture now measures spec-permitted canvas rounding separately from
icon decoding, samples each row of the native button's gradient for alpha
compositing, and uses a PNG with known source pixels for embedded-image tests.
SDK and desktop pixel tolerances were retained. Stale expectations were aligned
with the September extension compatibility fixes: size dictionaries use own
properties, and ImageData takes precedence when both icon sources are supplied.

## Installed launcher verification

Installation used `SUMMIT_MESA_PREFIX=prefix-20261002 bash
tools/install-on-workstation.sh bundle-4bl4v7jv`. The actual
`/boot/home/Desktop/Summit-current.sh` was launched with an isolated profile and
the normal environment, without simulated Bluetooth or input-synthesis flags.
The installed link, application/helper executable paths, and loaded WebKit and
JavaScriptCore library paths all select the final bundle. The image-codec page
was visually inspected after painting. The owned browser group quit normally;
Clipper, AirTop, Bluetooth preferences and the mouse connection were preserved.

Local evidence is retained in `.vm/issue42/`: `native-regressions.json`,
`extension-tail.json`, `jsc-results.json`, `listener-results.json`,
`platform-hm0-results.json`, `graphics-hm0-results.json`, `install.log`,
`installed-results.json` and `installed.png`. The final PDF browser result is
`.vm/issue38-browser-results.json`; icon and script-injection reports are in
the corresponding `.vm/modern-extension-*` directories.

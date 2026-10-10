# WX 5100 media decoding

The port patch explicitly selects the `amduvd` Media Kit add-on on x86-64 for
progressive, eight-bit, 4:2:0 H.264 Baseline/Main/High streams within its size and
reference limits. The add-on performs the complete bitstream/device validation.
It registers no formats with Media Kit, leaving software decoding available.
`SUMMIT_HARDWARE_VIDEO=0` disables named hardware selection.

AMD supplies RGB32 using each picture's declared color range and matrix. The
Raspberry Pi retains its existing I420 path. Timeout and slow-close state is
tracked independently for each backend. Negotiated dimensions and row sizes are
checked before allocating the output buffer.

File playback recovers from a hardware decode error by seeking the extractor to
the keyframe preceding the next output frame, initializing software decoding,
and discarding pictures already returned. The replacement format must fit the
original bitmap. MSE retains compressed samples back to a keyframe, preserving
the preceding GOP while reordered output is pending. It replays those samples
through software and suppresses already consumed timestamps. The cache switches
to software at 512 samples or 64 MiB, retaining at most one additional demuxed
packet needed for recovery. Cancellation, interruption and normal EOF remain
separate from decoder failure.

## Native qualification, 11 October 2026

Focused tests compiled the actual amended `MediaDecoderSelectionHaiku.cpp` and
linked to the idle reference engine's WTF/Media Kit dependencies on X399. These
are decoder tests, not yet full-browser qualification. Both amended decoder
translation units pass native syntax checks. The complete private engine build and final incremental update now pass under
`/boot/home/wx5100/summit-webkit/WebKitBuild/WX5100`, with port patch SHA-256
`1a2476f9afeeaf0a50e335c52bb8f5af0070a3f65a6a24079e9ab7e344c1ac0e`.
The resulting private browser bundle is `bundle-6mitguy5`.

The focused native results are:

- A six-second 320×240 H.264 clip returns all 144 pictures through AMD UVD.
- Forced failures after 17, 47, 49 and 141 output pictures each resume in
  software and return all 144 pictures, with no duplicate or missing pictures.
  Output guards, monotonic track time, decoder identity and EOF checks pass.
- All hardware RGB pixels match independently decoded YUV and floating-point
  color conversion within one channel value of fixed-point rounding. Every
  software suffix matches FFmpeg RGB output exactly, including frame order.
- High 10 returns all 50 pictures in software; disabling hardware returns all
  144 pictures in software. A 2.75 s seek reaches the 2.5 s keyframe and returns
  all 84 remaining hardware pictures, with complete RGB/order comparison.
- Four additional 24-frame clips declare BT.601/BT.709 and limited/full range.
  All 96 pictures pass the same complete RGB comparison with maximum error one.

The production add-on was restored and its SHA-256 verified after fault tests:
`3f28945cffd89cad4cc5f093088c97b3e175b13355ffb63d1c9392d49728a9e6`.
The driver remains the qualified UVD clock build `49537626…`; these tests do not
change the driver or qualify general GPU rendering.

The workstation still had the obsolete NVIDIA `nvdec` add-on ahead of the
software decoder. With the GTX 1070 removed, it failed during track setup and
left Media Kit without a usable native track decoder. It is preserved outside
the add-on search path at `/boot/home/wx5100/retired-nvidia/nvdec`, SHA-256
`7c3e23e6b91d2c878d4d605d444c3227a498e761e0631f30338e269bf2b3b460`.
No Media Kit library replacement or media-server restart was needed.

Evidence is in the X399 lab's `evidence/wx5100` directory:
`summit-decoder-syntax.log`,
`summit-track-hardware-after-nvdec.log`, `summit-track-faults.log`,
`summit-track-rgb-comparison.log`, `summit-track-colors.log`, and
`summit-track-color-comparison.log`. Raw BGRA captures and MP4 fixtures are kept
alongside them. The complete build log is `summit-private-build.log`.

## Focused test tools

`tools/amduvd-test/build.py` builds the real file decoder test against an idle
reference build, writing only its specified output directory. Supply the amended
engine checkout with `--source` and the reference's generated Ninja directory
with `--reference-build`. Its `track-decode` executable accepts a video, expected
frame count, expected final decoder short name and a BGRA capture path; an
optional final argument seeks to a time in microseconds before decoding.

`make-fixtures.sh OUTPUT-DIRECTORY` creates the four color fixtures with FFmpeg.
`verify-rgb.py INPUT.mp4 CAPTURE.bgra` checks the entire picture sequence with
NumPy (tested with 2.2.6). `--fallback-after N` additionally checks the software
suffix, and `--start-frame N` checks a capture beginning at a seek keyframe.

`fault-addon.cpp` builds a **test-only** proxy. It loads the real add-on from
`SUMMIT_TEST_AMDUVD_REAL` and fails after the output count specified by
`SUMMIT_TEST_AMDUVD_FAIL_AFTER`. It must never ship or remain in a Media Kit
add-on directory. The lab run held its workstation lock, saved the real add-on,
used a restoration trap and checked the restored hash before proceeding.

The host SPS selection-parser regression builds the actual parser with ASan
and UBSan (`sps-parser-test.py --output DIRECTORY`). It reproduces an overflow
from an out-of-range signed scaling delta, then verifies rejection before
addition, both legal boundary values and 25,000 malformed configurations.
The correction and early cancellation check for cached MSE replay are included
in the completed private engine build; the earlier focused results above
describe commit `fc9cd5a`. `mse.html` now accepts `timeoutMs` for sustained playback and
long-GOP recovery checks.

Additional native fault tests reproduced two file-recovery failures: a failed
hardware seek reset stopped playback, and a decoder format rejected on one
read could still write into the caller's undersized buffer on the next read.
The corrected module switches to software after reset failure and latches
incompatible output geometry before any subsequent decode. It preserves the
bitmap's original format and retains fallback preroll across interrupted reads.

The reset-failure test seeks to the 2.5 s keyframe and returns all 84 remaining
software pictures, exactly matching FFmpeg RGB. The format test advertises a
smaller hardware output, fails before hardware writes, then checks three reads:
all reject the larger software output without changing any byte of the guarded
buffer. Both tests fail against the prior module and pass after the correction.
The normal 144-picture hardware run and all four mid-stream failure runs also
pass again, with the same complete RGB comparisons. Logs are
`summit-seek-format-{before,after}.log`, `summit-failed-seek-rgb.log`,
`summit-track-{faults,hardware}-v2.log` and `summit-recovery-v2-rgb.log`.

The fault proxy's additional test controls are `SUMMIT_TEST_AMDUVD_FAIL_SEEK`
and `SUMMIT_TEST_AMDUVD_BAD_WIDTH` (the latter only takes effect with
`SUMMIT_TEST_AMDUVD_FAIL_AFTER=0`). The track test's expected decoder value
`format-error` checks repeated rejection without output writes. The proxy is
restored to the real add-on and its hash verified after each test group.

Two additional native file fixtures pass using the current selection module
(`c96cb9c`). A 36-picture stream changes coded height 240 → 256 → 240 at IDRs,
with cropping preserving visible 320×240 throughout. A twelve-picture
Baseline stream uses POC type 2 and MMCO 5 on each of its eleven P pictures.
Both finish entirely on `amduvd h264`, preserve frame counts and timestamp
order, and match independently decoded RGB within one channel value.
The latter fixture also passes a direct native NV12 comparison with zero
differences; the cropped-height fixture passes 204 Media Kit output pictures
in NV12, I420 and packed YUV with exact pixels and timestamps. These fixtures
and comparisons live under `format-transition/` and `mmco5-fixture/` in the
WX5100 evidence directory. They do not yet qualify the full browser/MSE path.

For browser pixel checks, `mse.html` accepts `capturePausedFrame=1` with its
pause controls and includes a full-resolution PNG in the JSON result.
`verify-rgb.py --start-frame N --frame-count 1` checks the corresponding
decoded BGRA capture against that exact source picture without searching for
a matching frame.

The full browser's first MSE H.264 run completes all 144 pictures with zero
drops and no media errors. The trace names the AMD UVD decoder and contains
144 ordered output timestamps; the page reaches normal end, the browser quits
cleanly and the crash monitor reports no new crashes or syslog events. Evidence:
`probe-20261011-025740-summit-wx5100-mse144-llvmpipe` in this checkout's
`.vm/bench/`, and `summit-browser-mse144-llvmpipe.log` in X399 evidence.

Page rendering for these media tests uses a separate software Mesa prefix at
`/boot/home/wx5100/summit-mesa/prefix`: llvmpipe (LLVM 20.1.8), Mesa 25.3.6.
The original installed Mesa contains only Zink and cannot initialize EGL with
the old NVIDIA card removed. The private stack passes independent surfaceless,
pbuffer and threaded GLES pixel checks with zero mismatches. Its build log is
`summit-software-mesa-build.log`. This is CPU page compositing with hardware
video decoding; it does not establish AMD GPU rendering.

File playback also selects AMD UVD and reaches normal end with one startup
preroll picture plus 143 playback pictures, no media error and clean shutdown
(`probe-20261011-025822-summit-wx5100-file144`). File playback does not currently
implement the browser's playback-quality frame counters.

The MSE paused seek to 2.75 seconds holds an unchanged picture for 1.5 seconds
and resumes to normal end. Its full-size PNG matches source frame 64 within
one RGB value; the first MSE timestamp is 0.083333 seconds. Evidence:
`probe-20261011-025903-summit-wx5100-mse-paused-seek` and
`summit-browser-mse-paused-pixels.log`.

The analogous file test reproduced a seek-boundary error: at 2.75 seconds,
frame 65 was shown instead of frame 66 (file timestamps start at zero).
The saved PNG fails comparison with frame 66 and matches frame 65 within one
RGB value. The substitute decoder's current time denotes the returned
picture's end, so seek completion now requires that end to be strictly past
the target. This retains the native track's start-timestamp comparison.
The correction is awaiting the private incremental build and repeated pixel
check. Before-fix evidence: `probe-20261011-025950-summit-wx5100-file-paused-seek`,
`summit-browser-file-paused-pixels-before.log` and
`summit-browser-file-paused-confirm65.log`.

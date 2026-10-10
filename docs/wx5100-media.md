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
translation units pass native syntax checks. A complete private engine build is
in progress under `/boot/home/wx5100/summit-webkit/WebKitBuild/WX5100`; MSE browser
playback and runtime recovery still require verification with that engine.

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
The correction and an early cancellation check for cached MSE replay are
pending the next native incremental build; the native results above describe
commit `fc9cd5a`. `mse.html` now accepts `timeoutMs` for sustained playback and
long-GOP recovery checks.

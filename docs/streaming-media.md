# MediaSource streaming

Summit uses incremental container parsers and Haiku Media Kit decoders. The
native integration is checked on X399 with actual playback, moving video
pixels, seeking, pause/resume and end events. Direct DASH and encrypted media retain
their separately documented H.264/AAC scope.

| Byte-stream MIME type | Supported codecs |
| --- | --- |
| `video/mp4`, `audio/mp4` | Existing H.264 and AAC-LC; HEVC Main/Main10/Main Still Picture, VP9, AV1, AC-3 and E-AC-3 |
| `video/webm`, `audio/webm` | VP8, VP9, AV1, Opus and Vorbis; audio MIME types reject video codecs |
| `audio/aac` | ADTS AAC-LC, including MPEG-2 AAC-LC |
| `audio/mpeg` | MPEG audio Layers I, II and III |
| `video/mp2t` | H.264, HEVC, AAC-LC, MPEG audio Layers I/II/III, AC-3 and E-AC-3 |

Raw AAC and MPEG audio use MSE sequence mode and reject a `codecs` parameter,
including an empty parameter. MIME type matching is case insensitive. Other
containers validate codec strings and reject incompatible combinations. Vorbis
requires the installed FFmpeg library's public packet-duration API. Unsupported
WebM content compression or encryption produces an append error. The new clear
codecs do not extend the [Clear Key](encrypted-media.md) support matrix.

The decoder registry can select installed hardware. Existing NVDEC/Raspberry Pi
H.264 selection and software fallback remain enabled. On X399, the additional
video codecs listed here were tested with software decoding; this change does
not add NVDEC codec implementations. HEVC/AV1 presentation timestamps are kept
from the container because the installed decoders do not preserve them reliably.

WebM parsing supports fixed, Xiph and EBML lacing, Opus pre-skip, discard
padding and audio seek preroll. Fully discarded packets still prime the decoder.
PCM clipping uses exact sample counts for append windows and seeks. Changing
between stereo floating-point Opus and mono 16-bit MPEG audio drains the old
output before replacing the native sound player.

Raw MPEG audio handles incremental headers and frames, free-format MPEG frame
sizing, ID3v1/v2 and ICY header metadata. Layer III uses sufficient seek preroll
for the bit reservoir in low-rate streams. Parser storage is bounded.

MPEG-TS uses installed FFmpeg public APIs behind transport validation. It
accepts one program, checks packet continuity, PSI CRCs, PES headers and PCR,
and drops exact duplicate transport packets. Arbitrary append boundaries do
not truncate access units. Explicit end-of-stream drains final reordered
frames before ending playback; truncated transport produces a decode error.
Abort preserves committed program and decoder configuration while discarding
partial input. The parser unwraps 33-bit clocks, joins declared discontinuities,
and resets its internal timestamp offset on abort or a successful MSE
`timestampOffset` change, following the
[TS byte-stream requirements](https://www.w3.org/TR/mse-byte-stream-format-mp2t/).
Changed programs or PMT track layouts require `changeType()` to initialize a
new parser and remain subject to MSE's track-count constraints. The TS path
retains NVDEC H.264 selection, verified on X399.

## Verification

The previous integration bundle is `bundle-e821ncjv`, using engine patch SHA-256
`f9e39981f6ad6ff511cab0639fa0a9231d719cb4412be95b1fddfb2cabfd13c0`.
The tests cover 31 real-browser scenarios plus exact PCM seek/trim and video
presentation-order checks. The native codec probe passes 43 decode checks on
29 files. AAC (252 DOM checks), Clear Key (207 checks), DASH (82 checks)
and all five host suites pass on the integration. Owned test processes exit normally with
no new crash reports or remaining helpers.

```sh
python3 tools/bench/test-webm-parser.py --output .vm/webm-parser
python3 tools/bench/test-mpeg-audio-parser.py --output .vm/mpeg-audio-parser
SUMMIT_BENCH_HOST=workstation SUMMIT_WS_MULTIPLEX=0 \
  python3 tools/bench/test-streaming-codecs.py \
  --fixtures .vm/webm-parser .vm/mpeg-audio-parser \
  --output .vm/streaming-codecs
SUMMIT_BENCH_HOST=workstation SUMMIT_WS_MULTIPLEX=0 \
  python3 tools/bench/test-streaming.py --bundle /absolute/frozen/bundle \
  --output .vm/streaming-browser
```

`test-streaming.py` generates its own WebM, raw-audio, fragmented-MP4 and TS media.
Add its `media` directory to the codec probe’s `--fixtures` arguments to include
those WebM and MP4 files in the native checks. Its browser-only malformed TS
fixtures must be excluded from native decoding; use the TS parser's
`result.json` manifest for the valid transport fixtures. Parser tests require FFmpeg,
a C++ compiler, and sanitizer support on the host. Native tests compile their
own driver and launch an isolated browser profile through `tools/ws.sh`.

Local evidence is retained in `.vm/issues43-49/` under
`webm-parser-integrated`, `mpeg-audio-parser`, `streaming-codecs-timing-x399`
and `streaming-browser-timing-x399`.

The TS foundation uses pinned FFmpeg 6.1.2 public headers and checks the
installed `libavformat.so.60`, `libavcodec.so.60` and `libavutil.so.58` ABI
versions before use. `tools/prepare-ffmpeg-headers.py` installs only headers in
a private prefix; it does not replace OS libraries. To reproduce its portable
checks, prepare the headers with an absolute `--prefix`, then run
`tools/bench/test-mpegts-parser.py --headers PREFIX --output .vm/mpegts-parser`.
Pass `.vm/mpegts-parser` to the native codec probe's `--fixtures` argument.
Foundation evidence is in `.vm/issues43-49/{mpegts-parser,mpegts-codecs-rollover}/`.

The initial TS foundation also compiled in the native WebKit build. Bundle
`bundle-zxsy87w0` verifies the pinned dependency manifest, header license
notices and a real-browser streaming smoke test. Its engine patch is
`181476f3ab7c1781507942516a5686171831141dd02ae3b73723ffe6c9273865`.
That historical foundation bundle did not expose TS through MediaSource.

Current TS portable checks pass 10 fixtures (988 packets) at nine append
sizes, 26 structural cases, and 128 mutations under ASan/UBSan. They also
compare two discontinuity directions with an independent timeline and check
abort and timestamp-offset reset after rollover. The X399 native probe passes
21 decode checks across 12 files, including both discontinuities and NVDEC
H.264. Evidence: `.vm/issues43-49/{mpegts-parser-complete,ts-native-complete}/`.
The final browser integration verification is in progress.

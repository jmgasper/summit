# MediaSource streaming

Summit uses incremental container parsers and Haiku Media Kit decoders. The
formats below are verified on X399 with actual playback, moving video pixels,
seeking, pause/resume and end events. Direct DASH and encrypted media retain
their separately documented H.264/AAC scope.

| Byte-stream MIME type | Supported codecs |
| --- | --- |
| `video/mp4`, `audio/mp4` | Existing H.264 and AAC-LC; HEVC Main/Main10/Main Still Picture, VP9, AV1, AC-3 and E-AC-3 |
| `video/webm`, `audio/webm` | VP8, VP9, AV1, Opus and Vorbis; audio MIME types reject video codecs |
| `audio/aac` | ADTS AAC-LC, including MPEG-2 AAC-LC |
| `audio/mpeg` | MPEG audio Layers I, II and III |

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
sizing, ID3v1/v2 and ICY header metadata. Parser storage is bounded. MPEG-TS is
still in progress and is not advertised as supported.

## Verification

The integration bundle is `bundle-e821ncjv`, using engine patch SHA-256
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

`test-streaming.py` generates its own WebM, raw-audio and fragmented-MP4 media.
Add its `media` directory to the codec probe’s `--fixtures` arguments to include
those WebM and MP4 files in the native checks. Parser tests require FFmpeg,
a C++ compiler, and sanitizer support on the host. Native tests compile their
own driver and launch an isolated browser profile through `tools/ws.sh`.

Local evidence is retained in `.vm/issues43-49/` under
`webm-parser-integrated`, `mpeg-audio-parser`, `streaming-codecs-timing-x399`
and `streaming-browser-timing-x399`.

# Peer to peer: camera, microphone, screen sharing, MediaRecorder, WebRTC, Web Audio

Summit's engine now has the pieces html5test.co lists under "Peer to peer"
(RTCPeerConnection, data channels, `getUserMedia`, `getDisplayMedia`,
`enumerateDevices`, `MediaRecorder`) and Web Audio, which call clients use for
level meters and noise processing. The goal is Zoom and Teams calls in the
browser with a webcam and a shared screen.

## What is built and where

The engine options (`Source/cmake/OptionsHaiku.cmake`): `ENABLE_MEDIA_STREAM`,
`ENABLE_WEB_RTC` (with `USE_LIBWEBRTC`), `ENABLE_MEDIA_RECORDER` and
`ENABLE_WEB_AUDIO` are on. An existing build directory keeps its cached
values, so they have to be passed once (`-DENABLE_MEDIA_STREAM=ON
-DENABLE_WEB_RTC=ON -DENABLE_MEDIA_RECORDER=ON -DENABLE_WEB_AUDIO=ON`); the
change rebuilds everything.

### libwebrtc

WebKit's vendored libwebrtc (`Source/ThirdParty/libwebrtc`, built by its
`CMakeLists.txt`) builds for Haiku with a branch of its own in that file:

- `WEBRTC_POSIX` without `WEBRTC_LINUX`: no epoll, prctl or `/proc`. The
  socket server uses `poll()` (select() is limited to descriptors below
  `FD_SETSIZE`, and WebKit processes have more).
- BoringSSL's symbols get the prefix `webrtc_bssl_`
  (`BORINGSSL_PREFIX`): the same processes load the system's OpenSSL (curl,
  WebCrypto), and an unprefixed BoringSSL archive member could satisfy a
  reference meant for OpenSSL.
- Opus is the bundled one; libevent and ALSA are not used (WebKit's
  `LibWebRTCAudioModule` drives audio, the stdlib task queue runs tasks).
- libvpx gets SIMD per file (`-mavx2` only on `*_avx2.c` and so on) and
  dispatches at run time, so the library stays baseline x86_64 and ARMv8.0:
  upstream compiles all of it with `-mavx2`, or `-march=armv8.2-a+dotprod` on
  arm64, which the Raspberry Pi 4's Cortex-A72 cannot run. On arm64 Haiku the
  configuration is NEON only (`vpx_config.h`, `aarch64_cpudetect.c`) and
  BoringSSL takes its CPU features from the compiler
  (`OPENSSL_STATIC_ARMCAP`; the Pi has no crypto extensions).
- Source changes, all marked `WEBRTC_WEBKIT_BUILD change`: thread ids and
  names (`find_thread`, `rename_thread`), CPU count, process and thread CPU
  time, resident memory (`get_next_area_info`), interfaces that are up
  (`IFF_LINK`), socket options Haiku does not have (no-ops), `IP_DONTFRAG`,
  `MSG_NOSIGNAL`; abseil's `GetTID()`.
- `getifaddrs()` comes from libbsd; the library links `bsd` and `network`.
- The WebM muxer (libwebm's mkvmuxer) is built into it for MediaRecorder.

Video codecs are libwebrtc's own, VP8 and VP9 through libvpx, and H.264
decoding through the system's FFmpeg (`LibWebRTCH264DecoderHaiku.cpp`):
libavcodec is loaded when first needed (only functions are looked up, and only
the leading members of AVPacket and AVFrame are used, which have kept their
places for many major versions); without it, H.264 is not offered.
`SUMMIT_WEBRTC_H264=0` turns it off. There is no H.264 encoder (Apple uses
VideoToolbox, GTK GStreamer; Haiku's FFmpeg has neither x264 nor OpenH264),
so H.264 is received but not sent.

The bundled libvpx, Opus and libsrtp are compiled with hidden visibility: the
same processes load the system's FFmpeg (through the Media Kit's ffmpeg
add-on), whose own libvpx and libopus calls must not bind to these copies.

### Capture (`WebCore/platform/mediastream/haiku`)

Capture runs in the web process, as on GTK and WPE; the UI process asks it for
devices and to check a request's constraints (`UserMediaCaptureManager`,
the GLib files, which are portable).

- `HaikuCaptureNode.{h,cpp}`: device lists from the Media Kit (live nodes that
  are physical inputs producing raw video or raw audio; the default one
  first) and a consumer node of ours connected to the chosen output.
  Persistent ids are `haiku:<node name>` (node ids change across restarts).
- Camera (`HaikuVideoCaptureSource`): the Media Kit node of a USB camera
  (`usb_webcam` add-on) sends `B_RGB32` pictures of the size put in the
  connection's format; they become I420 frames (libyuv). Presets 160x120 to
  1920x1080 at up to 30 per second. One consumer at a time: a camera in use
  elsewhere fails with "The device is in use by another program."
- Microphone (`HaikuAudioCaptureSource`): a sound card's input or a USB
  microphone, samples converted to interleaved float. The sound card node is
  never stopped (it also plays).
- Screen (`HaikuDisplayCaptureSource`): `BScreen::ReadBitmap` on a thread of
  its own at the frame rate asked for (15 by default), converted to I420 and
  scaled to the size asked for.
- `SUMMIT_CAPTURE_TRACE=1` traces device lists and connections.

### Frames and samples

- `VideoFrameHaiku` (`platform/graphics/haiku`): the port's video frame. I420
  pictures (cameras, screens, WebRTC) share libwebrtc's buffers without a
  copy either way; BGRA/RGBA ones come from pages (canvas, WebCodecs).
  Conversions, scaling and rotation with libyuv; images for painting are
  made once per frame.
- `HaikuAudioData` (`platform/audio/haiku`): interleaved 32-bit float, the
  format every Haiku audio source hands on.

### Playing streams

- `MediaPlayerPrivateMediaStreamHaiku`: a `<video>` or `<audio>` with a
  `srcObject`. The newest picture goes to the compositor as it arrives (I420
  as three textures converted by the YUV shader,
  `CoordinatedPlatformLayerBufferVideoFrameHaiku`).
- `AudioMediaStreamTrackRendererHaiku`: each audio track plays through a
  BSoundPlayer of its own, after a short FIFO that drops late samples.

### WebRTC glue (`platform/mediastream/libwebrtc/haiku`)

The provider (`LibWebRTCProviderHaiku`) and the incoming and outgoing audio
and video sources. Outgoing audio goes to libwebrtc as 10 ms chunks of 16-bit
samples at the capture rate; libwebrtc resamples. Sockets live in the network
process (`NetworkRTCProvider`, as on GTK).

Host candidates are not hidden behind mDNS names on Haiku: WebKit hides them
only where the network process can answer for the names (Avahi, Bonjour), and
Haiku has no mDNS responder. Pages that make peer connections can see the
machine's local addresses, as in browsers before mDNS candidates.

### MediaRecorder

`MediaRecorderPrivateHaiku`: WebM with VP8 (default) or VP9 and Opus,
encoded on a work queue of its own (libvpx, Opus) and muxed as it goes
(libwebm). Audio is resampled to 48 kHz stereo.

### Web Audio

- `AudioDestinationHaiku`: a BSoundPlayer at the system mixer's rate.
- `FFTFrameHaiku`: PFFFT from the libwebrtc tree (the stub asserted on every
  analyser, convolver and panner).
- `decodeAudioData`: PCM WAV directly, everything else through the Media
  Kit (`AudioFileReaderHaiku`).
- PannerNode's HRTF responses are built into the library.
- `MediaStreamAudioSourceNode` and `MediaStreamAudioDestinationNode` work
  with streams (`MediaStreamTrackAudioSourceProviderHaiku`).

## Permissions (engine and Summit)

`decidePolicyForUserMediaPermissionRequest` (UIProcess/haiku/WebView.cpp)
goes through `SitePermissionsHaiku::requestMedia`:

- Camera and microphone are decided for the site in the address bar and can
  be remembered (Block / Allow), like notifications and location. A request
  for both is one question ("camera-microphone"), saved as both.
- A screen is asked for every time. The request lists the screens; Summit
  shows a picker and answers with the one chosen
  (`BWebKitContext::RespondToMediaPermissionRequest`).
- Tabs that use the camera, microphone or a screen get a red dot
  (`capturingCamera`, `capturingMicrophone`, `capturingScreen` in
  `B_WEBKIT_STATE_CHANGED`).

`getUserMedia` needs a secure context (https, localhost, file:);
`SUMMIT_INSECURE_MEDIA_CAPTURE=1` allows plain http for testing.

## Testing

`tools/bench/pages/rtc-selftest.html` checks the html5test features, Web Audio
(an oscillator into an analyser), a canvas stream played locally, recorded
and played back, and a two-peer loopback with video, audio and a data
channel. It needs no camera; `?camera=1` and `?screen=1` add the real
devices. The title ends as `rtc selftest: N/M`.

## Not done yet

- An H.264 encoder for WebRTC.
- Echo cancellation, noise suppression and gain control (libwebrtc's audio
  processing module is in the tree; it needs the played audio as reference).
- Window capture (only whole screens), system audio.
- WebCodecs.

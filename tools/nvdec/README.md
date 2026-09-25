# Workstation NVDEC plugin fix

`reason-lifetime.patch` applies to the NVDEC plugin source at
`/boot/home/build/nvdec` on the workstation. The H.264 decoder keeps the
diagnostic string pointer supplied to `nvdecH264Create()`. The original plugin
passed a stack buffer from `Setup()`, which becomes invalid before decoding.
The patch makes that buffer a decoder member.

Apply it from the Summit checkout on the build host:

```sh
bash tools/ws.sh 'cd /boot/home/build/nvdec && patch -p1' \
  < tools/nvdec/reason-lifetime.patch
```

Then rebuild on the workstation:

```sh
cd /boot/home/build/nvdec
g++ -std=gnu++17 -fPIC -O2 -g \
  -I/boot/system/develop/headers/private/media \
  -c NVDecPlugin.cpp -o NVDecPlugin.summit.o
g++ -shared -o nvdec.summit NVDecPlugin.summit.o nvdec_engine.o \
  nvdec_h264.o nvdec_convert.o h264_parse.o nvRmApi.o -lbe -lmedia
```

The installed add-on is at
`/boot/home/config/non-packaged/add-ons/media/plugins/nvdec`. Keep a backup
before replacing it. Check that `mediadecode /boot/home/bbb12s.mp4 60` names
the NVDEC decoder and produces frames. This check exercises the decoder but
does not establish playback on a website.

On 2026-09-23 the workstation's installed add-on selected
`H.264 on the graphics card (NVDEC)` for `/boot/home/bbb12s.mp4` and decoded
60 of its 1920×1080 frames in 674.9 ms. An engine compile was active, so that
elapsed time is a functional check rather than a throughput benchmark. Summit's
own codec selection is checked below.

A browser-process check on 2026-09-23 opened the Reddit H.264 CMAF URL in
`bundle-kou9exyv`. The video played to its 12.7-second `ended` event with no
media error (`.vm/bench/probe-20260923-015416-summit-codec-image-check/`).
While it played, `listimage` showed both the system FFmpeg media plugin and
`/boot/home/config/non-packaged/add-ons/media/plugins/nvdec` loaded in the
active WebProcess. This establishes that the NVDEC add-on was available inside
Summit's decoding process, but loading an add-on does not prove that Media Kit
selected its decoder for this track. The WebKit engine build was active during
the playback check, so its timing is not a throughput measurement.

`check-codec.cpp` reproduces Summit's HTTP media path more closely: it opens
the downloaded MP4 through `BFile`, requests RGB32 from `DecodedFormat()`,
prints `GetCodecInfo()`, and reads a decoded frame. Build and run it on Haiku:

```sh
c++ -std=c++17 -O2 tools/nvdec/check-codec.cpp -lbe -lmedia -o check-codec
./check-codec reddit-cmaf-720.mp4
```

On the same Reddit CMAF file, it selected `H.264 on the graphics card (NVDEC)`
and decoded one 720×1280 frame successfully. This confirms that Media Kit
selects NVDEC for the file and format request Summit uses.

The browser now supports opt-in `SUMMIT_MEDIA_CODEC_TRACE=1` logging after
`DecodedFormat()`. On 2026-09-23, the mimalloc bundle `bundle-vlgss17_`
reported `H.264 on the graphics card (NVDEC) (nvdec h264), status=0` while
playing that direct Reddit CMAF file to its 12.7-second `ended` event without
an error (`.vm/bench/probe-20260923-025529-summit-mimalloc-browser-codec/`).
A separate `/r/popular/` feed scroll logged the same selected decoder for
multiple video tracks
(`.vm/bench/scroll-20260923-025633-reddit-mimalloc-codec-feed/`). This proves
that the actual Reddit page reaches the accelerated decoder path. Still-image
captures do not establish that every feed video plays smoothly from start to
finish; the direct file probe establishes completion for one URL.

## Skipping conversion of pictures that will not be shown

`skip-conversion.patch` (applied on the workstation on 2026-09-25) lets a
caller pass a negative `media_decode_info::time_to_decode`: pictures whose time
is earlier than its magnitude are decoded but not converted to RGB. Summit's
MSE renderer uses it after a seek, when every picture from the keyframe up to
the target has to be decoded but none is shown. Other callers pass 0 or
`B_INFINITE_TIMEOUT` and are unaffected. It is built the same way as above into
`nvdec.skip` and installed by rename over
`/boot/home/config/non-packaged/add-ons/media/plugins/nvdec`.

Keep backups **outside** that directory: Media Kit registers every add-on in it,
and a second H.264 decoder there (the old `nvdec.before-summit` copy) could be
picked instead of the installed one. Backups are now in
`/boot/home/build/nvdec/installed-backups/`.

On YouTube with `&t=120`, the first picture after the seek arrived 0.61 s after
the seek settled, against 1.13 s before, with no dropped frames afterwards.

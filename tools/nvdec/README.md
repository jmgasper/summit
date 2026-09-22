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
own selected codec still needs to be logged from `BMediaTrack::GetCodecInfo()`
after `DecodedFormat()` on a real video source.

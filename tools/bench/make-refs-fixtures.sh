#!/usr/bin/env bash
# Make 10 s 1080p30 H.264 clips with 4, 15 and 16 reference frames (level
# 5.1), for the NVDEC picture-slot limit: the X399's add-on decodes 15 and
# skips most of 16, which Summit then hands to libavcodec. Needs ffmpeg with
# libx264. pages/media.html?src=refs-media/refs16.mp4 plays one.
set -euo pipefail
OUT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/pages/refs-media
mkdir -p "$OUT"
for refs in 4 15 16; do
    ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 10 \
        -c:v libx264 -preset slow -refs "$refs" -bf 3 -level 5.1 -x264-params keyint=300 \
        -pix_fmt yuv420p -movflags +faststart "$OUT/refs$refs.mp4"
done
ls -l "$OUT"

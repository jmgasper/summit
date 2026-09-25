#!/usr/bin/env bash
# Make the fragmented MP4 files pages/mse.html appends: 12 s of 720p30 H.264
# High profile with B-frames and 44.1 kHz stereo AAC, as separate video and
# audio streams in 2-second fragments, the way YouTube and most DASH players
# serve them. Needs ffmpeg with libx264.
set -euo pipefail
OUT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/pages/mse-media
mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440:sample_rate=44100 \
    -t 12 -c:v libx264 -profile:v high -bf 2 -g 60 -pix_fmt yuv420p -c:a aac -b:a 128k -ac 2 "$TMP/plain.mp4"
for kind in video audio; do
    select=$([[ $kind == video ]] && echo -an || echo -vn)
    ffmpeg -loglevel error -y -i "$TMP/plain.mp4" $select -c copy \
        -movflags frag_keyframe+empty_moov+default_base_moof -frag_duration 2000000 "$OUT/$kind-frag.mp4"
done
ls -l "$OUT"

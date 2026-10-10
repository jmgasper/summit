#!/usr/bin/env bash
# Small H.264 fixtures whose range and matrix cannot be inferred from size.
set -euo pipefail
if (( $# != 1 )); then
    echo 'usage: make-fixtures.sh OUTPUT-DIRECTORY' >&2
    exit 2
fi
mkdir -p "$1"
for matrix in bt709 smpte170m; do
    for range in tv pc; do
        ffmpeg -v error -y -f lavfi -i testsrc2=size=320x240:rate=24 -t 1 \
            -vf "scale=in_range=tv:out_range=$range" -pix_fmt yuv420p -c:v libx264 \
            -threads 2 -profile:v high -bf 3 -g 12 -keyint_min 12 -sc_threshold 0 \
            -color_range "$range" -colorspace "$matrix" -color_primaries "$matrix" -color_trc "$matrix" \
            "$1/$matrix-$range.mp4"
    done
done

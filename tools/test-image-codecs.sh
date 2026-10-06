#!/usr/bin/env bash
# Run on Haiku with the prepared private prefix. The source directory can be
# a staged codec folder; by default it is the prepared engine checkout.
set -euo pipefail
SUMMIT_CODEC_ROOT=$(cd "$(dirname "$0")/.." && pwd)
SUMMIT_CODEC_SOURCE=${SUMMIT_CODEC_SOURCE:-$SUMMIT_CODEC_ROOT/.cache/WebKit/Source/WebCore/platform/image-decoders/jpegxr}
SUMMIT_CODEC_PREFIX=${SUMMIT_CODEC_PREFIX:-/boot/home/summit-deps/image-codecs-20261006}
SUMMIT_CODEC_OUTPUT=${SUMMIT_CODEC_OUTPUT:-$SUMMIT_CODEC_ROOT/.vm/image-codec-tests}
mkdir -p "$SUMMIT_CODEC_OUTPUT"
c++ -std=c++23 -O2 -fPIC -DSUMMIT_CODEC_STANDALONE \
    -I"$SUMMIT_CODEC_SOURCE" -I"$SUMMIT_CODEC_PREFIX/include/jxrlib" \
    "$SUMMIT_CODEC_SOURCE/JPEGXRCodec.cpp" "$SUMMIT_CODEC_ROOT/tests/ImageCodecTests.cpp" \
    -L"$SUMMIT_CODEC_PREFIX/lib" -Wl,-rpath,"$SUMMIT_CODEC_PREFIX/lib" \
    -ljxrglue -ljpegxr -o "$SUMMIT_CODEC_OUTPUT/ImageCodecTests"
"$SUMMIT_CODEC_OUTPUT/ImageCodecTests" "$SUMMIT_CODEC_OUTPUT/pattern.jxr" \
    "$SUMMIT_CODEC_ROOT/tests/fixtures/image-codecs/rgb-19x7.jxr"

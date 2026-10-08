#!/usr/bin/env bash
# Cross-builds the probes in probes/ for arm64 Haiku into OUT (default the
# Pi share's tools directory) and the summitctl test driver.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${1:-/mnt/Documents/SummitPi/tools}
CROSS=/mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-
SYSROOT=/mnt/HaikuWork/build/summit-arm64/sysroot
MESA=/mnt/HaikuWork/rpi4/mesa
GLDEPS=/mnt/HaikuWork/rpi4/summit-gl/deps
mkdir -p "$OUT"
SPECS=$(mktemp); printf '*libgcc:\n-lgcc_s -lgcc\n\n' > "$SPECS"
CXX="${CROSS}g++ --sysroot=$SYSROOT -specs=$SPECS -O2"
$CXX "$HERE/probes/stime.cpp" -o "$OUT/stime"
$CXX "$HERE/probes/dltime.cpp" -o "$OUT/dltime"
$CXX "$HERE/probes/faultbench.cpp" -o "$OUT/faultbench"
$CXX "$HERE/probes/threadstate.cpp" -o "$OUT/threadstate"
$CXX "$HERE/probes/killonspawn.cpp" -o "$OUT/killonspawn"
$CXX -I$GLDEPS/include "$HERE/probes/fctime.cpp" -L$GLDEPS/lib -lfontconfig \
	-Wl,-rpath-link,$GLDEPS/lib -Wl,-rpath-link,/mnt/HaikuWork/build/summit-arm64/deps/lib -o "$OUT/fctime"
for p in egltime readback; do
	$CXX -I$MESA/mesa-25.3.6/include "$HERE/probes/$p.cpp" -L$SYSROOT/boot/system/lib -L$MESA/stage -lEGL -lGLESv2 -o "$OUT/$p"
done
# Compile the exact policy under test, not a second implementation of it.
python3 - "$ROOT/.cache/WebKit" "$OUT/readback-policy.inc" <<'PY'
import pathlib, sys
source = pathlib.Path(sys.argv[1]) / 'Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/AcceleratedSurface.cpp'
text = source.read_text()
start = text.index('static int readbackBandRowsHaiku(')
end = text.index('\n// The driver copies', start)
pathlib.Path(sys.argv[2]).write_text(text[start:end])
PY
$CXX -std=c++17 -I"$OUT" -I$MESA/mesa-25.3.6/include "$HERE/probes/readback-policy.cpp" \
	-L$SYSROOT/boot/system/lib -L$MESA/stage -lEGL -lGLESv2 -o "$OUT/readback-policy"
$CXX -std=c++17 -I"$ROOT/src" "$ROOT/tools/bench/summitctl.cpp" -lbe -o "$OUT/summitctl"
cp "$HERE"/board/*.sh "$OUT"/
rm -f "$SPECS"
ls "$OUT"

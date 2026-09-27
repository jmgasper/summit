#!/usr/bin/env bash
# Run the profile-training workloads on the X399 against an instrumented
# bundle (built with -fprofile-generate -fprofile-update=atomic and linked with
# -u__gcov_dump). Every process writes its counters on exit because
# SUMMIT_GCOV_DUMP=1; helpers get a minute to do so before anyone is killed.
#
#   bash tools/bench/train-pgo.sh /boot/home/summit/build-modern-browser/BUNDLE
#
# Workloads: Speedometer 3.1, the 400-card scroll fixture, a Wikipedia wheel
# burst, two windows of real sites with a scroll while the other reloads, and
# video through NVDEC, through libavcodec (16 reference frames) and through
# Media Source Extensions. Needs tools/bench/make-refs-fixtures.sh and
# make-mse-fixtures.sh to have been run.
set -euo pipefail
bundle=${1:?usage: train-pgo.sh BUNDLE_PATH}
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
export SUMMIT_BENCH_HOST=workstation SUMMIT_BENCH_LEFTOVER_GRACE=60
env=(--env SUMMIT_LIBRARY_PATH_PREFIX=/boot/home/summit-mesa/prefix/lib --env SUMMIT_GCOV_DUMP=1
     --env SUMMIT_SKIA_GL_CONTEXT=1 --env SUMMIT_SCROLL_REFRESH_TIMER=16)
step() { echo "== $(date +%H:%M:%S) $*"; }

step speedometer
python3 tools/bench/run-speedometer.py --bundle "$bundle" --iterations 10 --label pgo2-train "${env[@]}" | grep -E '^score|contention' || true
step scroll fixture
python3 tools/bench/run-probe.py --bundle "$bundle" --page scroll.html --label pgo2-train-scroll "${env[@]}" | tail -1 || true
step wikipedia scroll
python3 tools/bench/run-scroll.py "https://en.wikipedia.org/wiki/Haiku_(operating_system)" --bundle "$bundle" --settle 10 \
    --notches 200 --window 0,24,1919,1079 --label pgo2-train-wiki "${env[@]}" | tail -1 || true
step real sites
python3 tools/bench/run-multitab.py --bundle "$bundle" --windows 2 --tabs 6 --idle 10 --load-timeout 120 --label pgo2-train "${env[@]}" \
    | grep -E 'load:|scroll under' | cut -c1-160 || true
for page in "media.html?src=refs-media/refs4.mp4&seek=6.5" "media.html?src=refs-media/refs16.mp4&seek=6.5" "mse.html"; do
    step "$page"
    python3 tools/bench/run-probe.py --bundle "$bundle" --page "$page" --label pgo2-train-media "${env[@]}" | tail -1 || true
done
step done
bash tools/ws.sh "find /boot/home/summit-webkit-extensions/WebKitBuild -newer $bundle/Summit -name '*.gcda' | wc -l"

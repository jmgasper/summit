#!/bin/sh
# scroll-test.sh BUNDLE PROFILE URL TAG [K=V...] : loads URL, then 40 wheel
# notches 50 ms apart at (600,500); prints frames composited during the burst,
# their interval and mean per-frame costs from the compositor timing trace.
b=$1; p=$2; url=$3; tag=$4; shift 4
for kv in "$@"; do export "$kv"; done
d=/boot/home/summit-ec; log=$d/logs/scroll-$tag.log
export SUMMIT_COMPOSITOR_TIMING_TRACE=1 SUMMIT_ENABLE_INPUT_SYNTHESIS=1
"$b/Summit" --profile "$p" "$url" > $log 2>&1 &
pid=$!
sleep 14
t0=$($d/stime /bin/true 2>&1 | awk '{print $3}')
$d/summitctl --team $pid scroll 40 50 1 600 500 > /dev/null 2>&1
sleep 4
t1=$($d/stime /bin/true 2>&1 | awk '{print $3}')
$d/summitctl --team $pid quit > /dev/null; sleep 3; kill -9 $pid 2>/dev/null
awk -v t0=$t0 -v t1=$t1 -v tag=$tag '/Summit compositor timing:/ {
    for (k = 1; k <= NF; k++) { split($k, kv, "="); v[kv[1]] = kv[2] }
    at = v["at"] / 1e6; if (at < t0 || at > t0 + 2.6) next
    n++; if (last) { gap = at - last; gaps += gap; if (gap > worst) worst = gap; if (gap > 0.050) long++ } last = at
    total += v["total"]; read += v["read"]; paint += v["paint"]; flush += v["flush"]; tiles += v["tiles"] }
  END { if (n < 2) { print tag ": no frames"; exit }
    printf "%s: %d frames in the burst, %.1f fps, worst gap %.0f ms, gaps>50ms %d; per frame total %.1f flush %.1f paint %.1f read %.1f ms, tiles %.1f\n",
      tag, n, (n - 1) / gaps, worst * 1000, long, total / n, flush / n, paint / n, read / n, tiles / n }' $log

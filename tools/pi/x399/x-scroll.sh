#!/bin/sh
# x-scroll.sh BUNDLE PROFILE URL TAG : 40 wheel notches 50 ms apart after 14 s; frames during the burst.
b=$1; p=$2; url=$3; tag=$4; d=/boot/home/summit/claude-ec
M=/boot/home/summit-mesa/prefix-20261002
export WEBKIT_EXEC_PATH=$b LIBRARY_PATH="$M/lib:$b/lib:/boot/system/lib" __EGL_VENDOR_LIBRARY_FILENAMES=$M/data/glvnd/egl_vendor.d/50_mesa.json
export SUMMIT_SKIA_GL_CONTEXT=1 SUMMIT_SCROLL_REFRESH_TIMER=16 SUMMIT_COMPOSITOR_TIMING_TRACE=1 SUMMIT_ENABLE_INPUT_SYNTHESIS=1
"$b/Summit" --profile "$p" "$url" > $d/logs/scroll-$tag.log 2>&1 &
pid=$!
sleep 14
t0=$($d/stime /bin/true 2>&1 | awk '{print $3}')
/boot/home/summit/claude-ctl/summitctl --team $pid scroll 40 50 1 600 500 > /dev/null 2>&1
sleep 4
/boot/home/summit/claude-ctl/summitctl --team $pid quit > /dev/null; sleep 4; kill -9 $pid 2>/dev/null
awk -v t0=$t0 -v tag=$tag '/Summit compositor timing:/ { for (k = 1; k <= NF; k++) { split($k, kv, "="); v[kv[1]] = kv[2] }
    at = v["at"] / 1e6; if (at < t0 || at > t0 + 2.6) next
    n++; if (last) { gap = at - last; gaps += gap; if (gap > worst) worst = gap; if (gap > 0.033) long++ } last = at; total += v["total"] }
  END { if (n < 2) { print tag ": no frames"; exit } printf "%s: %d frames, %.1f fps, worst gap %.0f ms, gaps>33ms %d, mean frame %.1f ms\n", tag, n, (n - 1) / gaps, worst * 1000, long, total / n }' $d/logs/scroll-$tag.log

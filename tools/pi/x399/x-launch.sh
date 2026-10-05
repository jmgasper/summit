#!/bin/sh
# x-launch.sh BUNDLE PROFILE LOG SECONDS URL [K=V ...] : a private bundle copy
# launched like the desktop launcher (Mesa prefix-20261002), with timing traces;
# quits it with summitctl after SECONDS.
b=$1; profile=$2; log=$3; secs=$4; url=$5; shift 5
M=/boot/home/summit-mesa/prefix-20261002
export WEBKIT_EXEC_PATH=$b LIBRARY_PATH="$M/lib:$b/lib:/boot/system/lib"
export __EGL_VENDOR_LIBRARY_FILENAMES=$M/data/glvnd/egl_vendor.d/50_mesa.json
export SUMMIT_SKIA_GL_CONTEXT=1 SUMMIT_SCROLL_REFRESH_TIMER=16
export SUMMIT_EXTENSION_TIMING=1 SUMMIT_COMPOSITOR_TIMING_TRACE=1 SUMMIT_STARTUP_TRACE=1 SUMMIT_ENABLE_INPUT_SYNTHESIS=1
for kv in "$@"; do export "$kv"; done
/boot/home/summit/claude-ec/stime "$b/Summit" --profile "$profile" $url > "$log" 2>&1 &
pid=$!
sleep "$secs"
/boot/home/summit/claude-ctl/summitctl --team $pid quit >/dev/null 2>&1
sleep 4
kill -9 $pid 2>/dev/null
echo "pid $pid done"

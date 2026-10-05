#!/bin/sh
# x-bench.sh BUNDLE PROFILE URL PREFIX SECONDS : like the Pi's bench-title.sh, launched like the desktop launcher.
b=$1; p=$2; url=$3; prefix=$4; secs=$5; d=/boot/home/summit/claude-ec
M=/boot/home/summit-mesa/prefix-20261002
export WEBKIT_EXEC_PATH=$b LIBRARY_PATH="$M/lib:$b/lib:/boot/system/lib" __EGL_VENDOR_LIBRARY_FILENAMES=$M/data/glvnd/egl_vendor.d/50_mesa.json
export SUMMIT_SKIA_GL_CONTEXT=1 SUMMIT_SCROLL_REFRESH_TIMER=16
"$b/Summit" --profile "$p" "$url" > $d/logs/x-bench.log 2>&1 &
pid=$!
t=0; result=timeout
while [ $t -lt $secs ]; do
  sleep 1; t=$((t + 1))
  s=$(/boot/home/summit/claude-ctl/summitctl --team $pid state 2>/dev/null)
  case "$s" in *"\"title\":\"$prefix"*) result=$(echo "$s" | sed 's/.*"title":"\([^"]*\)".*/\1/'); break;; esac
done
echo "$result"
/boot/home/summit/claude-ctl/summitctl --team $pid quit > /dev/null 2>&1; sleep 4; kill -9 $pid 2>/dev/null

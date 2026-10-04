#!/bin/sh
# pi-launch.sh SUMMIT PROFILE LOG SECONDS URL [K=V ...]
# Starts SUMMIT (path to the Summit executable) with timing traces, waits
# SECONDS, asks it to quit, then kills what is left of that team.
summit=$1; profile=$2; log=$3; secs=$4; url=$5; shift 5
for kv in "$@"; do export "$kv"; done
export SUMMIT_EXTENSION_TIMING=1 SUMMIT_COMPOSITOR_TIMING_TRACE=1
/boot/home/summit-ec/stime "$summit" --profile "$profile" $url > "$log" 2>&1 &
pid=$!
sleep "$secs"
/boot/home/summit-ec/summitctl --team $pid quit >/dev/null 2>&1
sleep 3
kill -9 $pid 2>/dev/null
echo "pid $pid done"

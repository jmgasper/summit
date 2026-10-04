#!/bin/sh
# frame-rate.sh SUMMIT PROFILE URL WAIT [K=V...] : loads URL, after WAIT
# seconds prints the window's frame statistics twice, 10 s apart (summitctl
# framestats), and the CPU used by each of the browser's teams in between.
summit=$1; p=$2; url=$3; wait=$4; d=/boot/home/summit-ec
shift 4
l=${LOGS:-$d/logs}
# A blanked screen stops app_server drawing and skews the compositor.
for t in $(ps | awk '/screen_blanker/ && !/awk/ {print $(NF-3)}'); do kill $t; done
env "$@" "$summit" --profile "$p" "$url" > $l/frame-rate.log 2>&1 &
pid=$!
sleep $wait
teams="$pid $(ps | awk -v n="$(dirname $summit)/" 'index($0, n) && !index($0, "/Summit") {print $(NF-3)}' | tr '\n' ' ')"
$d/summitctl --team $pid framestats
$d/threadstate 10000 2 $teams > $l/frame-rate.threads 2>&1
$d/summitctl --team $pid framestats
awk '$NF == "ms" {v = $(NF-1); sub(/^\+/, "", v); if ($1 != first && first != "") ms[$2] += v; if (first == "") first = $1} END {for (t in ms) printf "team %s: %.2f cores\n", t, ms[t] / 10000}' $l/frame-rate.threads
$d/summitctl --team $pid quit > /dev/null 2>&1; sleep 4; kill -9 $pid 2>/dev/null
for t in $teams; do kill -9 $t 2>/dev/null; done

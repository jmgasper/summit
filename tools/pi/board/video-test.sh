#!/bin/sh
# video-test.sh SUMMIT PROFILE URL SECONDS TAG [K=V...] : plays URL for
# SECONDS, then prints the page title (vplay.html puts frames shown and
# dropped there), the CPU time each Summit process used during the last 10
# seconds and the browser's media lines. Logs: $LOGS (default logs/).
summit=$1; p=$2; url=$3; secs=$4; tag=$5; d=/boot/home/summit-ec
shift 5
l=${LOGS:-$d/logs}
# A blanked screen stops app_server drawing and skews the compositor.
for t in $(ps | awk '/screen_blanker/ && !/awk/ {print $(NF-3)}'); do kill $t; done
env "$@" "$summit" --profile "$p" "$url" > $l/video-$tag.log 2>&1 &
pid=$!
sleep $((secs - 10))
teams="$pid $(ps | awk -v n="$(dirname $summit)/" 'index($0, n) && !index($0, "/Summit") {print $(NF-3)}' | tr '\n' ' ')"
$d/threadstate 10000 2 $teams > $l/video-$tag.threads 2>&1
t=$($d/summitctl --team $pid state 2>/dev/null | sed -n 's/.*"title":"\([^"]*\)".*/\1/p')
echo "title: $t"
# The second sample holds each thread's CPU time over the 10 s.
awk '$NF == "ms" {v = $(NF-1); sub(/^\+/, "", v); if ($1 != first && first != "") ms[$2] += v; if (first == "") first = $1} END {for (t in ms) printf "team %s: %.2f cores\n", t, ms[t] / 10000}' $l/video-$tag.threads
t2=$($d/summitctl --team $pid state 2>/dev/null | sed -n 's/.*"title":"\([^"]*\)".*/\1/p')
echo "title: $t2"
$d/summitctl --team $pid quit > /dev/null 2>&1; sleep 4; kill -9 $pid 2>/dev/null
for t in $teams; do kill -9 $t 2>/dev/null; done
grep -a -E "Summit media|rpi_mmal|MSE.*decoder|did not take" $l/video-$tag.log | head -8

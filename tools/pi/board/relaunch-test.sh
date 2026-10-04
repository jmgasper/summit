#!/bin/sh
# relaunch-test.sh SUMMIT PROFILE URL WAIT [DELAY] : loads URL, asks the browser to
# quit after WAIT s, starts it again DELAY s later (default 2, with a page) and reports whether the new
# start got a window of its own while the old team may still be exiting.
summit=$1; p=$2; url=$3; wait=$4; d=/boot/home/summit-ec
l=${LOGS:-$d/logs}
SUMMIT_QUIT_TRACE=1 "$summit" --profile "$p" "$url" > $l/relaunch-1.log 2>&1 &
pid=$!
sleep $wait
$d/summitctl --team $pid quit > /dev/null
sleep ${5:-2}
old=$(ps | awk -v t=$pid '$(NF-3) == t {f=1} END {print f ? "still exiting" : "gone"}')
"$summit" --profile "$p" "file:///boot/home/summit-ec/pages/text.html" > $l/relaunch-2.log 2>&1 &
pid2=$!
sleep 8
alive=$(ps | awk -v t=$pid2 '$(NF-3) == t {f=1} END {print f ? "running" : "exited"}')
title=$($d/summitctl --team $pid2 state 2>/dev/null | sed -n 's/.*"title":"\([^"]*\)".*/\1/p')
echo "old browser 2 s after quit: $old; new start: $alive, window title \"$title\""
$d/summitctl --team $pid2 quit > /dev/null 2>&1; sleep 4; kill -9 $pid2 2>/dev/null

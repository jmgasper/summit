#!/bin/sh
# scroll-prof.sh BUNDLE PROFILE URL TAG [K=V...] : scroll-test with a system-wide
# profile of the 3 s around the wheel burst in logs/prof-TAG.txt.
b=$1; p=$2; url=$3; tag=$4; shift 4
for kv in "$@"; do export "$kv"; done
d=/boot/home/summit-ec
export SUMMIT_COMPOSITOR_TIMING_TRACE=1 SUMMIT_ENABLE_INPUT_SYNTHESIS=1
"$b/Summit" --profile "$p" "$url" > $d/logs/scroll-$tag.log 2>&1 &
pid=$!
sleep 14
profile -a -i 1000 -o $d/logs/prof-$tag.txt sleep 3 > /dev/null 2>&1 &
sleep 0.2
$d/summitctl --team $pid scroll 40 50 1 600 500 > /dev/null 2>&1
sleep 4
$d/summitctl --team $pid quit > /dev/null; sleep 3; kill -9 $pid 2>/dev/null
cp $d/logs/prof-$tag.txt /Documents/SummitPi/prof-$tag.txt

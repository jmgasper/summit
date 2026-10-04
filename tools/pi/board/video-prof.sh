#!/bin/sh
# video-prof.sh SUMMIT PROFILE URL TAG [K=V...] : plays URL for 15 s, then
# profiles the whole system for 10 s (logs/prof-TAG.txt, also copied to
# /Documents/SummitPi/prof-TAG.txt for tools/pi/profile-summary.py).
summit=$1; p=$2; url=$3; tag=$4; d=/boot/home/summit-ec
shift 4
l=${LOGS:-$d/logs}
# A blanked screen stops app_server drawing and skews the compositor.
for t in $(ps | awk '/screen_blanker/ && !/awk/ {print $(NF-3)}'); do kill $t; done
env "$@" "$summit" --profile "$p" "$url" > $l/video-prof-$tag.log 2>&1 &
pid=$!
sleep 15
profile -a -i 1000 -o $l/prof-$tag.txt sleep 10 > /dev/null 2>&1
$d/summitctl --team $pid quit > /dev/null 2>&1; sleep 4; kill -9 $pid 2>/dev/null
cp $l/prof-$tag.txt /Documents/SummitPi/prof-$tag.txt

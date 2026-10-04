#!/bin/sh
# session-test.sh BUNDLE PROFILE : opens 5 tabs, quits, relaunches without a URL
# and counts the web processes 8 s after the relaunch.
b=$1; p=$2
rm -rf "$p"
export SUMMIT_STARTUP_TRACE=1
"$b/Summit" --profile "$p" file:///boot/home/summit-ec/pages/text.html > /boot/home/summit-ec/logs/session-1.log 2>&1 &
pid=$!
sleep 6
for u in a b c d; do /boot/home/summit-ec/summitctl --team $pid newtab "file:///boot/home/summit-ec/pages/text.html?$u" > /dev/null; sleep 1; done
sleep 4
echo "first run web processes: $(ps | grep -c '[W]ebProcess')"
/boot/home/summit-ec/summitctl --team $pid quit > /dev/null; sleep 4; kill -9 $pid 2>/dev/null
/boot/home/summit-ec/stime "$b/Summit" --profile "$p" > /boot/home/summit-ec/logs/session-2.log 2>&1 &
pid=$!
sleep 8
echo "restored run web processes: $(ps | grep -c '[W]ebProcess')"
/boot/home/summit-ec/summitctl --team $pid state | head -c 600; echo
/boot/home/summit-ec/summitctl --team $pid quit > /dev/null; sleep 4; kill -9 $pid 2>/dev/null
grep -a "Summit startup" /boot/home/summit-ec/logs/session-2.log | head -20

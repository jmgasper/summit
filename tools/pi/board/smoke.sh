#!/bin/sh
# smoke.sh BUNDLE PROFILE : visits a few sites in one browser and reports
# each title and whether the browser and its web processes are still alive.
b=$1; p=$2; d=/boot/home/summit-ec
"$b/Summit" --profile "$p" about:blank > $d/logs/smoke.log 2>&1 &
pid=$!
sleep 6
for u in https://en.wikipedia.org/wiki/BeOS https://github.com/WebKit/WebKit https://www.bbc.com/news https://news.ycombinator.com/ https://www.youtube.com/ file:///boot/home/summit-ec/pages/video.html; do
  $d/summitctl --team $pid navigate "$u" > /dev/null; sleep 12
  t=$($d/summitctl --team $pid state 2>/dev/null | sed -n 's/.*"title":"\([^"]*\)".*/\1/p' | cut -c1-60)
  echo "$u -> ${t:-?} (web processes: $(ps | grep -c '[W]ebProcess'))"
done
$d/summitctl --team $pid quit > /dev/null; sleep 4
echo "browser left after quit: $(ps | grep -c "[r]d/$(basename $b)/Summit")"
kill -9 $pid 2>/dev/null
grep -a -i -E "crash|fatal|abort|segment|ASSERT" $d/logs/smoke.log | head -5
ls -t /boot/home/Desktop 2>/dev/null | head -3

#!/bin/sh
# kill-cycles.sh SUMMIT PROFILE N URL : N times: launch, load URL, quit,
# force-kill after 3 s (as the benchmark scripts do); after each round lists
# Summit processes left over and whether kill -9 removes them.
summit=$1; profile=$2; n=$3; url=$4; d=/boot/home/summit-ec
i=1
while [ $i -le $n ]; do
  "$summit" --profile "$profile" "$url" > $d/logs/kill-cycles.log 2>&1 &
  pid=$!
  sleep 10
  $d/summitctl --team $pid quit > /dev/null 2>&1; sleep 3; kill -9 $pid 2>/dev/null; sleep 2
  left=$(ps | grep -E "[N]etworkProcess|[W]ebProcess" | grep -v "summit-ec/rd" | awk '{print $(NF-3)}' | tr '\n' ' ')
  for t in $left; do kill -9 $t 2>/dev/null; done
  sleep 2
  stuck=$(ps | grep -E "[N]etworkProcess|[W]ebProcess" | grep -v "summit-ec/rd" | awk '{print $(NF-3)}' | tr '\n' ' ')
  echo "round $i: left [$left] stuck after kill [$stuck]"
  i=$((i + 1))
done

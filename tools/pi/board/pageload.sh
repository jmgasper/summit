#!/bin/sh
# pageload.sh BUNDLE PROFILE ROUNDS URL... : one browser on about:blank; each
# URL navigated ROUNDS times, printing seconds until the tab stops loading.
b=$1; p=$2; rounds=$3; shift 3
d=/boot/home/summit-ec
"$b/Summit" --profile "$p" about:blank > $d/logs/pageload.log 2>&1 &
pid=$!
sleep 6
r=1
while [ $r -le $rounds ]; do
  for url in "$@"; do
    $d/summitctl --team $pid navigate about:blank > /dev/null; sleep 2
    t0=$($d/stime /bin/true 2>&1 | awk '{print $3}')
    $d/summitctl --team $pid navigate "$url" > /dev/null
    sleep 0.3
    while :; do
      s=$($d/summitctl --team $pid state 2>/dev/null)
      case "$s" in *'"loading":true'*) sleep 0.1;; *) break;; esac
    done
    t1=$($d/stime /bin/true 2>&1 | awk '{print $3}')
    echo "$url round $r: $(echo "$t1 - $t0" | bc 2>/dev/null || awk -v a=$t0 -v b=$t1 'BEGIN {printf "%.2f", b - a}') s"
  done
  r=$((r + 1))
done
$d/summitctl --team $pid quit > /dev/null; sleep 3; kill -9 $pid 2>/dev/null

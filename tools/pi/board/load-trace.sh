#!/bin/sh
# load-trace.sh BUNDLE PROFILE URL TAG [K=V...] : browser on about:blank, then
# loads URL with the given environment; log in logs/load-TAG.log.
b=$1; p=$2; url=$3; tag=$4; shift 4
for kv in "$@"; do export "$kv"; done
d=/boot/home/summit-ec
"$b/Summit" --profile "$p" about:blank > $d/logs/load-$tag.log 2>&1 &
pid=$!
sleep 6
t0=$($d/stime /bin/true 2>&1 | awk '{print $3}')
$d/summitctl --team $pid navigate "$url" > /dev/null
sleep 0.3
while :; do
  s=$($d/summitctl --team $pid state 2>/dev/null)
  case "$s" in *'"loading":true'*) sleep 0.2;; *) break;; esac
done
t1=$($d/stime /bin/true 2>&1 | awk '{print $3}')
awk -v a=$t0 -v b=$t1 'BEGIN {printf "loaded in %.2f s\n", b - a}'
sleep 3
$d/summitctl --team $pid quit > /dev/null; sleep 3; kill -9 $pid 2>/dev/null

#!/bin/sh
# idle-cpu.sh BUNDLE PROFILE URL SETTLE MEASURE [K=V...] : launches Summit on
# URL, waits SETTLE s, reports CPU of its processes over MEASURE s and memory.
b=$1; p=$2; url=$3; settle=$4; measure=$5; shift 5
for kv in "$@"; do export "$kv"; done
d=/boot/home/summit-ec
"$b/Summit" --profile "$p" "$url" > $d/logs/idle.log 2>&1 &
pid=$!
sleep "$settle"
$d/cpu-snap.sh $d/logs/cpu-a.txt; sleep "$measure"; $d/cpu-snap.sh $d/logs/cpu-b.txt
$d/cpu-diff.sh $d/logs/cpu-a.txt $d/logs/cpu-b.txt "$measure" "Summit|WebProcess|NetworkProcess|app_server"
for t in $(awk -F'\t' '$2 ~ /WebProcess|NetworkProcess|\/Summit/ {print $1}' $d/logs/cpu-b.txt | sort -u); do
  listarea $t 2>/dev/null | awk -v t=$t 'NF >= 8 && $1 ~ /^[0-9]+$/ {ram += strtonum("0x" $(NF-5))} END {printf "team %s areas %.1f MiB allocated\n", t, ram / 1048576}'
done
$d/summitctl --team $pid quit > /dev/null; sleep 3; kill -9 $pid 2>/dev/null

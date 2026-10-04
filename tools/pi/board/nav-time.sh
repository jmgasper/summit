#!/bin/sh
# nav-time.sh SUMMIT PROFILE FROM TO PREFIX : loads FROM, waits 12 s, then
# navigates to TO and prints the seconds until the title starts with PREFIX.
summit=$1; p=$2; from=$3; to=$4; prefix=$5; d=/boot/home/summit-ec
"$summit" --profile "$p" "$from" > $d/logs/nav-time.log 2>&1 &
pid=$!
sleep 14
t0=$($d/stime /bin/true 2>&1 | awk '{print $3}')
$d/summitctl --team $pid navigate "$to" > /dev/null
r=timeout; i=0
while [ $i -lt 120 ]; do
  sleep 0.25; i=$((i + 1))
  case "$($d/summitctl --team $pid state 2>/dev/null)" in *"\"title\":\"$prefix"*)
    t1=$($d/stime /bin/true 2>&1 | awk '{print $3}'); r=$(awk -v a=$t0 -v b=$t1 'BEGIN {printf "%.2f s", b - a}'); break;; esac
done
echo "$r"
$d/summitctl --team $pid quit > /dev/null 2>&1; sleep 4; kill -9 $pid 2>/dev/null

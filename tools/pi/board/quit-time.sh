#!/bin/sh
# quit-time.sh BUNDLE PROFILE URL : seconds from summitctl quit until the browser team is gone.
b=$1; p=$2; url=$3; d=/boot/home/summit-ec
"$b/Summit" --profile "$p" "$url" > $d/logs/quit.log 2>&1 &
pid=$!
sleep 12
t0=$($d/stime /bin/true 2>&1 | awk '{print $3}')
$d/summitctl --team $pid quit > /dev/null
n=0
while ps | awk -v t=$pid '$(NF-3) == t {f=1} END {exit !f}'; do sleep 0.1; n=$((n+1)); [ $n -gt 300 ] && break; done
t1=$($d/stime /bin/true 2>&1 | awk '{print $3}')
awk -v a=$t0 -v b=$t1 -v u="$url" 'BEGIN {printf "quit took %.2f s (%s)\n", b - a, u}'
sleep 2; echo "left: $(ps | grep -c "[r]d/$(basename $b)/")"

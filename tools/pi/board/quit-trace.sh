#!/bin/sh
# quit-trace.sh SUMMIT PROFILE URL [WAIT] [K=V...] : loads URL, waits WAIT
# seconds (default 25), asks the browser to quit and prints how long its team
# lived on, with the browser's own lines about closing (logs/quit-trace.log).
# SUMMIT is the executable (an installed /boot/system/apps/Summit, or
# $RD/b-NAME/Summit). Whatever is left after 40 s is killed and listed.
summit=$1; p=$2; url=$3; wait=${4:-25}; d=/boot/home/summit-ec
shift 4 2>/dev/null
: > $d/logs/quit-trace.log
before=$(ps | grep -E "[W]ebProcess|[N]etworkProcess" | awk '{print $(NF-3)}' | tr '\n' ' ')
env SUMMIT_QUIT_TRACE=1 "$@" "$summit" --profile "$p" "$url" >> $d/logs/quit-trace.log 2>&1 &
pid=$!
sleep $wait
title=$($d/summitctl --team $pid state 2>/dev/null | sed -n 's/.*"title":"\([^"]*\)".*/\1/p' | cut -c1-50)
t0=$($d/stime /bin/true 2>&1 | awk '{print $3}')
$d/summitctl --team $pid quit > /dev/null
n=0
while ps | awk -v t=$pid '$(NF-3) == t {f=1} END {exit !f}'; do sleep 0.1; n=$((n+1)); [ $n -gt 400 ] && break; done
t1=$($d/stime /bin/true 2>&1 | awk '{print $3}')
awk -v a=$t0 -v b=$t1 -v u="$url" -v t="$title" 'BEGIN {printf "quit took %.2f s (%s, \"%s\")\n", b - a, u, t}'
sleep 2
left=""
for x in $(ps | grep -E "[W]ebProcess|[N]etworkProcess" | awk '{print $(NF-3)}'); do
  case " $before " in *" $x "*) ;; *) left="$left $x";; esac
done
echo "helpers left 2 s later:${left:- none}"
kill -9 $pid 2>/dev/null
for x in $left; do kill -9 $x 2>/dev/null; done
grep -a -E "quit|close|Close|TryClose|tryClose" $d/logs/quit-trace.log | head -30

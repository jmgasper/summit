#!/bin/sh
# quit-strace.sh SUMMIT PROFILE URL WAIT TAG : loads URL, after WAIT seconds
# attaches strace (all threads, syscall durations) to the browser and to its
# busiest web process, asks the browser to quit, and keeps the traces in
# logs/strace-TAG-{ui,web}.txt (NO_STRACE=1: none). The browser's output:
# logs/quit-strace-TAG.log; when the teams went: logs/quit-strace-TAG.times.
# THREADS=1 also samples their threads (threadstate).
summit=$1; p=$2; url=$3; wait=$4; tag=$5; d=/boot/home/summit-ec
log=$d/logs/quit-strace-$tag.log
: > $log
env SUMMIT_QUIT_TRACE=1 "$summit" --profile "$p" "$url" >> $log 2>&1 &
pid=$!
sleep $wait
# The web process with the most threads is the page's (the spare one is idle).
web=$(ps | awk -v n="$(dirname $summit)/WebProcess" 'index($0, n) {print $(NF-2), $(NF-3)}' | sort -n | tail -1 | awk '{print $2}')
if [ -z "$NO_STRACE" ]; then
  strace -T -o $d/logs/strace-$tag-ui.txt $pid > /dev/null 2>&1 &
  [ -n "$web" ] && { strace -T -o $d/logs/strace-$tag-web.txt $web > /dev/null 2>&1 & }
fi
# THREADS=1: thread states of both teams every 250 ms (logs/quit-strace-TAG.threads).
[ -n "$THREADS" ] && { $d/threadstate 250 400 $pid $web > $d/logs/quit-strace-$tag.threads 2>&1 & }
sleep 0.5
t0=$($d/stime /bin/true 2>&1 | awk '{print $3}')
echo "quit asked at $t0 (browser $pid, web $web)" > $d/logs/quit-strace-$tag.times
$d/summitctl --team $pid quit > /dev/null
n=0; webgone=""
while ps | awk -v t=$pid '$(NF-3) == t {f=1} END {exit !f}'; do
  if [ -z "$webgone" ] && ! ps | awk -v t=$web '$(NF-3) == t {f=1} END {exit !f}'; then
    webgone=$($d/stime /bin/true 2>&1 | awk '{print $3}'); echo "web process gone at $webgone" >> $d/logs/quit-strace-$tag.times
  fi
  sleep 0.1; n=$((n+1)); [ $n -gt 600 ] && break
done
t1=$($d/stime /bin/true 2>&1 | awk '{print $3}')
echo "browser gone at $t1" >> $d/logs/quit-strace-$tag.times
awk -v a=$t0 -v b=$t1 'BEGIN {printf "quit took %.2f s\n", b - a}'
sleep 1
cat $d/logs/quit-strace-$tag.times
kill -9 $pid 2>/dev/null
[ -n "$web" ] && kill -9 $web 2>/dev/null
grep -a "Summit quit\|quit asked" $log

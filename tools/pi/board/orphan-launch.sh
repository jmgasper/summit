#!/bin/sh
# orphan-launch.sh SUMMIT PROFILE [SKIP] : starts the browser and kills it
# while it starts a helper (killonspawn; SKIP helpers are let through first),
# then lists that bundle's helpers whose threads are all suspended, 3 s later.
summit=$1; p=$2; skip=${3:-0}; d=/boot/home/summit-ec
"$summit" --profile "$p" about:blank > /dev/null 2>&1 &
pid=$!
$d/killonspawn $pid $skip
sleep 3
for t in $(ps | awk -v n="$(dirname $summit)/" 'index($0, n) && !index($0, "/Summit ") {print $(NF-3)}'); do
  $d/threadstate 100 1 $t > /tmp/orphan-threads.txt
  n=$(wc -l < /tmp/orphan-threads.txt); s=$(grep -c " susp " /tmp/orphan-threads.txt)
  echo "team $t: $n threads, $s suspended"
done

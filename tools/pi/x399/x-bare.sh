#!/bin/sh
# x-bare.sh ROUNDS BUNDLE... : time of a bare WebProcess / NetworkProcess (loads its libraries, exits on missing arguments).
rounds=$1; shift
M=/boot/home/summit-mesa/prefix-20261002
r=1
while [ $r -le $rounds ]; do
  for b in "$@"; do
    for exe in WebProcess NetworkProcess; do
      s=$(/boot/home/summit/claude-ec/stime /bin/true 2>&1 | awk '{print $3}')
      LIBRARY_PATH="$M/lib:$b/lib:/boot/system/lib" $b/$exe > /dev/null 2>&1
      e=$(/boot/home/summit/claude-ec/stime /bin/true 2>&1 | awk '{print $3}')
      awk -v a=$s -v z=$e -v b=$(basename $b) -v x=$exe 'BEGIN { printf "%s %s %.1f ms\n", b, x, (z - a) * 1000 }'
    done
  done
  r=$((r + 1))
done

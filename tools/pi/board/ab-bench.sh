#!/bin/sh
# ab-bench.sh ROUNDS URL PREFIX OUT A B... : alternates bundles A B... (names in the RAM disk) on a title benchmark.
rounds=$1; url=$2; prefix=$3; out=$4; shift 4
d=/boot/home/summit-ec
: > $out
r=1
while [ $r -le $rounds ]; do
  for b in "$@"; do
    echo "$b $($d/bench-title.sh ${RD:-$d/rd}/$b/Summit ${RD:-$d/rd}/p-bench-$b "$url" "$prefix" 90)" >> $out
  done
  r=$((r + 1))
done
echo done >> $out

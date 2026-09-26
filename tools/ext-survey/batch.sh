#!/bin/sh
# batch.sh LISTFILE : each line "name package url files|nofiles wait"; stops when BENCH_LOCK appears.
S=/boot/home/summit/ext-survey
while read N P U F W; do
  case "$N" in ''|'#'*) continue;; esac
  if test -e /boot/home/summit/BENCH_LOCK; then echo "LOCKED before $N"; exit 9; fi
  if test -e $S/done-$N; then continue; fi
  echo "===== $N"
  $S/tools/run-ext.sh $N $S/packages/$P "$U" $F $W < /dev/null
  touch $S/done-$N
done < $1
echo "BATCH DONE"

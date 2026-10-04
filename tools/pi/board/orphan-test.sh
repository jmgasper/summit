#!/bin/sh
# orphan-test.sh SUMMIT PROFILE URL [URL2] : loads URL (then URL2, another
# site, 12 s later), force-kills the browser (QUIT_FIRST=1: 4 s after asking
# it to quit), and lists its helper processes still alive 2, 6 and 15 s later.
summit=$1; p=$2; url=$3; url2=$4; d=/boot/home/summit-ec
before=$(ps | grep -E "[W]ebProcess|[N]etworkProcess" | awk '{print $(NF-3)}' | tr '\n' ' ')
"$summit" --profile "$p" "$url" > $d/logs/orphan.log 2>&1 &
pid=$!
sleep 12
if [ -n "$url2" ]; then $d/summitctl --team $pid navigate "$url2" > /dev/null; sleep 10; fi
if [ -n "$QUIT_FIRST" ]; then $d/summitctl --team $pid quit > /dev/null 2>&1; sleep 4; fi
kill -9 $pid
for s in 2 4 9; do
  sleep $s
  now=$(ps | grep -E "[W]ebProcess|[N]etworkProcess" | awk '{print $(NF-3) ":" $1}' | tr '\n' ' ')
  left=""
  for x in $now; do case " $before " in *" ${x%%:*} "*) ;; *) left="$left $x";; esac; done
  echo "after +$s s:$left"
done
for x in $left; do kill -9 ${x%%:*} 2>/dev/null; done

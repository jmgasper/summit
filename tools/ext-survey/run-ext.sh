#!/bin/sh
# run-ext.sh NAME PACKAGE URL [files|nofiles] [WAIT] [keep]
# fresh profile, install PACKAGE through the Extensions window, open URL, screenshot, quit (unless keep).
S=${SURVEY_ROOT:-/boot/home/summit/ext-survey}
E=$S/tools/extctl
C=${SURVEY_CTL:-/boot/home/summit/tmp/claude-summitctl/summitctl}
# Window frames: the X399's right monitor by default.
BROWSER_FRAME=${SURVEY_FRAME:-1960 60 3800 1040}
MANAGER_FRAME=${SURVEY_MANAGER_FRAME:-2000 100 2690 670}
MANAGER_AWAY=${SURVEY_MANAGER_AWAY:-3000 700 3690 1270}
N=$1; PKG=$2; URL=${3:-about:blank}; FILES=${4:-nofiles}; WAIT=${5:-12}; KEEP=$6
if test -e /boot/home/summit/BENCH_LOCK; then echo LOCKED; exit 9; fi
T=$(FRESH=1 $S/tools/launch.sh $N about:blank)
echo "team $T"
sleep 5
$C --team $T frame $BROWSER_FRAME
$E --team $T show; sleep 1
$E --team $T wframe Extensions $MANAGER_FRAME
$E --team $T install $PKG
i=0; while test $i -lt 60; do if $E --team $T ready >/dev/null 2>&1; then break; fi; sleep 1; i=$((i+1)); done; echo "review ready after ${i}s"
if test "$FILES" = files; then $E --team $T approve files; else $E --team $T approve; fi
sleep $WAIT
echo "--- catalog"; grep -c '"identifier"' $S/profile-$N/Extensions/catalog.json 2>/dev/null || echo "NOT INSTALLED"
$E --team $T wframe Extensions $MANAGER_AWAY
if test "$URL" != about:blank; then $C --team $T navigate "$URL"; sleep $WAIT; fi
$C --team $T state | cut -c1-600
screenshot -s -f png $S/shot-$N.png >/dev/null 2>&1
if test -z "$KEEP"; then
  $C --team $T quit; sleep 4
  if ps | grep -q " $T "; then kill $T; sleep 1; fi
fi
L=$(wc -l < $S/log-$N.txt); echo "--- log ($L lines)"
if test $L -gt 5000; then head -3000 $S/log-$N.txt > $S/l.tmp; echo "... truncated from $L lines" >> $S/l.tmp; mv $S/l.tmp $S/log-$N.txt; fi

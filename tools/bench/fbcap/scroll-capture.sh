#!/bin/sh
# scroll-capture.sh LABEL URL FRAMES [K=V...] (on the workstation: /boot/home/summit/claude-fl-cap.sh): load URL in a private Summit, scroll steadily
# (FL_NOTCHES notches, FL_MS apart, FL_DELTA lines each; then back up if FL_BACK=1) and
# capture the post column of the real frame buffer FRAMES times meanwhile (bench/cap-LABEL.raw).
cd /boot/home/summit
L=$1; U=$2; N=$3; shift 3
B=${FL_BUNDLE:-/boot/home/summit/claude-fl-bundle}
C=/boot/home/summit/claude-ctl/summitctl-devtools
sh claude-wake.sh
LOG=/boot/home/summit/claude-fl-$L.log
T=$(sh claude-run.sh $B /boot/home/summit/claude-fl-profile $LOG about:blank "$@")
echo team $T
sleep 6
$C --team $T frame 40 60 1500 1000
sleep 2
$C --team $T navigate "$U"
sleep ${FL_WAIT:-14}
$C --team $T --timeout-ms 60000 scroll 30 50 3 700 540
sleep 6
$C --team $T --timeout-ms 200000 scroll ${FL_NOTCHES:-150} ${FL_MS:-100} ${FL_DELTA:-1} 700 540 > /dev/null
./claude-fbcap ${FL_REGION:-676 365 1224 1340} ${FL_STEP:-8} $N 0 bench/cap-$L.raw
sleep 2
if [ -n "$FL_BACK" ]; then
  $C --team $T --timeout-ms 200000 scroll ${FL_NOTCHES:-150} ${FL_MS:-100} -${FL_DELTA:-1} 700 540 > /dev/null
  ./claude-fbcap ${FL_REGION:-676 365 1224 1340} ${FL_STEP:-8} $N 0 bench/cap-$L-back.raw
fi
sleep ${FL_SLEEP:-1}
[ -n "$FL_KEEP" ] || { $C --team $T quit; sleep 3; }

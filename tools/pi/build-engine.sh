#!/usr/bin/env bash
# Incremental build of the Pi's GL engine (rpi4/summit-gl) in a memory-capped
# scope. Log: rpi4/summit-gl/ninja-ec-<tag>.log, last line "ninja exit N".
W=/mnt/HaikuWork/build/summit-arm64
B=/mnt/HaikuWork/rpi4/summit-gl
TAG=${1:?tag}; shift
. $W/hosttools/env.sh
JOBS=${JOBS:-9}
LOG=$B/ninja-ec-$TAG.log
{
  systemd-run --user --scope -q -p MemoryHigh=20G -p MemoryMax=24G \
    nice -n 10 ninja -C $B/WebKitBuild -k 0 -j$JOBS "$@"
  echo "ninja exit $?"
} > "$LOG" 2>&1

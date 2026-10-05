#!/usr/bin/env bash
# Detached arm64 engine build (survives the agent's shell); log in ninja-<tag>.log.
W=/mnt/HaikuWork/build/summit-arm64
. $W/hosttools/env.sh
TAG=${1:-current}
JOBS=${JOBS:-9}
systemd-run --user --scope -q -p MemoryHigh=20G -p MemoryMax=24G \
    nice -n 10 ninja -C /home/jmgasper/summit-arm64-build/WebKitBuild -k 0 -j$JOBS > $W/ninja-$TAG.log 2>&1
echo "ninja exit $?" >> $W/ninja-$TAG.log

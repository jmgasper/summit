#!/usr/bin/env bash
# Incremental arm64 engine build, held in a memory-capped scope so a runaway
# compile is killed here rather than the OOM killer taking someone's VM.
W=/mnt/HaikuWork/build/summit-arm64
. $W/hosttools/env.sh
JOBS=${JOBS:-10}
exec systemd-run --user --scope -q -p MemoryHigh=16G -p MemoryMax=20G \
    nice -n 10 ninja -C $W/WebKitBuild -k 0 -j$JOBS "$@"

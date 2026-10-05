#!/bin/sh
# Stop any running arm64 Summit, then start it with the crash reporter preloaded.
B=/boot/home/summit-arm64
for t in $(ps | grep -E "summit-arm64/(Summit|WebProcess|NetworkProcess)" | grep -v grep | awk '{print $(NF-3)}'); do kill -9 $t; done
sleep 2
(env WEBKIT_EXEC_PATH=$B LIBRARY_PATH=$B/lib:/boot/system/non-packaged/lib:/boot/system/lib \
	LD_PRELOAD=$B/lib/libsummitcrash.so $B/Summit "$@" \
	> /boot/home/summit-arm64-run.log 2>&1 &)

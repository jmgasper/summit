#!/bin/sh
# x-speedo.sh BUNDLE PROFILE TAG : one Speedometer 3.1 run (5 iterations) from the host's server; the server stores the result.
b=$1; p=$2; tag=$3
rm -rf "$p"
/boot/home/summit/claude-ec/x-launch.sh "$b" "$p" /boot/home/summit/claude-ec/logs/speedo-$tag.log 300 "http://192.168.1.64:8961/?startAutomatically=true&iterationCount=5" SUMMIT_COMPOSITOR_TIMING_TRACE=0 SUMMIT_STARTUP_TRACE=0 SUMMIT_EXTENSION_TIMING=0

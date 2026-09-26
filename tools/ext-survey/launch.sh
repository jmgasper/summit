#!/bin/sh
# launch.sh NAME [URL]  -> fresh profile /boot/home/summit/ext-survey/profile-NAME, log log-NAME.txt, prints team id
S=/boot/home/summit/ext-survey
if test -e /boot/home/summit/BENCH_LOCK; then echo LOCKED; exit 9; fi
B=/boot/home/summit/build-modern-browser/ext-survey-bundle
N=$1; shift
URL=${1:-about:blank}
P=$S/profile-$N
if test -n "$FRESH"; then rm -rf "$P"; fi
mkdir -p "$P"
cd $S
env WEBKIT_EXEC_PATH=$B LIBRARY_PATH=/boot/home/summit-mesa/prefix/lib:$B/lib:/boot/system/lib SUMMIT_SKIA_GL_CONTEXT=1 \
SUMMIT_TRACE_EXTENSION_CONSOLE=1 SUMMIT_TRACE_PAGE_CONSOLE=${PAGECONSOLE:-1} $DEVMODE \
  $B/Summit --profile "$P" "$URL" > $S/log-$N.txt 2>&1 &
echo $!

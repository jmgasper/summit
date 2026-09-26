#!/bin/sh
# launch.sh NAME [URL]  -> fresh profile /boot/home/summit/ext-survey/profile-NAME, log log-NAME.txt, prints team id
# Defaults are the X399's; SURVEY_ROOT, SURVEY_BUNDLE and SURVEY_MESA select another machine.
S=${SURVEY_ROOT:-/boot/home/summit/ext-survey}
if test -e /boot/home/summit/BENCH_LOCK; then echo LOCKED; exit 9; fi
B=${SURVEY_BUNDLE:-/boot/home/summit/build-modern-browser/ext-survey-bundle}
M=${SURVEY_MESA:-/boot/home/summit-mesa/prefix/lib}
N=$1; shift
URL=${1:-about:blank}
P=$S/profile-$N
if test -n "$FRESH"; then rm -rf "$P"; fi
mkdir -p "$P"
cd $S
env WEBKIT_EXEC_PATH=$B LIBRARY_PATH=$M:$B/lib:/boot/system/lib SUMMIT_SKIA_GL_CONTEXT=1 \
SUMMIT_TRACE_EXTENSION_CONSOLE=1 SUMMIT_TRACE_PAGE_CONSOLE=${PAGECONSOLE:-1} $DEVMODE \
  $B/Summit --profile "$P" "$URL" > $S/log-$N.txt 2>&1 &
echo $!

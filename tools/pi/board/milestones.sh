#!/bin/sh
# milestones.sh LOG : start-up lines of a pi-launch log, in seconds after launch
awk '/^Summit launch/ { l = $3 }
  /^Summit startup/ { printf "%6.3f  %s\n", $3 - l, substr($0, index($0, "pid=")) }
  /^Summit browser timing/ { printf "%6.3f  browser: %s\n", $4 - l, substr($0, index($0, $5)) }
  /^Summit shader warm-up/ { print "        " $0 }' "$1"

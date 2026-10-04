#!/bin/sh
# tiles-frame.sh LOG... : paint and total of the first composition with tiles in each log
for f in "$@"; do
  awk -v f="$f" '/compositor timing/ && / tiles=[1-9]/ && done == 0 { for (k = 1; k <= NF; k++) if ($k ~ /^paint=|^total=/) printf "%s ", $k; print f; done = 1 }' "$f"
done

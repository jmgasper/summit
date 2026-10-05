#!/bin/sh
# x-ab-start.sh BUNDLE PROFILE TAG URL [K=V...] : one launch; prints browser code, window, first frame with tiles.
b=$1; profile=$2; tag=$3; url=$4; shift 4
log=/boot/home/summit/claude-ec/logs/$tag.log
mkdir -p /boot/home/summit/claude-ec/logs
/boot/home/summit/claude-ec/x-launch.sh "$b" "$profile" "$log" 8 "$url" "$@" > /dev/null
awk -v tag="$tag" '
  /^Summit launch/ { l = $3 }
  /browser ready to run/ && !r { r = $4 - l }
  /browser window shown/ && !w { w = $4 - l }
  /Summit compositor timing:/ && !f && / tiles=[1-9]/ { for (k = 1; k <= NF; k++) if ($k ~ /^at=/) { split($k, a, "="); f = a[2] / 1e6 - l } }
  END { printf "%s ready %.3f window %.3f first-frame %.3f\n", tag, r, w, f }' "$log"

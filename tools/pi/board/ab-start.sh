#!/bin/sh
# ab-start.sh SUMMIT PROFILE TAG ROUNDS URL [K=V...] : warm launches, prints
# ready / window / first-frame seconds after launch for each round.
summit=$1; profile=$2; tag=$3; rounds=$4; url=$5; shift 5
i=1
while [ $i -le $rounds ]; do
  log=/boot/home/summit-ec/logs/$tag-$i.log
  /boot/home/summit-ec/pi-launch.sh "$summit" "$profile" "$log" 12 "$url" "$@" > /dev/null
  awk -v tag="$tag-$i" '
    /^Summit launch/ { l = $3 }
    /browser ready to run/ && !r { r = $4 - l }
    /browser window shown/ && !w { w = $4 - l }
    /Summit compositor timing:/ && !f && / tiles=[1-9]/ { for (k = 1; k <= NF; k++) if ($k ~ /^at=/) { split($k, a, "="); f = a[2] / 1e6 - l } }
    END { printf "%s ready %.2f window %.2f first-frame %.2f\n", tag, r, w, f }' "$log"
  i=$((i + 1))
done

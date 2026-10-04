#!/bin/sh
# style-sum.sh LOG : style passes over 50 ms and the total style/layout time of the page
awk '/Summit style phases:/ && !/about:blank/ { for (k = 1; k <= NF; k++) { split($k, kv, "="); v[kv[1]] = kv[2] }
       s += v["total"]; n++; if (v["total"] >= 50) big = big sprintf(" %.0f", v["total"]) }
     /Summit layout: context/ { for (k = 1; k <= NF; k++) { split($k, kv, "="); w[kv[1]] = kv[2] } l += w["total"] }
     END { printf "style %d passes %.0f ms (big:%s), layout %.0f ms\n", n, s, big, l }' "$1"

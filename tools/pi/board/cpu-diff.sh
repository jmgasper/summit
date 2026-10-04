#!/bin/sh
# cpu-diff.sh A B SECONDS PATTERN : CPU used between two cpu-snap.sh files by
# teams whose name matches PATTERN, per team and the busiest threads.
awk -F'\t' -v secs="$3" -v pat="$4" '
  NR == FNR { a[$3] = $5; next }
  $2 ~ pat { d = $5 - a[$3]; team[$1 " " $2] += d; if (d > 0) th[$1 " " $4] += d }
  END { for (t in team) printf "team %-60s %7.1f ms  %5.1f%% of a core\n", t, team[t], team[t] / (secs * 10);
        for (t in th) if (th[t] >= secs * 2) printf "   thread %-50s %7.1f ms\n", t, th[t] }' "$1" "$2"

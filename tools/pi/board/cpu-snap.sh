#!/bin/sh
# cpu-snap.sh OUT : one line per thread: team-id team-name | thread-id thread-name | cpu-ms
ps -a | awk '
  /^Team / { want_team = 1; next }
  /^Thread / { next }
  NF == 0 { next }
  want_team && NF < 5 { want_team = 0; next }
  want_team { tid = $(NF-3); $(NF-3) = $(NF-2) = $(NF-1) = $NF = ""; tname = $0; gsub(/ +$/, "", tname); want_team = 0; next }
  NF < 6 { next }
  { ms = $(NF-1) + $NF; id = $(NF-4); $(NF-4) = $(NF-3) = $(NF-2) = $(NF-1) = $NF = ""; n = $0; gsub(/ +$/, "", n);
    printf "%s\t%s\t%s\t%s\t%d\n", tid, tname, id, n, ms }' > "$1"

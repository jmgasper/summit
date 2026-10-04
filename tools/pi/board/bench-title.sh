#!/bin/sh
# bench-title.sh SUMMIT PROFILE URL PREFIX SECONDS : launches SUMMIT on URL
# and waits (at most SECONDS) for a tab title starting with PREFIX, which a
# benchmark page sets to its result; prints it, then quits the browser.
summit=$1; profile=$2; url=$3; prefix=$4; secs=$5
d=/boot/home/summit-ec
"$summit" --profile "$profile" "$url" > $d/logs/bench-title.log 2>&1 &
pid=$!
t=0
result=timeout
while [ $t -lt $secs ]; do
  sleep 1; t=$((t + 1))
  s=$($d/summitctl --team $pid state 2>/dev/null)
  case "$s" in *"\"title\":\"$prefix"*)
    result=$(echo "$s" | sed 's/.*"title":"\([^"]*\)".*/\1/'); break;;
  esac
done
echo "$result"
$d/summitctl --team $pid quit > /dev/null 2>&1; sleep 3; kill -9 $pid 2>/dev/null

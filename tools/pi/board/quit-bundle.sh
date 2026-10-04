#!/bin/sh
# quit-bundle.sh NAME : asks the Summit of RAM-disk bundle NAME to quit, then kills what is left of it.
for t in $(ps | awk -v n="rd/$1/" 'index($0, n) && index($0, "/Summit") {print $(NF-3)}'); do
  /boot/home/summit-ec/summitctl --team $t quit > /dev/null 2>&1
done
sleep 4
for t in $(ps | awk -v n="rd/$1/" 'index($0, n) {print $(NF-3)}'); do kill -9 $t 2>/dev/null; done
ps | grep -c "rd/$1/"

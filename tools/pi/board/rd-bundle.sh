#!/bin/sh
# rd-bundle.sh NAME : copies /Documents/SummitPi/NAME into the RAM disk
# /boot/home/summit-ec/rd (mounted on first use) as a runnable bundle.
name=$1
rd=/boot/home/summit-ec/rd
mkdir -p $rd
if ! df $rd 2>/dev/null | grep -q ramfs; then mount -t ramfs $rd || exit 1; fi
rm -rf $rd/$name
cp -a /Documents/SummitPi/$name $rd/$name || exit 1
chmod 755 $rd/$name/Summit $rd/$name/WebProcess $rd/$name/NetworkProcess
ln -sf libWebKit.so.1 $rd/$name/lib/libWebKit.so
echo "$rd/$name ready"; du -sh $rd/$name

#!/bin/sh
# sd-bundle.sh NAME [FILE...] : copies /Documents/SummitPi/NAME to the SD card
# as a runnable bundle in /boot/home/summit-ec/sd/NAME (the share cannot be
# mapped, and the RAM disks hang when a force-killed helper ran from them).
# With FILEs (paths inside the bundle, such as lib/libWebKit.so.1) only those
# are copied over an existing copy: the card writes ~1.4 MB/s.
name=$1; shift
src=/Documents/SummitPi/$name; dst=/boot/home/summit-ec/sd/$name
mkdir -p /boot/home/summit-ec/sd
if [ $# -eq 0 ]; then
  rm -rf $dst
  cp -a $src $dst || exit 1
else
  for f in "$@"; do cp $src/$f $dst/$f || exit 1; done
fi
chmod 755 $dst/Summit $dst/WebProcess $dst/NetworkProcess
ln -sf libWebKit.so.1 $dst/lib/libWebKit.so
sync
echo "$dst ready"

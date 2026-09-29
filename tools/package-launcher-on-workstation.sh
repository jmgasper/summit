#!/usr/bin/env bash
# Make the Deskbar's Applications > Summit entry run the installed build: the
# `summit` package's /boot/system/apps/Summit becomes a script that runs the
# desktop launcher (/boot/home/Desktop/Summit-current.sh), which
# install-on-workstation.sh keeps pointing at the latest bundle. Replaces the
# 2026-09-21 preview package, whose apps/Summit was the legacy-engine browser.
#
#   bash tools/package-launcher-on-workstation.sh
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=0.1.0~dev2-1
bash "$SUMMIT_ROOT/tools/ws.sh" 'mkdir -p /boot/home/summit/tmp/launcher-package && cat > /boot/home/summit/tmp/launcher-package/Summit.hvif' < "$SUMMIT_ROOT/resources/Summit.hvif"
bash "$SUMMIT_ROOT/tools/ws.sh" "set -e
cd /boot/home/summit/tmp/launcher-package
rm -rf stage; mkdir -p stage/apps stage/data/deskbar/menu/Applications
cat > stage/apps/Summit <<'SH'
#!/bin/sh
# Summit from the applications menu: the build the desktop launcher points at.
exec /boot/home/Desktop/Summit-current.sh \"\$@\"
SH
chmod +x stage/apps/Summit
addattr -f Summit.hvif -t icon BEOS:ICON stage/apps/Summit
ln -s ../../../../apps/Summit stage/data/deskbar/menu/Applications/Summit
cat > stage/.PackageInfo <<'PI'
name summit
version $version
architecture x86_64
summary \"A native WebKit browser for KunanyiOS\"
description \"Summit is a browser in development for KunanyiOS and Haiku. This package puts Summit in the applications menu; it runs the build installed by Summit's workstation install script (/boot/home/Desktop/Summit-current.sh).\"
packager \"KunanyiOS contributors\"
vendor \"KunanyiOS\"
copyrights { \"2026 KunanyiOS contributors\" }
licenses { \"MIT\" }
provides {
    summit = ${version%-*}
    app:Summit = ${version%-*}
}
requires {
    haiku >= r1~beta6
}
PI
rm -f summit-$version-x86_64.hpkg
package create -C stage summit-$version-x86_64.hpkg
package list summit-$version-x86_64.hpkg | grep -E 'apps/Summit|deskbar'
if pkgman list-installed 2>/dev/null | grep -q '^summit\b' || ls /boot/system/packages | grep -q '^summit-'; then pkgman uninstall -y summit; fi
pkgman install -y ./summit-$version-x86_64.hpkg
sleep 2
ls -la /boot/system/apps/Summit /boot/system/data/deskbar/menu/Applications/Summit
head -3 /boot/system/apps/Summit
listattr /boot/system/apps/Summit | grep ICON"

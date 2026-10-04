#!/usr/bin/env bash
# mkbundle.sh TAG NAME : packages the Pi engine whose build log is
# rpi4/summit-gl/ninja-ec-TAG.log, and Summit from build/summit-arm64/summit,
# with the air/OS packaging script of the rpi4 fork (plus GNU hash tables for
# Summit's own executable), then stages both as a runnable bundle in
# /mnt/Documents/SummitPi/NAME (Summit with RPATH $ORIGIN/lib). The Pi sees
# the share as /Documents; board/rd-bundle.sh copies it to a RAM disk there.
#   DEPS_PREFIX / TLS_PREFIX: dependency prefixes (build-deps-gnu-hash.sh's
#   prefix for both gives every bundled library a GNU hash table).
#   OUT_PACKAGES=dir: keep the .hpkg files there too.
set -euo pipefail
TAG=$1 NAME=$2
H=${SUMMIT_PI_WORK:-/mnt/HaikuWork/tmp/summit-ec}
FORK_SCRIPT=/mnt/HaikuWork/rpi4/haiku/tools/airos/build-arm64-app-packages.sh
[[ $NAME =~ ^b-[A-Za-z0-9_-]+$ ]] || { echo "bad name $NAME"; exit 2; }
mkdir -p $H
# Summit's own link: a GNU hash table, and the libraries most symbols are
# found in right after libWebKit (see the engine's PlatformHaiku.cmake).
sed -e 's|-lscintilla -llexilla -lcurl -lcolumnlistview -o "$BUILDDIR/Summit"|-lscintilla -llexilla -lcurl -lcolumnlistview -Wl,--hash-style=both -o "$BUILDDIR/Summit"|' \
	-e 's|		-lWebKit -lbe -lnetwork -lcrypto -lbnetapi|		-lWebKit -lJavaScriptCore -lbe -lstdc++ -lroot -lnetwork -lcrypto -lbnetapi|' \
	$FORK_SCRIPT > $H/pkg.sh
grep -q 'hash-style=both -o "$BUILDDIR/Summit"' $H/pkg.sh && grep -q 'lWebKit -lJavaScriptCore -lbe' $H/pkg.sh \
	|| { echo "packaging script changed; check the Summit link line"; exit 1; }
rm -rf $H/pkgs-$NAME && mkdir -p $H/pkgs-$NAME
SUMMIT_ENGINE=/mnt/HaikuWork/rpi4/summit-gl/WebKitBuild SUMMIT_ENGINE_LOG=/mnt/HaikuWork/rpi4/summit-gl/ninja-ec-$TAG.log \
SUMMIT_ENGINE_EXTRA_DEPS=${DEPS_PREFIX:-/mnt/HaikuWork/rpi4/summit-gl/deps} TLS_DEPS=${TLS_PREFIX:-/mnt/HaikuWork/build/summit-arm64/deps} \
SUMMIT_WEBKIT_VERSION=${SUMMIT_WEBKIT_VERSION:-1.10.0-3} SUMMIT_REVISION=${SUMMIT_REVISION:-1} \
APPBUILD=$H/appbuild JOBS=12 bash $H/pkg.sh $H/pkgs-$NAME summit_webkit summit > $H/pkg-$NAME.log 2>&1
[[ -n ${OUT_PACKAGES:-} ]] && cp $H/pkgs-$NAME/*.hpkg "$OUT_PACKAGES"/
T=/mnt/HaikuWork/build/arm64/objects/linux/x86_64/release/tools/package/package
export LD_LIBRARY_PATH=/mnt/HaikuWork/build/arm64/objects/linux/lib
rm -rf $H/x-$NAME && mkdir -p $H/x-$NAME
for p in $H/pkgs-$NAME/*.hpkg; do $T extract -C $H/x-$NAME "$p"; done
D=/mnt/Documents/SummitPi/$NAME
rm -rf "$D" && mkdir -p "$D"
cp -a $H/x-$NAME/lib/summit-webkit/. "$D"/ 2>/dev/null || true   # the share refuses the libWebKit.so link
cp $H/x-$NAME/apps/Summit "$D"/Summit && chmod 755 "$D"/Summit
/mnt/HaikuWork/toolchains/patchelf/bin/patchelf --force-rpath --set-rpath '$ORIGIN/lib' "$D"/Summit
ls $H/pkgs-$NAME; du -sh "$D"

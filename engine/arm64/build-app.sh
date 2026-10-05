#!/usr/bin/env bash
# Cross-compile the Summit browser app for arm64 Haiku against the cross-built
# engine, mirroring tools/build-modern-browser.py (browser target, extensions).
set -euo pipefail
W=/mnt/HaikuWork/build/summit-arm64
. $W/hosttools/env.sh
ROOT=$W/summit
WK=$W/WebKit/Source/WebKit
ENGINE=$W/WebKitBuild
SYSROOT=$W/sysroot
H=$SYSROOT/boot/system/develop/headers
OUT=$W/app
X=/mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-
rm -rf $OUT; mkdir -p $OUT/include/WebKit $OUT/obj

cp $WK/UIProcess/API/haiku/WebKitView.h $WK/UIProcess/API/haiku/WebKitContext.h \
   $WK/UIProcess/API/haiku/WebKitEmbedding.h $WK/UIProcess/API/haiku/WebKitExtensionPermission.h $WK/UIProcess/API/haiku/WebKitInfo.h \
   $WK/Shared/API/c/WKBase.h $WK/Shared/API/c/WKDeclarationSpecifiers.h \
   $WK/Shared/API/c/haiku/WKBaseHaiku.h $OUT/include/WebKit/

FLAGS=(--sysroot=$SYSROOT -std=c++23 -O2 -Wall -Wextra -Wno-multichar -DBUILDING_HAIKU__=1
       -I$OUT/include -DSUMMIT_MODERN_WEBKIT=1 -I$ROOT/src -I$ROOT/vendor
       -I$H/private/netservices -I$H/private -I$W/deps/include)
SOURCES=(src/main.cpp src/core/Address.cpp src/core/Profile.cpp src/core/ExtensionCatalog.cpp
         src/core/ExtensionIdentity.cpp src/core/Favicon.cpp src/core/InternalPages.cpp
         src/ui/FaviconCache.cpp src/ui/PreferencesWindow.cpp src/ui/SharedProfile.cpp
         src/ui/BrowserWindow.cpp src/ui/Chrome.cpp src/ui/ExtensionPermissionPrompt.cpp
         src/ui/ExtensionController.cpp src/ui/ExtensionInstaller.cpp src/ui/ExtensionManager.cpp)
OBJS=()
for s in "${SOURCES[@]}"; do
    o=$OUT/obj/$(echo $s | tr / _ | sed 's/\.cpp$/.o/')
    ${X}g++ "${FLAGS[@]}" -c $ROOT/$s -o $o &
    OBJS+=($o)
done
wait
for o in "${OBJS[@]}"; do [ -f $o ] || { echo "missing $o"; exit 1; }; done
if [ "${1:-}" = --compile-only ]; then echo compiled; exit 0; fi

# -lgcc_s: the cross libgcc.a carries a private unwinder; without the shared one every
# C++ exception calls std::terminate (a damaged profile.json aborted Summit at start).
${X}g++ --sysroot=$SYSROOT "${OBJS[@]}" -L$ENGINE/lib -L$W/deps/lib \
    -Wl,-rpath-link,$ENGINE/lib -Wl,-rpath-link,$W/deps/lib -Wl,-rpath-link,$SYSROOT/boot/system/lib \
    -Wl,-rpath,'$ORIGIN/lib' \
    -lWebKit -lbe -lnetwork -lcrypto -lbnetapi -ltranslation -ltracker -lgame -lgcc_s -o $OUT/Summit
rc -I $ROOT/resources -o $OUT/Summit.rsrc $ROOT/resources/Summit.rdef
xres -o $OUT/Summit $OUT/Summit.rsrc
echo linked $OUT/Summit

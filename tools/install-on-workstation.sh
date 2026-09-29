#!/usr/bin/env bash
# Install a built Summit bundle on the X399 workstation: the desktop launcher
# /boot/home/Desktop/Summit-current.sh points at it (the previous launcher is
# kept in /boot/home/summit/launcher-backups/, not on the Desktop). Programs that embed the engine
# (Aurora and its apps) use the summit_webkit package instead
# (tools/package-summit-webkit.sh).
#
#   bash tools/install-on-workstation.sh bundle-XXXXXXXX
#
# SUMMIT_MESA_PREFIX names the private Mesa the launcher runs with, a
# directory of /boot/home/summit-mesa (default: prefix). A new Mesa build is
# installed beside the old one, so that a browser that is running keeps the
# libraries it started with.
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bundle=${1:?usage: install-on-workstation.sh bundle-NAME}
if [[ ! $bundle =~ ^bundle-[A-Za-z0-9_]+$ ]]; then
    echo 'The bundle must be named bundle-XXXXXXXX.' >&2
    exit 2
fi
mesa=${SUMMIT_MESA_PREFIX:-prefix}
if [[ ! $mesa =~ ^prefix[A-Za-z0-9_.-]*$ ]]; then
    echo 'SUMMIT_MESA_PREFIX must name a prefix* directory of /boot/home/summit-mesa.' >&2
    exit 2
fi
stamp=$(date +%Y%m%d-%H%M)
bash "$SUMMIT_ROOT/tools/ws.sh" "set -e
B=/boot/home/summit/build-modern-browser/$bundle
test -x \$B/Summit && test -x \$B/WebProcess && test -f \$B/lib/libWebKit.so.1
M=/boot/home/summit-mesa/$mesa
test -f \$M/lib/libEGL_mesa.so.0 && grep -q \$M/lib/ \$M/data/glvnd/egl_vendor.d/50_mesa.json
L=/boot/home/Desktop/Summit-current.sh
mkdir -p /boot/home/summit/launcher-backups
if test -f \$L; then cp \$L /boot/home/summit/launcher-backups/Summit-current.pre-$stamp.sh; fi
cat > \$L <<EOF
#!/bin/sh
set -eu
SUMMIT_BUNDLE=\$B
export WEBKIT_EXEC_PATH=\"\\\$SUMMIT_BUNDLE\"
export SUMMIT_SKIA_GL_CONTEXT=\\\${SUMMIT_SKIA_GL_CONTEXT:-1}
export SUMMIT_SCROLL_REFRESH_TIMER=\\\${SUMMIT_SCROLL_REFRESH_TIMER:-16}
export LIBRARY_PATH=\"\$M/lib:\\\$SUMMIT_BUNDLE/lib:/boot/system/lib\"
export __EGL_VENDOR_LIBRARY_FILENAMES=\$M/data/glvnd/egl_vendor.d/50_mesa.json
# One line a second while the pointer moves: how late pointer events arrive
# and gaps in the mouse's own reports (docs/performance.md, 29 September).
export SUMMIT_INPUT_LAG_TRACE=\\\${SUMMIT_INPUT_LAG_TRACE:-1}
# Message dispatches of the window and application threads over 50 ms.
export SUMMIT_UI_STALL_TRACE=\\\${SUMMIT_UI_STALL_TRACE:-1}
exec \"\\\$SUMMIT_BUNDLE/Summit\" \"\\\$@\" 2>>/boot/home/summit/summit-stderr.log
EOF
chmod +x \$L
# libnetwork's nsswitch_conf_file_path() is not thread-safe; a real config file
# newer than the settings directory keeps a racy re-parse from opening the
# directory and exiting the network process (docs/kunanyios-platform-issues.md).
N=/boot/system/settings/network/nsswitch.conf
if ! test -f \$N; then
    printf '# Created by Summit (tools/install-on-workstation.sh): the same sources libnetwork\n# uses without a file. A file newer than /boot/system/settings stops a racy\n# re-parse in nsswitch_conf_file_path() from opening the directory instead;\n# see Summit docs/kunanyios-platform-issues.md.\nhosts: files dns\n' > \$N
    echo \"created \$N\"
fi
echo \"launcher -> \$B\"
cat \$L"

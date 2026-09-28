#!/usr/bin/env bash
# Install a built Summit bundle on the X399 workstation: the desktop launcher
# /boot/home/Desktop/Summit-current.sh points at it (the previous launcher is
# kept as Summit-current.pre-<date>.sh). Programs that embed the engine
# (Aurora and its apps) use the summit_webkit package instead
# (tools/package-summit-webkit.sh).
#
#   bash tools/install-on-workstation.sh bundle-XXXXXXXX
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bundle=${1:?usage: install-on-workstation.sh bundle-NAME}
if [[ ! $bundle =~ ^bundle-[A-Za-z0-9_]+$ ]]; then
    echo 'The bundle must be named bundle-XXXXXXXX.' >&2
    exit 2
fi
stamp=$(date +%Y%m%d-%H%M)
bash "$SUMMIT_ROOT/tools/ws.sh" "set -e
B=/boot/home/summit/build-modern-browser/$bundle
test -x \$B/Summit && test -x \$B/WebProcess && test -f \$B/lib/libWebKit.so.1
L=/boot/home/Desktop/Summit-current.sh
if test -f \$L; then cp \$L /boot/home/Desktop/Summit-current.pre-$stamp.sh; fi
cat > \$L <<EOF
#!/bin/sh
set -eu
SUMMIT_BUNDLE=\$B
export WEBKIT_EXEC_PATH=\"\\\$SUMMIT_BUNDLE\"
export SUMMIT_SKIA_GL_CONTEXT=\\\${SUMMIT_SKIA_GL_CONTEXT:-1}
export SUMMIT_SCROLL_REFRESH_TIMER=\\\${SUMMIT_SCROLL_REFRESH_TIMER:-16}
export LIBRARY_PATH=\"/boot/home/summit-mesa/prefix/lib:\\\$SUMMIT_BUNDLE/lib:/boot/system/lib\"
exec \"\\\$SUMMIT_BUNDLE/Summit\" \"\\\$@\"
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

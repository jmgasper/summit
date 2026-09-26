#!/usr/bin/env bash
# Install a built Summit bundle on the X399 workstation:
#   - the desktop launcher /boot/home/Desktop/Summit-current.sh points at it
#     (the previous launcher is kept as Summit-current.pre-<date>.sh);
#   - the shared engine directory /boot/home/config/non-packaged/lib/summit-webkit
#     gets its libWebKit, JavaScriptCore, ICU, WebProcess and NetworkProcess, for
#     other programs that embed the engine (Aurora). The previous contents are
#     kept as summit-webkit.pre-<date>.
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
E=/boot/home/config/non-packaged/lib/summit-webkit
if test -d \$E; then mv \$E \$E.pre-$stamp; fi
mkdir -p \$E
cp -a \$B/lib/. \$E/
cp -a \$B/WebProcess \$B/NetworkProcess \$E/
cp -a \$B/build-manifest.json \$E/ 2>/dev/null || true
echo \"launcher -> \$B\"
echo \"engine   -> \$E\"
cat \$L"

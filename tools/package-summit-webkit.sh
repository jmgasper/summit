#!/usr/bin/env bash
# Build the summit_webkit package (Summit's engine for other programs, such as
# Aurora) from a bundle in the build VM, and copy it to artifacts/.
#
#   bash tools/package-summit-webkit.sh bundle-XXXXXXXX
#
# The Mesa is the VM's llvmpipe build (/SummitExtensions/mesa/prefix): it needs
# no GPU driver, so the package works on any machine.
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bundle=${1:?usage: package-summit-webkit.sh bundle-NAME}
if [[ ! $bundle =~ ^bundle-[A-Za-z0-9_]+$ ]]; then
    echo 'The bundle must be named bundle-XXXXXXXX.' >&2
    exit 2
fi
version=${SUMMIT_WEBKIT_VERSION:-1.10.0}
guest_out=/SummitExtensions/summit/tmp/summit-webkit-package
shell="$SUMMIT_ROOT/tools/haiku.sh"
bash "$shell" "mkdir -p $guest_out && cat > $guest_out/make-package.sh" < "$SUMMIT_ROOT/tools/summit-webkit-package/make-package.sh"
bash "$shell" "sh $guest_out/make-package.sh /SummitExtensions/summit/build-modern-browser/$bundle \
    /SummitExtensions/mesa/prefix /boot/home/summit-webkit-extensions $guest_out $version"
mkdir -p "$SUMMIT_ROOT/artifacts"
name=summit_webkit-$version-1-x86_64.hpkg
bash "$shell" "cat $guest_out/$name" > "$SUMMIT_ROOT/artifacts/$name"
ls -la "$SUMMIT_ROOT/artifacts/$name"

#!/usr/bin/env bash
# Build the summit_webkit package (Summit's engine for other programs, such as
# Aurora) from a bundle in the build VM, and copy it to artifacts/.
#
#   bash tools/package-summit-webkit.sh bundle-XXXXXXXX
#
# The Mesa is the VM's llvmpipe build (/SummitExtensions/mesa/prefix): it needs
# no GPU driver, so the package works on any machine.
#
# SUMMIT_REMOTE_SHELL=tools/ws.sh builds it on the X399 workstation from one of
# its bundles instead, with its zink Mesa (SUMMIT_MESA_PREFIX, a prefix*
# directory of /boot/home/summit-mesa; default prefix): a package for that
# machine's GPU.
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bundle=${1:?usage: package-summit-webkit.sh bundle-NAME}
if [[ ! $bundle =~ ^bundle-[A-Za-z0-9_]+$ ]]; then
    echo 'The bundle must be named bundle-XXXXXXXX.' >&2
    exit 2
fi
version=${SUMMIT_WEBKIT_VERSION:-1.10.0}
revision=${SUMMIT_WEBKIT_REVISION:-1}
remote=${SUMMIT_REMOTE_SHELL:-tools/haiku.sh}
if [[ $remote == tools/ws.sh ]]; then
    mesa=${SUMMIT_MESA_PREFIX:-prefix}
    if [[ ! $mesa =~ ^prefix[A-Za-z0-9_.-]*$ ]]; then
        echo 'SUMMIT_MESA_PREFIX must name a prefix* directory of /boot/home/summit-mesa.' >&2
        exit 2
    fi
    guest_out=/boot/home/summit/tmp/summit-webkit-package
    bundles=/boot/home/summit/build-modern-browser
    mesa=/boot/home/summit-mesa/$mesa
elif [[ $remote == tools/haiku.sh ]]; then
    guest_out=/SummitExtensions/summit/tmp/summit-webkit-package
    bundles=/SummitExtensions/summit/build-modern-browser
    mesa=/SummitExtensions/mesa/prefix
else
    echo 'SUMMIT_REMOTE_SHELL must be tools/haiku.sh or tools/ws.sh.' >&2
    exit 2
fi
shell="$SUMMIT_ROOT/$remote"
bash "$shell" "mkdir -p $guest_out && cat > $guest_out/make-package.sh" < "$SUMMIT_ROOT/tools/summit-webkit-package/make-package.sh"
bash "$shell" "sh $guest_out/make-package.sh $bundles/$bundle \
    $mesa /boot/home/summit-webkit-extensions $guest_out $version $revision"
mkdir -p "$SUMMIT_ROOT/artifacts"
name=summit_webkit-$version-$revision-x86_64.hpkg
bash "$shell" "cat $guest_out/$name" > "$SUMMIT_ROOT/artifacts/$name"
ls -la "$SUMMIT_ROOT/artifacts/$name"

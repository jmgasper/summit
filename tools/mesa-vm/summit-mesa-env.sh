# Source this file (sh/bash) inside the Haiku guest to run a program against
# the private Mesa/libglvnd stack instead of the system Mesa 22.0.5 packages.
#
#   . /SummitExtensions/mesa/summit-mesa-env.sh
#   ./your-egl-program
#
# Haiku's runtime_loader uses LIBRARY_PATH as the complete runtime search path:
# once it is set, the built-in defaults are no longer consulted, so they are
# repeated here after the private directory.
SUMMIT_MESA_PREFIX=${SUMMIT_MESA_PREFIX:-/SummitExtensions/mesa/prefix}
_summit_default_libs='%A/lib:/boot/home/config/non-packaged/lib:/boot/home/config/lib:/boot/system/non-packaged/lib:/boot/system/lib'
case ":${LIBRARY_PATH:-}:" in
    *":$SUMMIT_MESA_PREFIX/lib:"*) ;;
    *) LIBRARY_PATH="$SUMMIT_MESA_PREFIX/lib:${LIBRARY_PATH:-$_summit_default_libs}" ;;
esac
export LIBRARY_PATH SUMMIT_MESA_PREFIX
unset _summit_default_libs
# libglvnd's libEGL is built to look in $SUMMIT_MESA_PREFIX/data/glvnd/egl_vendor.d
# first, so this is redundant for the pinned prefix. It is exported so the
# selection stays explicit and survives relocation of the prefix.
export __EGL_VENDOR_LIBRARY_FILENAMES="$SUMMIT_MESA_PREFIX/data/glvnd/egl_vendor.d/50_mesa.json"
# zink turns GLES into Vulkan. The Vulkan loader only finds a driver whose ICD
# manifest it can see, and the workstation's NVK build is installed privately,
# so point SUMMIT_VULKAN_ICD at its manifest before sourcing this file.
if [ -n "${SUMMIT_VULKAN_ICD:-}" ]; then
    export VK_ICD_FILENAMES="$SUMMIT_VULKAN_ICD"
fi
# Optional knobs (unset by default):
#   GALLIUM_DRIVER=softpipe|llvmpipe   choose the software rasterizer
#   LP_NUM_THREADS=N                   llvmpipe rasterizer threads
#   EGL_LOG_LEVEL=debug                Mesa EGL diagnostics
#   __EGL_VENDOR_LIBRARY_FILENAMES     must stay pointed at 50_mesa.json

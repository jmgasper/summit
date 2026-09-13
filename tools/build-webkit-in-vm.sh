#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
python3 tools/prepare-webkit.py
# Source paths are fixed and passed as archive members, never as remote commands.
tar -C .cache/WebKit -czf - --exclude=__pycache__ CMakeLists.txt Source Tools |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit-webkit && tar xzf - -C /boot/home/summit-webkit'
bash tools/haiku.sh 'cd /boot/home/summit-webkit && cmake -S . -B WebKitBuild/Release -G Ninja -DPORT=Haiku -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-ftrack-macro-expansion=0 --param ggc-min-expand=10" -DENABLE_WEBKIT=OFF -DENABLE_WEBKIT_LEGACY=ON -DENABLE_LAYOUT_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/boot/home/summit-webkit-install && DISABLE_ASLR=1 ninja -C WebKitBuild/Release -j6'

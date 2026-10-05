#!/usr/bin/env bash
# reorder-needed.sh ELF LIB... : moves the given DT_NEEDED entries (adding any
# that are missing) to the front of ELF's list, in the order given. Haiku's
# runtime loader searches images breadth-first in load order, and the
# executable's list comes first: the libraries most symbols are found in
# (or that look up most of their own symbols) are searched first.
set -euo pipefail
P=${PATCHELF:-/mnt/HaikuWork/toolchains/patchelf/bin/patchelf}
elf=$1; shift
libs=("$@")
for l in "${libs[@]}"; do $P --remove-needed "$l" "$elf" 2>/dev/null || true; done
for (( i=${#libs[@]}-1; i>=0; i-- )); do $P --add-needed "${libs[$i]}" "$elf"; done

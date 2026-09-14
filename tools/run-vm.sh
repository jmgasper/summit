#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
if [[ ! -f .vm/work.qcow2 ]]; then
    echo 'Missing .vm/work.qcow2. See docs/VM.md.' >&2
    exit 1
fi
if [[ -S .vm/qmp.sock ]]; then
    # A timeout is not evidence of termination. Refuse to start a second VM.
    python3 tools/vm.py status
    exit 0
fi
if [[ -f .vm/qemu.pid ]] && kill -0 "$(cat .vm/qemu.pid)" 2>/dev/null; then
    echo 'VM process exists but QMP is missing; inspect the existing process.' >&2
    exit 1
fi
SUMMIT_EXTENSION_DISK=()
if [[ -f .vm/extensions.qcow2 ]]; then
    SUMMIT_EXTENSION_DISK=(
        -drive file=.vm/extensions.qcow2,format=qcow2,if=none,id=summit-extensions
        -device usb-storage,drive=summit-extensions,id=summit-extension-disk,serial=SUMMITEXTENSIONS01
    )
fi
exec qemu-system-x86_64 -enable-kvm -cpu host -m 20480 -smp 12 \
    -drive file=.vm/work.qcow2,format=qcow2,if=ide,index=0 \
    -nic user,model=e1000,hostfwd=tcp:127.0.0.1:2225-:22 \
    -device qemu-xhci -device usb-tablet -vga std -display none \
    "${SUMMIT_EXTENSION_DISK[@]}" \
    -vnc 127.0.0.1:5 -qmp unix:.vm/qmp.sock,server=on,wait=off \
    -pidfile .vm/qemu.pid -serial file:.vm/serial.log -daemonize

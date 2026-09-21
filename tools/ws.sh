#!/usr/bin/env bash
# Run a command on the Haiku workstation (Threadripper 1950X, GeForce GTX 1070).
# Mirrors tools/haiku.sh, which does the same for the QEMU VM.
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SUMMIT_WS_HOST=${SUMMIT_WS_HOST:-192.168.1.237}
SUMMIT_WS_USER=${SUMMIT_WS_USER:-user}
# Keep native compiler temporaries on the selected build volume when requested.
if [[ -n "${SUMMIT_NATIVE_TMPDIR:-}" && $# -gt 0 ]]; then
    printf -v SUMMIT_QUOTED_TMPDIR '%q' "$SUMMIT_NATIVE_TMPDIR"
    SUMMIT_NATIVE_COMMAND="export TMPDIR=$SUMMIT_QUOTED_TMPDIR"$'\n'"$*"
    set -- "$SUMMIT_NATIVE_COMMAND"
fi
SUMMIT_SSH=(ssh -o IdentitiesOnly=yes
    -o UserKnownHostsFile="$SUMMIT_ROOT/.vm/ws_known_hosts" -o StrictHostKeyChecking=accept-new
    -o LogLevel=ERROR -o ConnectTimeout=15 -o ServerAliveInterval=30 -o ServerAliveCountMax=8)
if [[ -f $SUMMIT_ROOT/.vm/ws_id_ed25519 ]]; then
    SUMMIT_SSH+=(-i "$SUMMIT_ROOT/.vm/ws_id_ed25519" -o BatchMode=yes)
    exec "${SUMMIT_SSH[@]}" "$SUMMIT_WS_USER@$SUMMIT_WS_HOST" "$@"
fi
# Password fallback for a machine that has not been given the key yet.
exec sshpass -f "$SUMMIT_ROOT/.vm/ws.pass" "${SUMMIT_SSH[@]}" \
    "$SUMMIT_WS_USER@$SUMMIT_WS_HOST" "$@"

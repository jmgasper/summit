#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# Keep native compiler temporaries on the selected build volume when requested.
if [[ -n "${SUMMIT_NATIVE_TMPDIR:-}" && $# -gt 0 ]]; then
    printf -v SUMMIT_QUOTED_TMPDIR '%q' "$SUMMIT_NATIVE_TMPDIR"
    SUMMIT_NATIVE_COMMAND="export TMPDIR=$SUMMIT_QUOTED_TMPDIR"$'\n'"$*"
    set -- "$SUMMIT_NATIVE_COMMAND"
fi
exec ssh -i "$SUMMIT_ROOT/.vm/id_ed25519" -p 2225 \
    -o IdentitiesOnly=yes -o BatchMode=yes \
    -o UserKnownHostsFile="$SUMMIT_ROOT/.vm/known_hosts" \
    -o ConnectTimeout=10 user@127.0.0.1 "$@"

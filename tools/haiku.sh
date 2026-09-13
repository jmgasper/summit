#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec ssh -i "$SUMMIT_ROOT/.vm/id_ed25519" -p 2225 \
    -o IdentitiesOnly=yes -o BatchMode=yes \
    -o UserKnownHostsFile="$SUMMIT_ROOT/.vm/known_hosts" \
    -o ConnectTimeout=10 user@127.0.0.1 "$@"

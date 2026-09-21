#!/usr/bin/env bash
# Host-side driver for the private Mesa build inside Summit's Haiku VM.
# See docs/vm-egl-stack.md.
#
#   tools/mesa-vm/build.sh fetch                 download + verify pinned inputs
#   tools/mesa-vm/build.sh upload                copy inputs/scripts to the guest
#   tools/mesa-vm/build.sh run STAGE...          run guest stages in the foreground
#   tools/mesa-vm/build.sh start STAGE...        run guest stages detached (long builds)
#   tools/mesa-vm/build.sh status                is a detached run alive? tail its log
#   tools/mesa-vm/build.sh wait [SECONDS]        poll until the detached run ends
#
# Environment forwarded to the guest: SUMMIT_MESA_ROOT, SUMMIT_MESA_JOBS,
# SUMMIT_MESA_DRIVERS (softpipe,llvmpipe [default] | softpipe | zink).
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SUMMIT_ROOT=$(cd -- "$HERE/../.." && pwd)
# The build host is selected with SUMMIT_REMOTE_SHELL: tools/haiku.sh (the QEMU
# VM, the default) or tools/ws.sh (the workstation, which has a real GPU).
SUMMIT_REMOTE_SHELL=${SUMMIT_REMOTE_SHELL:-tools/haiku.sh}
if [[ ! $SUMMIT_REMOTE_SHELL =~ ^tools/(haiku|ws)\.sh$ ]]; then
    echo 'SUMMIT_REMOTE_SHELL must be tools/haiku.sh or tools/ws.sh.' >&2
    exit 2
fi
HAIKU="$SUMMIT_ROOT/$SUMMIT_REMOTE_SHELL"
CACHE=${SUMMIT_MESA_CACHE:-$SUMMIT_ROOT/.vm/mesa-inputs}
GUEST_ROOT=${SUMMIT_MESA_ROOT:-/SummitExtensions/mesa}
JOBS=${SUMMIT_MESA_JOBS:-4}
DRIVERS=${SUMMIT_MESA_DRIVERS:-softpipe,llvmpipe}

# name|sha256|url  (local copies under /mnt/HaikuWork are used when present)
INPUTS=(
"mesa-25.3.6.tar.xz|59217efeac3b64e7ced958324b9db7494f1e0741aeb22d780276514cc1b8f206|https://archive.mesa3d.org/mesa-25.3.6.tar.xz"
"libglvnd-v1.7.0.tar.gz|2b6e15b06aafb4c0b6e2348124808cbd9b291c647299eaaba2e3202f51ff2f3d|https://gitlab.freedesktop.org/glvnd/libglvnd/-/archive/v1.7.0/libglvnd-v1.7.0.tar.gz"
"meson-1.8.4-py3-none-any.whl|2d71499af23ea06abe1bb7ea16843973f53b47b1ce89810c32ec154e6c1dc1db|pip:meson==1.8.4"
"mako-1.3.10-py3-none-any.whl|baef24a52fc4fc514a0887ac600f9f1cff3d82c61d4d700a1fa84d597b88db59|pip:Mako==1.3.10"
"packaging-25.0-py3-none-any.whl|29572ef2b1f17581046b3a2227d5c611fb25ec70ca1ba8554b24b0e69331a484|pip:packaging==25.0"
"MarkupSafe-2.1.5.tar.gz|d283d37a890ba4c1ae73ffadf8046435c76e7bc2247bbb63c00bd1a709c6544b|pip-sdist:MarkupSafe==2.1.5"
)
LOCAL_COPIES=(
    /mnt/HaikuWork/cache/mesa-25.3.6.tar.xz
    /mnt/HaikuWork/artifacts/mali-haiku-mesa/20260915T100752Z-80955e/references/libglvnd-v1.7.0.tar.gz
)
REPO_FILES=(
    guest-build.sh summit-mesa-env.sh eglprobe.cpp gles2-draw-probe.cpp
    mesa-25.3.6-haiku-x86_64.patch
    libglvnd-1.7.0.patchset libglvnd-haiku-mutex.patch
    libglvnd-haiku-bitmap.patch libglvnd-haiku-private-prefix.patch
)

sha() { sha256sum "$1" | awk '{print $1}'; }

fetch() {
    mkdir -p "$CACHE"
    local entry name hash url copy
    for entry in "${INPUTS[@]}"; do
        IFS='|' read -r name hash url <<<"$entry"
        if [[ -f "$CACHE/$name" && $(sha "$CACHE/$name") == "$hash" ]]; then
            continue
        fi
        for copy in "${LOCAL_COPIES[@]}"; do
            if [[ $(basename "$copy") == "$name" && -f "$copy" ]]; then
                cp "$copy" "$CACHE/$name"
            fi
        done
        if [[ ! -f "$CACHE/$name" ]]; then
            case "$url" in
                pip:*) python3 -m pip download --no-deps --only-binary=:all: \
                    --python-version 3.10 --implementation py --abi none \
                    --platform any -d "$CACHE" "${url#pip:}" ;;
                pip-sdist:*) python3 -m pip download --no-deps --no-binary=:all: \
                    -d "$CACHE" "${url#pip-sdist:}" ;;
                *) curl -fL -o "$CACHE/$name" "$url" ;;
            esac
        fi
        if [[ $(sha "$CACHE/$name") != "$hash" ]]; then
            echo "sha256 mismatch: $CACHE/$name" >&2
            exit 2
        fi
    done
    echo "inputs verified in $CACHE"
}

upload() {
    fetch
    bash "$HAIKU" "mkdir -p '$GUEST_ROOT/inputs' '$GUEST_ROOT/tmp' '$GUEST_ROOT/logs'"
    local entry name hash url
    local names=()
    for entry in "${INPUTS[@]}"; do
        IFS='|' read -r name hash url <<<"$entry"
        names+=("$name")
    done
    tar -C "$CACHE" -cf - "${names[@]}" | bash "$HAIKU" "tar -xf - -C '$GUEST_ROOT/inputs'"
    # Optional extra Mesa patches (mesa-25.3.6-summit-NN-*.patch) are applied
    # in name order after the KunanyiOS subset.
    local extra=()
    while IFS= read -r -d '' name; do extra+=("$(basename "$name")"); done \
        < <(find "$HERE" -maxdepth 1 -name 'mesa-25.3.6-summit-*.patch' -print0 | sort -z)
    bash "$HAIKU" "rm -f '$GUEST_ROOT'/inputs/mesa-25.3.6-summit-*.patch"
    tar -C "$HERE" -cf - "${REPO_FILES[@]}" "${extra[@]}" \
        | bash "$HAIKU" "tar -xf - -C '$GUEST_ROOT/inputs' && chmod +x '$GUEST_ROOT/inputs/guest-build.sh'"
    bash "$HAIKU" "cd '$GUEST_ROOT/inputs' && sha256sum *" | sort -k2
}

guest_env="SUMMIT_MESA_ROOT='$GUEST_ROOT' SUMMIT_MESA_JOBS='$JOBS' SUMMIT_MESA_DRIVERS='$DRIVERS'"

case "${1:-}" in
    fetch) fetch ;;
    upload) upload ;;
    run)
        shift
        bash "$HAIKU" "$guest_env bash '$GUEST_ROOT/inputs/guest-build.sh' $*" ;;
    start)
        shift
        bash "$HAIKU" "cd '$GUEST_ROOT' && if [ -f logs/current.pid ] && kill -0 \$(cat logs/current.pid) 2>/dev/null; then echo 'a detached run is still active' >&2; exit 1; fi; log=logs/\$(date +%Y%m%dT%H%M%S)-$(echo "$*" | tr ' ' '+').log; ln -sf \"\$(basename \$log)\" logs/current.log; $guest_env nohup bash inputs/guest-build.sh $* > \$log 2>&1 < /dev/null & echo \$! > logs/current.pid; echo started pid \$(cat logs/current.pid) log $GUEST_ROOT/\$log" ;;
    status)
        bash "$HAIKU" "cd '$GUEST_ROOT/logs' && if kill -0 \$(cat current.pid) 2>/dev/null; then echo RUNNING; else echo FINISHED; fi; tail -n ${2:-15} current.log" ;;
    wait)
        limit=${2:-540}
        start=$(date +%s)
        while (( $(date +%s) - start < limit )); do
            state=$(bash "$HAIKU" "cd '$GUEST_ROOT/logs' && if kill -0 \$(cat current.pid) 2>/dev/null; then echo RUNNING; else echo FINISHED; fi" || echo SSH-ERROR)
            [[ $state == FINISHED ]] && break
            sleep 20
        done
        bash "$HAIKU" "cd '$GUEST_ROOT/logs' && if kill -0 \$(cat current.pid) 2>/dev/null; then echo RUNNING; else echo FINISHED; fi; tail -n 12 current.log" ;;
    *)
        sed -n '2,14p' "$0"; exit 1 ;;
esac

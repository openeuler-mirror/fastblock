#!/bin/bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
run_smoke="${KFASTBLOCK_CI_RUN_SMOKE:-0}"
hostbuild_kdir="${KFASTBLOCK_HOSTBUILD_KDIR:-/root/kernel/.hostbuild}"
uname_r="$(uname -r)"

resolve_kdir() {
    if [ -f "$hostbuild_kdir/include/config/auto.conf" ] && [ -f "$hostbuild_kdir/include/generated/utsrelease.h" ]; then
        printf '%s\n' "$hostbuild_kdir"
        return 0
    fi

    printf '/lib/modules/%s/build\n' "$uname_r"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --with-smoke)
            run_smoke=1
            ;;
        --without-smoke)
            run_smoke=0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 1
            ;;
    esac
    shift
done

echo "[kfastblock-ci] build monitor binaries"
"$repo_root/build.sh" -c monitor

echo "[kfastblock-ci] verify monitor module tree"
(cd "$repo_root/monitor" && go build ./...)

kdir="$(resolve_kdir)"
if [ -d "$kdir" ]; then
    echo "[kfastblock-ci] build kfastblock module and tools with KDIR=$kdir"
    make -C "$repo_root/kfastblock" KDIR="$kdir" clean all
else
    echo "[kfastblock-ci] skip kfastblock kernel module build: missing KDIR=$kdir"
    echo "[kfastblock-ci] build kfastblock userspace tools only"
    make -C "$repo_root/kfastblock" kfastblock-admin kfastblock-rawctl
fi

if [ "$run_smoke" = "1" ]; then
    if [ "$(id -u)" -ne 0 ]; then
        echo "[kfastblock-ci] smoke requires root" >&2
        exit 1
    fi
    echo "[kfastblock-ci] run kfastblock smoke"
    "$repo_root/scripts/run-kfastblock-dev-smoke.sh"
else
    echo "[kfastblock-ci] smoke skipped"
fi

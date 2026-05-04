#!/bin/bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
run_smoke="${KFASTBLOCK_CI_RUN_SMOKE:-0}"

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

echo "[kfastblock-ci] build kfastblock module and tools"
make -C "$repo_root/kfastblock" clean all

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

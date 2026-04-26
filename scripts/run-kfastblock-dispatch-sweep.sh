#!/bin/bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact_root="${KFASTBLOCK_TEST_ARTIFACT_ROOT:-$repo_root/.artifacts/kfastblock-tests}"
run_dir="$artifact_root/dispatch-sweep-$(date +%Y%m%d-%H%M%S)"
log_file="$run_dir/dispatch-sweep.log"
windows="${KFASTBLOCK_SWEEP_WINDOWS:-1,4,8,16}"
duration_sec="${KFASTBLOCK_SWEEP_DURATION_SEC:-6}"
io_workers="${KFASTBLOCK_SWEEP_IO_WORKERS:-1}"
open_workers="${KFASTBLOCK_SWEEP_OPEN_WORKERS:-1}"
attach_cycles="${KFASTBLOCK_SWEEP_ATTACH_CYCLES:-1}"

mkdir -p "$run_dir"
exec > >(tee -a "$log_file") 2>&1

printf 'dispatch_window\tartifact_dir\tattach_detach\topen\tio\n' > "$run_dir/summary.tsv"
echo "artifact_dir=$run_dir"
echo "windows=$windows duration_sec=$duration_sec io_workers=$io_workers open_workers=$open_workers attach_cycles=$attach_cycles"

IFS=',' read -r -a window_values <<< "$windows"
for window in "${window_values[@]}"; do
    echo "[sweep] start dispatch_window=$window"
    env \
        KFASTBLOCK_TEST_ARTIFACT_ROOT="$run_dir" \
        KFASTBLOCK_TEST_REUSE_CLUSTER=1 \
        KFASTBLOCK_CONCURRENCY_DISPATCH_WINDOW="$window" \
        KFASTBLOCK_CONCURRENCY_DURATION_SEC="$duration_sec" \
        KFASTBLOCK_CONCURRENCY_IO_WORKERS="$io_workers" \
        KFASTBLOCK_CONCURRENCY_OPEN_WORKERS="$open_workers" \
        KFASTBLOCK_CONCURRENCY_ATTACH_CYCLES="$attach_cycles" \
        "$repo_root/scripts/run-kfastblock-concurrency.sh"

    latest_dir="$(ls -1dt "$run_dir"/concurrency-* | head -n 1)"
    attach_summary="$(tr '\n' ';' < "$latest_dir/attach-detach.summary")"
    open_summary=""
    io_summary=""
    if ls "$latest_dir"/open-*.summary >/dev/null 2>&1; then
        open_summary="$(cat "$latest_dir"/open-*.summary | tr '\n' ';')"
    fi
    if ls "$latest_dir"/io-*.summary >/dev/null 2>&1; then
        io_summary="$(cat "$latest_dir"/io-*.summary | tr '\n' ';')"
    fi
    printf '%s\t%s\t%s\t%s\t%s\n' \
        "$window" "$latest_dir" "$attach_summary" "$open_summary" "$io_summary" \
        >> "$run_dir/summary.tsv"
    echo "[sweep] ok dispatch_window=$window artifact=$latest_dir"
done

echo "[sweep] complete"

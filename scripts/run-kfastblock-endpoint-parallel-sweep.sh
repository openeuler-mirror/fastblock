#!/bin/bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact_root="${KFASTBLOCK_TEST_ARTIFACT_ROOT:-$repo_root/.artifacts/kfastblock-tests}"
run_dir="$artifact_root/endpoint-parallel-sweep-$(date +%Y%m%d-%H%M%S)"
log_file="$run_dir/endpoint-parallel-sweep.log"
limits="${KFASTBLOCK_ENDPOINT_PARALLEL_LIMITS:-1,2,4}"
duration_sec="${KFASTBLOCK_ENDPOINT_SWEEP_DURATION_SEC:-20}"
io_workers="${KFASTBLOCK_ENDPOINT_SWEEP_IO_WORKERS:-4}"
open_workers="${KFASTBLOCK_ENDPOINT_SWEEP_OPEN_WORKERS:-2}"
attach_cycles="${KFASTBLOCK_ENDPOINT_SWEEP_ATTACH_CYCLES:-0}"
dispatch_window="${KFASTBLOCK_ENDPOINT_SWEEP_DISPATCH_WINDOW:-}"
reuse_cluster="${KFASTBLOCK_ENDPOINT_SWEEP_REUSE_CLUSTER:-1}"

mkdir -p "$run_dir"
exec > >(tee -a "$log_file") 2>&1

printf 'parallel_limit\tartifact_dir\trefresh\topen\tio\tattach_detach\n' \
    > "$run_dir/summary.tsv"
echo "artifact_dir=$run_dir"
echo "limits=$limits duration_sec=$duration_sec io_workers=$io_workers open_workers=$open_workers attach_cycles=$attach_cycles"
echo "dispatch_window=${dispatch_window:-default}"
echo "reuse_cluster=$reuse_cluster"

IFS=',' read -r -a limit_values <<< "$limits"
for limit in "${limit_values[@]}"; do
    echo "[sweep] start osd_endpoint_parallel_limit=$limit"
    env \
        KFASTBLOCK_TEST_ARTIFACT_ROOT="$run_dir" \
        KFASTBLOCK_TEST_REUSE_CLUSTER="$reuse_cluster" \
        KFASTBLOCK_MODULE_PARAMS="osd_endpoint_parallel_limit=$limit" \
        KFASTBLOCK_FORCE_RELOAD_MODULE=1 \
        KFASTBLOCK_CONCURRENCY_DURATION_SEC="$duration_sec" \
        KFASTBLOCK_CONCURRENCY_IO_WORKERS="$io_workers" \
        KFASTBLOCK_CONCURRENCY_OPEN_WORKERS="$open_workers" \
        KFASTBLOCK_CONCURRENCY_ATTACH_CYCLES="$attach_cycles" \
        KFASTBLOCK_CONCURRENCY_DISPATCH_WINDOW="$dispatch_window" \
        "$repo_root/scripts/run-kfastblock-concurrency.sh"

    latest_dir="$(ls -1dt "$run_dir"/concurrency-* | head -n 1)"
    refresh_summary=""
    open_summary=""
    io_summary=""
    attach_summary=""
    if [ -f "$latest_dir/refresh.summary" ]; then
        refresh_summary="$(tr '\n' ';' < "$latest_dir/refresh.summary")"
    fi
    if ls "$latest_dir"/open-*.summary >/dev/null 2>&1; then
        open_summary="$(cat "$latest_dir"/open-*.summary | tr '\n' ';')"
    fi
    if ls "$latest_dir"/io-*.summary >/dev/null 2>&1; then
        io_summary="$(cat "$latest_dir"/io-*.summary | tr '\n' ';')"
    fi
    if [ -f "$latest_dir/attach-detach.summary" ]; then
        attach_summary="$(tr '\n' ';' < "$latest_dir/attach-detach.summary")"
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$limit" "$latest_dir" "$refresh_summary" "$open_summary" "$io_summary" \
        "$attach_summary" >> "$run_dir/summary.tsv"
    echo "[sweep] ok osd_endpoint_parallel_limit=$limit artifact=$latest_dir"
done

echo "[sweep] complete"

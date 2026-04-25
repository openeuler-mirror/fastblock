#!/bin/bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
matrix_root="${KFASTBLOCK_TEST_ARTIFACT_ROOT:-$repo_root/.artifacts/kfastblock-tests}"
run_dir="$matrix_root/matrix-$(date +%Y%m%d-%H%M%S)"
log_file="$run_dir/matrix.log"
run_sanitizer="${KFASTBLOCK_MATRIX_RUN_SANITIZER:-1}"
refresh_duration="${KFASTBLOCK_MATRIX_REFRESH_DURATION_SEC:-8}"
lifecycle_duration="${KFASTBLOCK_MATRIX_LIFECYCLE_DURATION_SEC:-8}"
lifecycle_cycles="${KFASTBLOCK_MATRIX_LIFECYCLE_ATTACH_CYCLES:-1}"
fault_duration="${KFASTBLOCK_MATRIX_FAULT_DURATION_SEC:-12}"
fault_down_sec="${KFASTBLOCK_MATRIX_FAULT_DOWN_SEC:-2}"

mkdir -p "$run_dir"
exec > >(tee -a "$log_file") 2>&1

while [ "$#" -gt 0 ]; do
    case "$1" in
        --skip-sanitizer)
            run_sanitizer=0
            ;;
        --with-sanitizer)
            run_sanitizer=1
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 1
            ;;
    esac
    shift
done

run_case() {
    local case_name="$1"
    shift

    echo "[matrix] start $case_name"
    if env KFASTBLOCK_TEST_ARTIFACT_ROOT="$run_dir" "$@"; then
        echo "[matrix] ok $case_name"
        printf '%s ok\n' "$case_name" >> "$run_dir/summary.txt"
    else
        echo "[matrix] fail $case_name" >&2
        printf '%s fail\n' "$case_name" >> "$run_dir/summary.txt"
        return 1
    fi
}

echo "artifact_dir=$run_dir"

run_case smoke "$repo_root/scripts/run-kfastblock-dev-smoke.sh"
run_case concurrency-refresh env \
    KFASTBLOCK_CONCURRENCY_DURATION_SEC="$refresh_duration" \
    KFASTBLOCK_CONCURRENCY_ATTACH_CYCLES=0 \
    KFASTBLOCK_CONCURRENCY_IO_WORKERS=2 \
    KFASTBLOCK_CONCURRENCY_OPEN_WORKERS=0 \
    "$repo_root/scripts/run-kfastblock-concurrency.sh"
run_case concurrency-lifecycle env \
    KFASTBLOCK_CONCURRENCY_DURATION_SEC="$lifecycle_duration" \
    KFASTBLOCK_CONCURRENCY_ATTACH_CYCLES="$lifecycle_cycles" \
    KFASTBLOCK_CONCURRENCY_IO_WORKERS=1 \
    KFASTBLOCK_CONCURRENCY_OPEN_WORKERS=1 \
    "$repo_root/scripts/run-kfastblock-concurrency.sh"
run_case fault-injection env \
    KFASTBLOCK_FAULT_IO_DURATION_SEC="$fault_duration" \
    KFASTBLOCK_FAULT_DOWN_SEC="$fault_down_sec" \
    "$repo_root/scripts/run-kfastblock-fault-injection.sh"

if [ "$run_sanitizer" = "1" ]; then
    run_case sanitizer "$repo_root/scripts/run-kfastblock-sanitizer.sh"
else
    echo "[matrix] skip sanitizer"
    printf '%s skipped\n' "sanitizer" >> "$run_dir/summary.txt"
fi

echo "[matrix] all cases complete"

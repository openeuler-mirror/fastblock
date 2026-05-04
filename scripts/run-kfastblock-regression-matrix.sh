#!/bin/bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
matrix_root="${KFASTBLOCK_TEST_ARTIFACT_ROOT:-$repo_root/.artifacts/kfastblock-tests}"
run_dir="$matrix_root/matrix-$(date +%Y%m%d-%H%M%S)"
log_file="$run_dir/matrix.log"
run_sanitizer="${KFASTBLOCK_MATRIX_RUN_SANITIZER:-1}"
profile="${KFASTBLOCK_MATRIX_PROFILE:-default}"
case_filter="${KFASTBLOCK_MATRIX_CASES:-}"
reuse_cluster="${KFASTBLOCK_MATRIX_REUSE_CLUSTER:-0}"
fault_reuse_cluster="${KFASTBLOCK_MATRIX_FAULT_REUSE_CLUSTER:-0}"
refresh_duration="${KFASTBLOCK_MATRIX_REFRESH_DURATION_SEC:-8}"
lifecycle_duration="${KFASTBLOCK_MATRIX_LIFECYCLE_DURATION_SEC:-8}"
lifecycle_cycles="${KFASTBLOCK_MATRIX_LIFECYCLE_ATTACH_CYCLES:-1}"
fault_duration="${KFASTBLOCK_MATRIX_FAULT_DURATION_SEC:-12}"
fault_down_sec="${KFASTBLOCK_MATRIX_FAULT_DOWN_SEC:-2}"

mkdir -p "$run_dir"
exec > >(tee -a "$log_file") 2>&1

apply_profile() {
    case "$profile" in
        quick)
            refresh_duration=4
            lifecycle_duration=6
            lifecycle_cycles=1
            fault_duration=8
            fault_down_sec=2
            ;;
        default)
            ;;
        stress)
            refresh_duration=12
            lifecycle_duration=12
            lifecycle_cycles=2
            fault_duration=16
            fault_down_sec=3
            ;;
        *)
            echo "unknown profile: $profile" >&2
            exit 1
            ;;
    esac
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --skip-sanitizer)
            run_sanitizer=0
            ;;
        --with-sanitizer)
            run_sanitizer=1
            ;;
        --profile)
            shift
            profile="$1"
            ;;
        --cases)
            shift
            case_filter="$1"
            ;;
        --reuse-cluster)
            reuse_cluster=1
            ;;
        --list-cases)
            echo "smoke"
            echo "concurrency-refresh"
            echo "concurrency-lifecycle"
            echo "fault-injection"
            echo "sanitizer"
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 1
            ;;
    esac
    shift
done

apply_profile

printf 'case\tstatus\tartifact_dir\tnotes\n' > "$run_dir/summary.tsv"

run_case() {
    local case_name="$1"
    shift
    local case_log="$run_dir/${case_name}.log"
    local case_status="ok"
    local artifact_path=""
    local notes=""

    echo "[matrix] start $case_name"
    if ! (env KFASTBLOCK_TEST_ARTIFACT_ROOT="$run_dir" "$@" 2>&1 | tee "$case_log"); then
        echo "[matrix] fail $case_name" >&2
        case_status="fail"
    fi

    artifact_path="$(sed -n 's/^artifact_dir=//p' "$case_log" | tail -n 1)"
    if [ -n "$artifact_path" ]; then
        notes="$(collect_case_notes "$case_name" "$artifact_path")"
    fi
    printf '%s\t%s\t%s\t%s\n' "$case_name" "$case_status" "$artifact_path" "$notes" >> "$run_dir/summary.tsv"

    if [ "$case_status" = "ok" ]; then
        echo "[matrix] ok $case_name artifact=${artifact_path:-unknown} notes=${notes:-none}"
        printf '%s ok\n' "$case_name" >> "$run_dir/summary.txt"
    else
        printf '%s fail\n' "$case_name" >> "$run_dir/summary.txt"
        return 1
    fi
}

collect_case_notes() {
    local case_name="$1"
    local artifact_path="$2"
    local notes=""

    case "$case_name" in
        smoke)
            if [ -f "$artifact_path/smoke.log" ]; then
                notes="$(tail -n 1 "$artifact_path/smoke.log" 2>/dev/null || true)"
            fi
            ;;
        concurrency-refresh|concurrency-lifecycle)
            if [ -f "$artifact_path/attach-detach.summary" ]; then
                notes="$(tr '\n' ';' < "$artifact_path/attach-detach.summary")"
            fi
            if [ -f "$artifact_path/refresh.summary" ]; then
                if [ -n "$notes" ]; then
                    notes="${notes} "
                fi
                notes="${notes}$(tr '\n' ';' < "$artifact_path/refresh.summary")"
            fi
            if ls "$artifact_path"/open-*.summary >/dev/null 2>&1; then
                if [ -n "$notes" ]; then
                    notes="${notes} "
                fi
                notes="${notes}$(cat "$artifact_path"/open-*.summary | tr '\n' ';')"
            fi
            ;;
        fault-injection)
            if [ -f "$artifact_path/fault-io.summary" ]; then
                notes="$(tr '\n' ';' < "$artifact_path/fault-io.summary")"
            fi
            ;;
        sanitizer)
            if [ -f "$artifact_path/sanitizer.log" ]; then
                notes="$(tail -n 2 "$artifact_path/sanitizer.log" | tr '\n' ';' 2>/dev/null || true)"
            fi
            ;;
    esac

    printf '%s\n' "$notes"
}

case_enabled() {
    local case_name="$1"
    local item

    if [ -z "$case_filter" ]; then
        return 0
    fi

    IFS=',' read -r -a _matrix_cases <<< "$case_filter"
    for item in "${_matrix_cases[@]}"; do
        if [ "$item" = "$case_name" ]; then
            return 0
        fi
    done
    return 1
}

run_case_if_enabled() {
    local case_name="$1"
    shift

    if case_enabled "$case_name"; then
        run_case "$case_name" "$@"
    else
        echo "[matrix] skip $case_name (filtered)"
        printf '%s skipped\n' "$case_name" >> "$run_dir/summary.txt"
        printf '%s\t%s\t%s\t%s\n' "$case_name" "skipped" "" "" >> "$run_dir/summary.tsv"
    fi
}

echo "artifact_dir=$run_dir"

run_case_if_enabled smoke env \
    KFASTBLOCK_TEST_REUSE_CLUSTER="$reuse_cluster" \
    "$repo_root/scripts/run-kfastblock-dev-smoke.sh"
run_case_if_enabled concurrency-refresh env \
    KFASTBLOCK_TEST_REUSE_CLUSTER=1 \
    KFASTBLOCK_CONCURRENCY_DURATION_SEC="$refresh_duration" \
    KFASTBLOCK_CONCURRENCY_ATTACH_CYCLES=0 \
    KFASTBLOCK_CONCURRENCY_IO_WORKERS=2 \
    KFASTBLOCK_CONCURRENCY_OPEN_WORKERS=0 \
    "$repo_root/scripts/run-kfastblock-concurrency.sh"
run_case_if_enabled concurrency-lifecycle env \
    KFASTBLOCK_TEST_REUSE_CLUSTER=1 \
    KFASTBLOCK_CONCURRENCY_DURATION_SEC="$lifecycle_duration" \
    KFASTBLOCK_CONCURRENCY_ATTACH_CYCLES="$lifecycle_cycles" \
    KFASTBLOCK_CONCURRENCY_IO_WORKERS=1 \
    KFASTBLOCK_CONCURRENCY_OPEN_WORKERS=1 \
    "$repo_root/scripts/run-kfastblock-concurrency.sh"
run_case_if_enabled fault-injection env \
    KFASTBLOCK_TEST_REUSE_CLUSTER="$fault_reuse_cluster" \
    KFASTBLOCK_FAULT_IO_DURATION_SEC="$fault_duration" \
    KFASTBLOCK_FAULT_DOWN_SEC="$fault_down_sec" \
    "$repo_root/scripts/run-kfastblock-fault-injection.sh"

if [ "$run_sanitizer" = "1" ]; then
    run_case_if_enabled sanitizer env \
        KFASTBLOCK_TEST_REUSE_CLUSTER=1 \
        "$repo_root/scripts/run-kfastblock-sanitizer.sh"
else
    echo "[matrix] skip sanitizer"
    printf '%s skipped\n' "sanitizer" >> "$run_dir/summary.txt"
    printf '%s\t%s\t%s\t%s\n' "sanitizer" "skipped" "" "" >> "$run_dir/summary.tsv"
fi

echo "[matrix] all cases complete"

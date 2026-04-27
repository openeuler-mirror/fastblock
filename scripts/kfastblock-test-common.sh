#!/bin/bash

set -euo pipefail

KFASTBLOCK_TEST_LOCK_FD=""

kfastblock_repo_root() {
    cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd
}

kfastblock_require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "run as root" >&2
        exit 1
    fi
}

kfastblock_artifact_dir() {
    local repo_root="$1"
    local test_name="$2"
    local root_dir="${KFASTBLOCK_TEST_ARTIFACT_ROOT:-$repo_root/.artifacts/kfastblock-tests}"
    local run_dir="$root_dir/${test_name}-$(date +%Y%m%d-%H%M%S)"

    mkdir -p "$run_dir"
    printf '%s\n' "$run_dir"
}

kfastblock_start_logging() {
    local log_file="$1"

    mkdir -p "$(dirname "$log_file")"
    exec > >(tee -a "$log_file") 2>&1
}

kfastblock_local_state_root() {
    local repo_root="$1"

    printf '%s\n' "${FASTBLOCK_VSTART_STATE_ROOT:-$repo_root/.vstart}"
}

kfastblock_local_config_file() {
    local repo_root="$1"

    printf '%s/etc/fastblock/fastblock.json\n' "$(kfastblock_local_state_root "$repo_root")"
}

kfastblock_local_run_dir() {
    local repo_root="$1"

    printf '%s/run\n' "$(kfastblock_local_state_root "$repo_root")"
}

kfastblock_local_log_dir() {
    local repo_root="$1"

    printf '%s/var/log/fastblock\n' "$(kfastblock_local_state_root "$repo_root")"
}

kfastblock_wait_for_pid_exit() {
    local pid="$1"
    local timeout_sec="${2:-20}"
    local deadline=$((SECONDS + timeout_sec))

    while kill -0 "$pid" >/dev/null 2>&1; do
        if [ "$SECONDS" -ge "$deadline" ]; then
            kill -9 "$pid" >/dev/null 2>&1 || true
            break
        fi
        sleep 1
    done
}

kfastblock_stop_local_process() {
    local pid_file="$1"

    if [ ! -f "$pid_file" ]; then
        return 0
    fi

    local pid
    pid="$(cat "$pid_file" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" >/dev/null 2>&1; then
        kill "$pid" >/dev/null 2>&1 || true
        kfastblock_wait_for_pid_exit "$pid"
    fi
    rm -f "$pid_file"
}

kfastblock_start_local_process() {
    local pid_file="$1"
    local log_file="$2"
    shift 2

    mkdir -p "$(dirname "$pid_file")" "$(dirname "$log_file")"
    rm -f "$pid_file"
    setsid -f /bin/bash -lc '
        _pid_file=$1
        _log_file=$2
        shift 2
        if [ -n "${KFASTBLOCK_TEST_LOCK_FD:-}" ]; then
            eval "exec ${KFASTBLOCK_TEST_LOCK_FD}>&-"
        fi
        echo $$ > "$_pid_file"
        exec "$@" < /dev/null >> "$_log_file" 2>&1
    ' bash "$pid_file" "$log_file" "$@"

    local attempt
    for attempt in $(seq 1 10); do
        if [ -f "$pid_file" ]; then
            local pid
            pid="$(cat "$pid_file" 2>/dev/null || true)"
            if [ -n "$pid" ] && kill -0 "$pid" >/dev/null 2>&1; then
                return 0
            fi
        fi
        sleep 1
    done

    echo "failed to start local process: $*" >&2
    return 1
}

kfastblock_stop_local_osd() {
    local repo_root="$1"
    local osd_id="$2"
    local pid_file

    pid_file="$(kfastblock_local_run_dir "$repo_root")/osd-${osd_id}.pid"
    kfastblock_stop_local_process "$pid_file"
}

kfastblock_start_local_osd() {
    local repo_root="$1"
    local osd_id="$2"
    local config_file
    local pid_file
    local log_file
    local osd_binary

    config_file="$(kfastblock_local_config_file "$repo_root")"
    pid_file="$(kfastblock_local_run_dir "$repo_root")/osd-${osd_id}.pid"
    log_file="$(kfastblock_local_log_dir "$repo_root")/osd${osd_id}.log"
    osd_binary="$repo_root/build/src/osd/fastblock-osd"

    kfastblock_stop_local_osd "$repo_root" "$osd_id"
    kfastblock_start_local_process "$pid_file" "$log_file" \
        "$osd_binary" -C "$config_file" --id "$osd_id" -N 0
}

kfastblock_restart_local_osd() {
    local repo_root="$1"
    local osd_id="$2"
    local down_sleep="${3:-2}"

    kfastblock_stop_local_osd "$repo_root" "$osd_id"
    sleep "$down_sleep"
    kfastblock_start_local_osd "$repo_root" "$osd_id"
}

kfastblock_capture_failure_snapshot() {
    local repo_root="$1"
    local run_dir="$2"
    local pool_name="${3:-}"
    local image_name="${4:-}"
    local config_file
    local log_dir
    local admin="$repo_root/kfastblock/tool/kfastblock-admin"

    config_file="$(kfastblock_local_config_file "$repo_root")"
    log_dir="$(kfastblock_local_log_dir "$repo_root")"

    "$admin" list > "$run_dir/admin-list.txt" 2>&1 || true
    if [ -n "$pool_name" ] && [ -n "$image_name" ]; then
        "$admin" show --pool-name "$pool_name" --image-name "$image_name" \
            > "$run_dir/admin-show.txt" 2>&1 || true
    fi
    if [ -f "$config_file" ]; then
        cp "$config_file" "$run_dir/fastblock.failure.json" 2>/dev/null || true
    fi
    if command -v dmesg >/dev/null 2>&1; then
        dmesg | tail -n 200 > "$run_dir/dmesg.tail" 2>/dev/null || true
    fi
    if [ -d "$log_dir" ]; then
        for log_file in monitor.log osd1.log osd2.log osd3.log; do
            if [ -f "$log_dir/$log_file" ]; then
                tail -n 200 "$log_dir/$log_file" > "$run_dir/$log_file.tail" || true
            fi
        done
    fi
}

kfastblock_detach_all_volumes() {
    local repo_root="$1"
    local line
    local pool
    local image

    while IFS= read -r line; do
        [ -z "$line" ] && continue
        pool="$(printf '%s\n' "$line" | sed -n 's/.*pool=\([^ ]*\).*/\1/p')"
        image="$(printf '%s\n' "$line" | sed -n 's/.*image=\([^ ]*\).*/\1/p')"
        [ -z "$pool" ] && continue
        [ -z "$image" ] && continue
        "$repo_root/kfastblock/tool/kfastblock-admin" detach \
            --pool-name "$pool" \
            --image-name "$image" >/dev/null 2>&1 || true
    done < <("$repo_root/kfastblock/tool/kfastblock-admin" list 2>/dev/null || true)
}

kfastblock_wait_cluster_active() {
    local repo_root="$1"
    local config_file="$2"
    local attempt
    local status_output=""

    for attempt in $(seq 1 30); do
        status_output="$("$repo_root/monitor/fastblock-client" \
            -conf="$config_file" \
            -op=status 2>/dev/null || true)"
        if printf '%s\n' "$status_output" | grep -q "3 osds: 3 up, 3 in" &&
           printf '%s\n' "$status_output" | grep -q "8  active"; then
            return 0
        fi
        sleep 2
    done

    echo "cluster did not reach 3 up / 3 in / 8 active" >&2
    printf '%s\n' "$status_output" >&2
    return 1
}

kfastblock_prepare_dev_cluster() {
    local repo_root="$1"
    local config_file="$2"
    local nic="${KFASTBLOCK_DEV_NIC:-rdmanic}"
    local osd_count="${KFASTBLOCK_DEV_OSDCOUNT:-3}"
    local replica_count="${KFASTBLOCK_DEV_REPLICA:-3}"
    local cores="${KFASTBLOCK_DEV_CORES:-1}"

    kfastblock_detach_all_volumes "$repo_root"
    if ! rdma link show | grep -q "^link ${nic}/"; then
        "$repo_root/scripts/create-rdma-rxe.sh" -n "$nic"
    fi

    "$repo_root/vstart.sh" -m dev -c "$osd_count" -r "$replica_count" -C "$cores" -n "$nic"
    kfastblock_wait_cluster_active "$repo_root" "$config_file"
}

kfastblock_prepare_or_reuse_dev_cluster() {
    local repo_root="$1"
    local config_file="$2"

    if [ "${KFASTBLOCK_TEST_REUSE_CLUSTER:-0}" = "1" ] && [ -f "$config_file" ]; then
        echo "reuse existing dev cluster"
        kfastblock_detach_all_volumes "$repo_root"
        kfastblock_wait_cluster_active "$repo_root" "$config_file"
        return 0
    fi

    kfastblock_prepare_dev_cluster "$repo_root" "$config_file"
}

kfastblock_resolve_monitor_addr() {
    local config_file="$1"
    local monitor_addr

    monitor_addr="$(jq -r '.mon_rpc_address' "$config_file")"
    if [ -z "$monitor_addr" ] || [ "$monitor_addr" = "null" ]; then
        echo "failed to resolve monitor address from $config_file" >&2
        return 1
    fi

    printf '%s\n' "$monitor_addr"
}

kfastblock_create_image() {
    local repo_root="$1"
    local config_file="$2"
    local pool_name="$3"
    local image_name="$4"
    local image_size="${5:-67108864}"

    "$repo_root/monitor/fastblock-client" \
        -conf="$config_file" \
        -op=createimage \
        -poolname="$pool_name" \
        -imagename="$image_name" \
        -imagesize="$image_size" >/dev/null
}

kfastblock_run_rawctl_checks() {
    local repo_root="$1"
    local monitor_addr="$2"
    local pool_name="$3"
    local image_name="$4"
    local pool_id="${5:-1}"

    "$repo_root/kfastblock/tool/kfastblock-rawctl" \
        get-image-info \
        --addr "${monitor_addr}:3334" \
        --pool-name "$pool_name" \
        --image-name "$image_name" >/dev/null

    "$repo_root/kfastblock/tool/kfastblock-rawctl" \
        get-cluster-map \
        --addr "${monitor_addr}:3334" \
        --pool-id "$pool_id" >/dev/null
}

kfastblock_apply_module_params() {
    local module_params="$1"
    local param_entry
    local name
    local value
    local param_path

    [ -z "$module_params" ] && return 0

    for param_entry in $module_params; do
        name="${param_entry%%=*}"
        value="${param_entry#*=}"
        if [ -z "$name" ] || [ "$name" = "$param_entry" ]; then
            echo "invalid module param entry: $param_entry" >&2
            return 1
        fi
        param_path="/sys/module/kfastblock/parameters/$name"
        if [ ! -w "$param_path" ]; then
            echo "module parameter is not writable: $param_path" >&2
            return 1
        fi
        printf '%s\n' "$value" > "$param_path"
    done
}

kfastblock_build_and_load_module() {
    local repo_root="$1"
    local module_params="${KFASTBLOCK_MODULE_PARAMS:-}"
    local force_reload="${KFASTBLOCK_FORCE_RELOAD_MODULE:-0}"

    make -C "$repo_root/kfastblock" all >/dev/null
    if lsmod | awk '$1 == "kfastblock" { found = 1 } END { exit(found ? 0 : 1) }'; then
        if [ "$force_reload" = "1" ]; then
            kfastblock_detach_all_volumes "$repo_root"
            rmmod kfastblock
        else
            kfastblock_apply_module_params "$module_params"
            return 0
        fi
    fi

    if [ -n "$module_params" ]; then
        # shellcheck disable=SC2086
        insmod "$repo_root/kfastblock/kfastblock.ko" $module_params
    else
        insmod "$repo_root/kfastblock/kfastblock.ko"
    fi
}

kfastblock_attach_volume() {
    local repo_root="$1"
    local monitor_addr="$2"
    local pool_name="$3"
    local image_name="$4"

    "$repo_root/kfastblock/tool/kfastblock-admin" attach \
        --monitor-addr "${monitor_addr}:3334" \
        --pool-name "$pool_name" \
        --image-name "$image_name"
}

kfastblock_detach_volume() {
    local repo_root="$1"
    local pool_name="$2"
    local image_name="$3"

    "$repo_root/kfastblock/tool/kfastblock-admin" detach \
        --pool-name "$pool_name" \
        --image-name "$image_name"
}

kfastblock_set_dispatch_window() {
    local repo_root="$1"
    local pool_name="$2"
    local image_name="$3"
    local value="$4"

    "$repo_root/kfastblock/tool/kfastblock-admin" set-dispatch-window \
        --pool-name "$pool_name" \
        --image-name "$image_name" \
        --value "$value" >/dev/null
}

kfastblock_show_volume() {
    local repo_root="$1"
    local pool_name="$2"
    local image_name="$3"

    "$repo_root/kfastblock/tool/kfastblock-admin" show \
        --pool-name "$pool_name" \
        --image-name "$image_name"
}

kfastblock_resolve_device() {
    local device_path

    device_path="$(ls -1 /dev/kfb* 2>/dev/null | sort | head -n 1 || true)"
    if [ -z "$device_path" ]; then
        echo "kfastblock device not found" >&2
        return 1
    fi

    printf '%s\n' "$device_path"
}

kfastblock_capture_context() {
    local config_file="$1"
    local run_dir="$2"

    if [ -f "$config_file" ]; then
        cp "$config_file" "$run_dir/fastblock.json"
    fi
    uname -a > "$run_dir/uname.txt"
    if [ -r "/boot/config-$(uname -r)" ]; then
        cp "/boot/config-$(uname -r)" "$run_dir/kernel-config"
    fi
}

kfastblock_acquire_test_lock() {
    local repo_root="$1"
    local lock_file

    if [ "${KFASTBLOCK_TEST_NO_LOCK:-0}" = "1" ]; then
        return 0
    fi

    lock_file="$(kfastblock_local_state_root "$repo_root")/kfastblock-test.lock"
    mkdir -p "$(dirname "$lock_file")"
    exec {KFASTBLOCK_TEST_LOCK_FD}> "$lock_file"
    flock "$KFASTBLOCK_TEST_LOCK_FD"
    export KFASTBLOCK_TEST_LOCK_FD
}

kfastblock_release_test_lock() {
    if [ -n "$KFASTBLOCK_TEST_LOCK_FD" ]; then
        flock -u "$KFASTBLOCK_TEST_LOCK_FD" || true
        eval "exec ${KFASTBLOCK_TEST_LOCK_FD}>&-"
        KFASTBLOCK_TEST_LOCK_FD=""
        export KFASTBLOCK_TEST_LOCK_FD
    fi
}

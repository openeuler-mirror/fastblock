#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LAB_ROOT="${LAB_ROOT:-$ROOT/.lab-ready}"
CONFIG_PATH="$LAB_ROOT/etc/fastblock/fastblock.json"
RUN_DIR="$LAB_ROOT/run"

HOST_IP="${HOST_IP:-}"
POOL_NAME="${POOL_NAME:-fb}"
TRANSPORT="${TRANSPORT:-tcp}"
IMAGE_NAME="${IMAGE_NAME:-restart-base-$(date +%s)}"
SNAPSHOT_NAME="${SNAPSHOT_NAME:-snap1}"
CLONE_IMAGE_NAME="${CLONE_IMAGE_NAME:-${IMAGE_NAME}-clone}"
EXPORTER_ENDPOINT="${EXPORTER_ENDPOINT:-http://127.0.0.1:9500}"

MONITOR_BIN="$ROOT/monitor/fastblock-mon"
CLIENT_BIN="$ROOT/monitor/fastblock-client"
OSD_BIN="$ROOT/build/src/osd/fastblock-osd"
NVMF_BIN="$ROOT/build/src/bdev/fastblock-nvmf-tgt"
EXPORTER_BIN="$ROOT/exporter/bin/fastblock-exporter"

usage() {
    cat <<'EOF'
Usage:
  scripts/snapshot-restart-smoke.sh [options]

Options:
  --lab-root <path>         Lab root (default: .lab-ready)
  --host-ip <ip>            Monitor/target address (default: read from config)
  --pool <name>             Pool name (default: fb)
  --transport <tcp|rdma>    Export transport (default: tcp)
  --image <name>            Base image name
  --snapshot <name>         Snapshot name (default: snap1)
  --clone-image <name>      Clone image name
  --exporter-endpoint <u>   Exporter endpoint (default: http://127.0.0.1:9500)
  -h, --help                Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --lab-root)
            LAB_ROOT="$2"
            CONFIG_PATH="$LAB_ROOT/etc/fastblock/fastblock.json"
            RUN_DIR="$LAB_ROOT/run"
            shift 2
            ;;
        --host-ip)
            HOST_IP="$2"
            shift 2
            ;;
        --pool)
            POOL_NAME="$2"
            shift 2
            ;;
        --transport)
            TRANSPORT="$2"
            shift 2
            ;;
        --image)
            IMAGE_NAME="$2"
            shift 2
            ;;
        --snapshot)
            SNAPSHOT_NAME="$2"
            shift 2
            ;;
        --clone-image)
            CLONE_IMAGE_NAME="$2"
            shift 2
            ;;
        --exporter-endpoint)
            EXPORTER_ENDPOINT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

log() {
    printf '[snapshot-restart] %s\n' "$*"
}

die() {
    printf '[snapshot-restart] ERROR: %s\n' "$*" >&2
    exit 1
}

require_commands() {
    local missing=()
    local cmd
    for cmd in curl python3 ip; do
        command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
    done
    if [[ ${#missing[@]} -ne 0 ]]; then
        die "missing required commands: ${missing[*]}"
    fi
}

ensure_inputs() {
    [[ -x "$MONITOR_BIN" ]] || die "missing binary: $MONITOR_BIN"
    [[ -x "$CLIENT_BIN" ]] || die "missing binary: $CLIENT_BIN"
    [[ -x "$OSD_BIN" ]] || die "missing binary: $OSD_BIN"
    [[ -x "$NVMF_BIN" ]] || die "missing binary: $NVMF_BIN"
    [[ -x "$EXPORTER_BIN" ]] || die "missing binary: $EXPORTER_BIN"
    [[ -f "$CONFIG_PATH" ]] || die "missing config: $CONFIG_PATH"
    [[ -f "$RUN_DIR/osd.id" ]] || die "missing osd id file: $RUN_DIR/osd.id"
    if [[ -z "$HOST_IP" ]]; then
        HOST_IP="$(python3 - <<'PY' "$CONFIG_PATH"
import json, sys
cfg = json.load(open(sys.argv[1]))
print(cfg.get("mon_rpc_address", ""))
PY
)"
    fi
    [[ -n "$HOST_IP" ]] || die "failed to detect host ip"
}

monitor_client() {
    "$CLIENT_BIN" -conf="$CONFIG_PATH" "$@"
}

http_json() {
    local method="$1"
    local path="$2"
    local body="${3:-}"
    if [[ -n "$body" ]]; then
        curl -fsS -X "$method" -H 'Content-Type: application/json' "$EXPORTER_ENDPOINT$path" -d "$body"
    else
        curl -fsS -X "$method" "$EXPORTER_ENDPOINT$path"
    fi
}

wait_for_monitor() {
    local i
    for ((i = 0; i < 30; i++)); do
        if monitor_client -op=status >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_for_osd_up() {
    local i status
    for ((i = 0; i < 60; i++)); do
        status="$(monitor_client -op=status 2>&1 || true)"
        if grep -q "1 osds: 1 up, 1 in" <<<"$status"; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_for_exporter() {
    local i
    for ((i = 0; i < 30; i++)); do
        if curl -fsS "$EXPORTER_ENDPOINT/healthz" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_for_spdk_sock() {
    local i sock
    for ((i = 0; i < 30; i++)); do
        sock="$(ls -t /var/tmp/fastblock_nvmf_tgt*.sock 2>/dev/null | head -n1 || true)"
        if [[ -n "$sock" && -S "$sock" ]]; then
            printf '%s\n' "$sock"
            return 0
        fi
        sleep 1
    done
    return 1
}

stop_pidfile() {
    local file="$1"
    if [[ ! -f "$file" ]]; then
        return 0
    fi
    local pid
    pid="$(cat "$file" 2>/dev/null || true)"
    if [[ -z "$pid" ]]; then
        return 0
    fi
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" || true
        for _ in $(seq 1 20); do
            if ! kill -0 "$pid" 2>/dev/null; then
                return 0
            fi
            sleep 1
        done
        kill -9 "$pid" || true
    fi
}

restart_osd() {
    log "restarting osd"
    stop_pidfile "$RUN_DIR/osd.pid"
    local osd_id
    osd_id="$(cat "$RUN_DIR/osd.id")"
    nohup "$OSD_BIN" -C "$CONFIG_PATH" --id "$osd_id" -N 0 >"$LAB_ROOT/var/log/fastblock/osd.restart.log" 2>&1 &
    echo $! > "$RUN_DIR/osd.pid"
    wait_for_osd_up || die "osd did not return to up/in"
}

restart_nvmf_and_exporter() {
    log "restarting nvmf target"
    stop_pidfile "$RUN_DIR/nvmf_tgt.pid"
    rm -f /var/tmp/fastblock_nvmf_tgt*.sock
    nohup "$NVMF_BIN" -s 2048 -C "$CONFIG_PATH" -S 1 >"$LAB_ROOT/var/log/fastblock/nvmf_tgt.restart.log" 2>&1 &
    echo $! > "$RUN_DIR/nvmf_tgt.pid"
    local sock
    sock="$(wait_for_spdk_sock)" || die "spdk rpc socket not ready after nvmf restart"

    log "restarting exporter"
    stop_pidfile "$RUN_DIR/exporter.pid"
    nohup "$EXPORTER_BIN" \
        -listen :9500 \
        -monitor-address "$HOST_IP:3333" \
        -node-name node-a \
        -spdk-rpc-sock "$sock" \
        -target-address "$HOST_IP" \
        -target-service-id 4420 \
        -nqn-prefix nqn.2026-04.io.fastblock >"$LAB_ROOT/var/log/fastblock/exporter.restart.log" 2>&1 &
    echo $! > "$RUN_DIR/exporter.pid"
    wait_for_exporter || die "exporter did not become healthy after restart"
}

create_image_if_needed() {
    local out
    out="$(monitor_client -op=getimage -poolname="$POOL_NAME" -imagename="$IMAGE_NAME" 2>&1 || true)"
    if grep -q "Image not found" <<<"$out"; then
        monitor_client -op=createimage -poolname="$POOL_NAME" -imagename="$IMAGE_NAME" -imagesize=$((64*1024*1024)) >/dev/null
    fi
}

export_id() {
    printf '%s' "$1"
}

main() {
    require_commands
    ensure_inputs
    wait_for_monitor || die "monitor is not ready"
    create_image_if_needed

    local base_export_id clone_export_id
    base_export_id="$(export_id "$IMAGE_NAME-exp")"
    clone_export_id="$(export_id "$CLONE_IMAGE_NAME-exp")"

    log "creating export for base image"
    http_json POST /v1/exports "{\"VolumeID\":\"$base_export_id\",\"PoolName\":\"$POOL_NAME\",\"ImageName\":\"$IMAGE_NAME\",\"BlockSize\":4096,\"Transport\":\"$TRANSPORT\",\"AllowAnyHost\":true}" >/dev/null

    log "creating snapshot"
    http_json POST "/v1/exports/$base_export_id/snapshots" "{\"snapshot_name\":\"$SNAPSHOT_NAME\"}" >/dev/null
    log "protecting snapshot"
    http_json POST "/v1/exports/$base_export_id/snapshots/$SNAPSHOT_NAME/protect" >/dev/null
    log "creating clone"
    http_json POST "/v1/exports/$base_export_id/snapshots/$SNAPSHOT_NAME/clone" "{\"clone_image_name\":\"$CLONE_IMAGE_NAME\"}" >/dev/null

    restart_osd
    restart_nvmf_and_exporter

    log "verifying snapshot still visible after restart"
    http_json GET "/v1/exports/$base_export_id/snapshots/$SNAPSHOT_NAME" >/dev/null
    log "verifying clone export after restart"
    http_json POST /v1/exports "{\"VolumeID\":\"$clone_export_id\",\"PoolName\":\"$POOL_NAME\",\"ImageName\":\"$CLONE_IMAGE_NAME\",\"BlockSize\":4096,\"Transport\":\"$TRANSPORT\",\"AllowAnyHost\":true}" >/dev/null
    log "verifying rollback trigger after restart"
    http_json POST "/v1/exports/$base_export_id/snapshots/$SNAPSHOT_NAME/rollback" >/dev/null

    log "snapshot restart smoke finished"
}

main "$@"

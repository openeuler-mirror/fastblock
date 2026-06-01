#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LAB_ROOT="${LAB_ROOT:-$ROOT/.lab-ready}"
CONFIG_PATH="$LAB_ROOT/etc/fastblock/fastblock.json"
LOG_DIR="$LAB_ROOT/var/log/fastblock"

POOL_NAME="${POOL_NAME:-fb}"
TRANSPORT="${TRANSPORT:-tcp}"
IMAGE_NAME="${IMAGE_NAME:-rollback-proof-base-$(date +%s)}"
SNAPSHOT_NAME="${SNAPSHOT_NAME:-snap-proof}"
VOLUME_ID="${VOLUME_ID:-${IMAGE_NAME}-exp}"
EXPORTER_ENDPOINT="${EXPORTER_ENDPOINT:-http://127.0.0.1:9500}"
IMAGE_SIZE_BYTES="${IMAGE_SIZE_BYTES:-16777216}"
OBJECT_SIZE_BYTES="${OBJECT_SIZE_BYTES:-4194304}"
BLOCK_SIZE_BYTES=4096
SECOND_OBJECT_BLOCK_INDEX=$((OBJECT_SIZE_BYTES / BLOCK_SIZE_BYTES))
KEEP_EXPORT="${KEEP_EXPORT:-0}"

MONITOR_BIN="$ROOT/monitor/fastblock-client"

TMPDIR=""
EXPORT_ID=""
EXPORT_NQN=""
EXPORT_TRADDR=""
EXPORT_TRSVCID=""
DEVICE_PATH=""
NVMF_LOG_BASELINE=0
OSD_LOG_BASELINE=0

usage() {
    cat <<'EOF'
Usage:
  scripts/rollback-proof.sh [options]

Options:
  --lab-root <path>          Lab root (default: .lab-ready)
  --pool <name>              Pool name (default: fb)
  --transport <tcp|rdma>     Export transport (default: tcp)
  --image <name>             Base image name (default: rollback-proof-base-<ts>)
  --snapshot <name>          Snapshot name (default: snap-proof)
  --volume-id <id>           Exporter volume id (default: <image>-exp)
  --exporter-endpoint <url>  Exporter endpoint (default: http://127.0.0.1:9500)
  --keep-export              Keep export and NVMe connection for debugging
  -h, --help                 Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --lab-root)
            LAB_ROOT="$2"
            CONFIG_PATH="$LAB_ROOT/etc/fastblock/fastblock.json"
            LOG_DIR="$LAB_ROOT/var/log/fastblock"
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
        --volume-id)
            VOLUME_ID="$2"
            shift 2
            ;;
        --exporter-endpoint)
            EXPORTER_ENDPOINT="$2"
            shift 2
            ;;
        --keep-export)
            KEEP_EXPORT=1
            shift
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
    printf '[rollback-proof] %s\n' "$*"
}

die() {
    printf '[rollback-proof] ERROR: %s\n' "$*" >&2
    exit 1
}

require_root() {
    if [[ "$(id -u)" -ne 0 ]]; then
        die "this script must run as root"
    fi
}

require_commands() {
    local missing=()
    local cmd
    for cmd in curl dd modprobe nvme python3 timeout; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            missing+=("$cmd")
        fi
    done
    if [[ ${#missing[@]} -ne 0 ]]; then
        die "missing required commands: ${missing[*]}"
    fi
}

ensure_inputs() {
    [[ -x "$MONITOR_BIN" ]] || die "missing binary: $MONITOR_BIN"
    [[ -f "$CONFIG_PATH" ]] || die "missing config: $CONFIG_PATH"
    [[ "$TRANSPORT" == "tcp" || "$TRANSPORT" == "rdma" ]] || die "unsupported transport: $TRANSPORT"
}

monitor_client() {
    "$MONITOR_BIN" -conf="$CONFIG_PATH" "$@"
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

json_get() {
    local key="$1"
    python3 - "$key" <<'PY'
import json
import sys

key = sys.argv[1]
value = json.load(sys.stdin)
for part in key.split("."):
    if isinstance(value, dict):
        value = value.get(part)
    else:
        raise SystemExit(f"unsupported path: {key}")
if isinstance(value, bool):
    print("true" if value else "false")
elif value is None:
    print("")
else:
    print(value)
PY
}

volume_export_id() {
    printf '%s' "$1" | tr ':/. ' '-----'
}

ensure_hostnqn() {
    mkdir -p /etc/nvme
    if [[ -s /etc/nvme/hostnqn ]]; then
        return
    fi
    if nvme gen-hostnqn > /etc/nvme/hostnqn 2>/dev/null; then
        return
    fi
    python3 - <<'PY' >/etc/nvme/hostnqn
import uuid
print(f"nqn.2014-08.org.nvmexpress:uuid:{uuid.uuid4()}")
PY
}

controllers_for_nqn() {
    local nqn="$1"
    local ctrl
    for ctrl in /sys/class/nvme/nvme*; do
        [[ -e "$ctrl/subsysnqn" ]] || continue
        if [[ "$(cat "$ctrl/subsysnqn" 2>/dev/null || true)" == "$nqn" ]]; then
            basename "$ctrl"
        fi
    done
}

force_delete_controllers() {
    local nqn="$1"
    local ctrl
    while read -r ctrl; do
        [[ -n "$ctrl" ]] || continue
        if [[ -w "/sys/class/nvme/$ctrl/delete_controller" ]]; then
            log "force delete controller $ctrl for nqn=$nqn"
            echo 1 > "/sys/class/nvme/$ctrl/delete_controller" || true
        fi
    done < <(controllers_for_nqn "$nqn")
}

wait_controllers_gone() {
    local nqn="$1"
    local i
    for ((i = 0; i < 20; i++)); do
        if [[ -z "$(controllers_for_nqn "$nqn")" ]]; then
            return 0
        fi
        sleep 1
    done
    return 1
}

disconnect_nqn() {
    local nqn="$1"
    [[ -n "$nqn" ]] || return 0
    timeout 5s nvme disconnect -n "$nqn" >/dev/null 2>&1 || true
    force_delete_controllers "$nqn"
    wait_controllers_gone "$nqn" || true
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

create_image_if_needed() {
    local out
    out="$(monitor_client -op=getimage -poolname="$POOL_NAME" -imagename="$IMAGE_NAME" 2>&1 || true)"
    if grep -q "Image not found" <<<"$out"; then
        log "creating image $POOL_NAME/$IMAGE_NAME size=$IMAGE_SIZE_BYTES object_size=$OBJECT_SIZE_BYTES"
        monitor_client \
            -op=createimage \
            -poolname="$POOL_NAME" \
            -imagename="$IMAGE_NAME" \
            -imagesize="$IMAGE_SIZE_BYTES" \
            -objectsize="$OBJECT_SIZE_BYTES" >/dev/null
        return
    fi
    log "image $POOL_NAME/$IMAGE_NAME already exists"
}

capture_log_baselines() {
    local nvmf_log="$LOG_DIR/nvmf_tgt.log"
    local osd_log="$LOG_DIR/osd.log"
    if [[ -f "$nvmf_log" ]]; then
        NVMF_LOG_BASELINE="$(wc -l < "$nvmf_log")"
    fi
    if [[ -f "$osd_log" ]]; then
        OSD_LOG_BASELINE="$(wc -l < "$osd_log")"
    fi
}

dump_new_log_matches() {
    local file="$1"
    local start_line="$2"
    local pattern="$3"
    [[ -f "$file" ]] || return 0
    local first_line=$((start_line + 1))
    sed -n "${first_line},\$p" "$file" | grep -En "$pattern" || true
}

print_relevant_logs() {
    printf '\n'
    log "nvmf_tgt log matches"
    dump_new_log_matches \
        "$LOG_DIR/nvmf_tgt.log" \
        "$NVMF_LOG_BASELINE" \
        'create snapshot monitor response|snapshot seq advanced|rollback issue read|rollback snapshot read done|rollback write done|rollback image .* finished'
    log "osd log matches"
    dump_new_log_matches \
        "$LOG_DIR/osd.log" \
        "$OSD_LOG_BASELINE" \
        'osd read request|snapshot read select snapshot|snapshot read select none|snapshot read fall back to head|prewrite snapshot'
}

prepare_pattern_files() {
    TMPDIR="$(mktemp -d /tmp/rollback-proof.XXXXXX)"
    python3 - "$TMPDIR" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
patterns = {
    "seg0_a.bin": b"@@SEG0@@A\n",
    "seg0_b.bin": b"@@SEG0@@B\n",
    "seg1_c.bin": b"@@SEG1@@C\n",
}
for name, prefix in patterns.items():
    data = prefix + bytes(4096 - len(prefix))
    (root / name).write_bytes(data)
PY
}

cleanup() {
    local rc=$?
    if [[ -n "$EXPORT_NQN" && "$KEEP_EXPORT" -eq 0 ]]; then
        disconnect_nqn "$EXPORT_NQN"
    fi
    if [[ -n "$EXPORT_ID" && "$KEEP_EXPORT" -eq 0 ]]; then
        http_json DELETE "/v1/exports/$EXPORT_ID" >/dev/null 2>&1 || true
    fi
    if [[ -n "$TMPDIR" && -d "$TMPDIR" ]]; then
        rm -rf "$TMPDIR"
    fi
    exit "$rc"
}

create_export() {
    local body
    body="$(python3 - "$VOLUME_ID" "$POOL_NAME" "$IMAGE_NAME" "$BLOCK_SIZE_BYTES" "$TRANSPORT" <<'PY'
import json
import sys

volume_id, pool_name, image_name, block_size, transport = sys.argv[1:6]
print(json.dumps({
    "volume_id": volume_id,
    "pool_name": pool_name,
    "image_name": image_name,
    "block_size": int(block_size),
    "transport": transport,
    "allow_any_host": True,
}))
PY
)"
    http_json POST /v1/exports "$body" >/dev/null
}

load_export_info() {
    local export_json
    export_json="$(http_json GET "/v1/exports/$EXPORT_ID")"
    EXPORT_NQN="$(printf '%s' "$export_json" | json_get nqn)"
    EXPORT_TRADDR="$(printf '%s' "$export_json" | json_get traddr)"
    EXPORT_TRSVCID="$(printf '%s' "$export_json" | json_get trsvcid)"
}

wait_device_for_nqn() {
    local nqn="$1"
    local i ctrl dev
    for ((i = 0; i < 30; i++)); do
        while read -r ctrl; do
            [[ -n "$ctrl" ]] || continue
            for dev in /sys/class/nvme/"$ctrl"/"${ctrl}"n*; do
                [[ -e "$dev" ]] || continue
                if [[ -b "/dev/$(basename "$dev")" ]]; then
                    printf '/dev/%s\n' "$(basename "$dev")"
                    return 0
                fi
            done
        done < <(controllers_for_nqn "$nqn")
        sleep 1
    done
    return 1
}

connect_export() {
    ensure_hostnqn
    modprobe nvme-fabrics
    if [[ "$TRANSPORT" == "tcp" ]]; then
        modprobe nvme-tcp
    else
        modprobe nvme-rdma
    fi

    disconnect_nqn "$EXPORT_NQN"
    log "nvme discover transport=$TRANSPORT traddr=$EXPORT_TRADDR trsvcid=$EXPORT_TRSVCID"
    nvme discover -t "$TRANSPORT" -a "$EXPORT_TRADDR" -s "$EXPORT_TRSVCID" >/dev/null
    log "nvme connect nqn=$EXPORT_NQN"
    nvme connect -t "$TRANSPORT" -n "$EXPORT_NQN" -a "$EXPORT_TRADDR" -s "$EXPORT_TRSVCID" >/dev/null
    DEVICE_PATH="$(wait_device_for_nqn "$EXPORT_NQN")" || die "failed to locate device for nqn $EXPORT_NQN"
    log "connected device $DEVICE_PATH"
}

write_block() {
    local file="$1"
    local block_index="$2"
    dd if="$file" of="$DEVICE_PATH" bs="$BLOCK_SIZE_BYTES" seek="$block_index" count=1 \
        oflag=direct conv=fsync,notrunc status=none
}

read_block() {
    local block_index="$1"
    local out_file="$2"
    dd if="$DEVICE_PATH" of="$out_file" bs="$BLOCK_SIZE_BYTES" skip="$block_index" count=1 iflag=direct status=none
}

verify_prefix() {
    local file="$1"
    local expect="$2"
    python3 - "$file" "$expect" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
expect = sys.argv[2].encode()
data = path.read_bytes()
if not data.startswith(expect):
    raise SystemExit(1)
PY
}

verify_zero() {
    local file="$1"
    python3 - "$file" <<'PY'
import pathlib
import sys

data = pathlib.Path(sys.argv[1]).read_bytes()
if any(data):
    raise SystemExit(1)
PY
}

create_snapshot() {
    http_json POST "/v1/exports/$EXPORT_ID/snapshots" "{\"snapshot_name\":\"$SNAPSHOT_NAME\"}" >/dev/null
}

rollback_snapshot() {
    http_json POST "/v1/exports/$EXPORT_ID/snapshots/$SNAPSHOT_NAME/rollback" >/dev/null
}

assert_log_contains() {
    local file="$1"
    local start_line="$2"
    local pattern="$3"
    [[ -f "$file" ]] || die "missing log file: $file"
    local first_line=$((start_line + 1))
    if ! sed -n "${first_line},\$p" "$file" | grep -Eq "$pattern"; then
        die "expected log pattern not found in $(basename "$file"): $pattern"
    fi
}

warn_on_log_pattern() {
    local file="$1"
    local start_line="$2"
    local pattern="$3"
    [[ -f "$file" ]] || return 0
    local first_line=$((start_line + 1))
    if sed -n "${first_line},\$p" "$file" | grep -Eq "$pattern"; then
        log "warning: suspicious log pattern seen in $(basename "$file"): $pattern"
    fi
}

run_proof() {
    local seg0_read="$TMPDIR/read_seg0.bin"
    local seg1_read="$TMPDIR/read_seg1.bin"

    log "writing baseline SEG0=A at block 0"
    write_block "$TMPDIR/seg0_a.bin" 0

    log "creating snapshot $SNAPSHOT_NAME"
    create_snapshot

    log "overwriting SEG0=B at block 0 after snapshot"
    write_block "$TMPDIR/seg0_b.bin" 0

    log "writing SEG1=C at block $SECOND_OBJECT_BLOCK_INDEX after snapshot"
    write_block "$TMPDIR/seg1_c.bin" "$SECOND_OBJECT_BLOCK_INDEX"

    log "rolling back export $EXPORT_ID to snapshot $SNAPSHOT_NAME"
    rollback_snapshot

    log "reading SEG0 after rollback"
    read_block 0 "$seg0_read"
    verify_prefix "$seg0_read" '@@SEG0@@A' || die "SEG0 verification failed after rollback"

    log "reading SEG1 after rollback"
    read_block "$SECOND_OBJECT_BLOCK_INDEX" "$seg1_read"
    verify_zero "$seg1_read" || die "SEG1 zero verification failed after rollback"

    assert_log_contains "$LOG_DIR/nvmf_tgt.log" "$NVMF_LOG_BASELINE" 'rollback issue read'
    assert_log_contains "$LOG_DIR/nvmf_tgt.log" "$NVMF_LOG_BASELINE" 'rollback snapshot read done'
    assert_log_contains "$LOG_DIR/nvmf_tgt.log" "$NVMF_LOG_BASELINE" 'rollback image .* finished with state 0'
    assert_log_contains "$LOG_DIR/osd.log" "$OSD_LOG_BASELINE" 'snapshot read select snapshot'
    assert_log_contains "$LOG_DIR/osd.log" "$OSD_LOG_BASELINE" 'snapshot read select none'
    warn_on_log_pattern "$LOG_DIR/osd.log" "$OSD_LOG_BASELINE" 'snapshot read fall back to head'
    warn_on_log_pattern "$LOG_DIR/osd.log" "$OSD_LOG_BASELINE" 'prewrite snapshot'
}

main() {
    require_root
    require_commands
    ensure_inputs
    trap cleanup EXIT

    wait_for_monitor || die "monitor is not ready; start the lab first"
    wait_for_exporter || die "exporter is not ready; start the lab first"

    prepare_pattern_files
    capture_log_baselines
    create_image_if_needed

    EXPORT_ID="$(volume_export_id "$VOLUME_ID")"
    log "creating export $EXPORT_ID for $POOL_NAME/$IMAGE_NAME"
    create_export
    load_export_info
    connect_export

    run_proof
    print_relevant_logs
    log "rollback proof passed for image=$IMAGE_NAME snapshot=$SNAPSHOT_NAME device=$DEVICE_PATH"
}

main "$@"

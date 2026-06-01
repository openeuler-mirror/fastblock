#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ACTION="${1:-run}"
if [[ $# -gt 0 ]]; then
    shift
fi

EXPORTER_ENDPOINT="${EXPORTER_ENDPOINT:-http://127.0.0.1:9500}"
POOL_NAME="${POOL_NAME:-fb}"
IMAGE_NAME="${IMAGE_NAME:-}"
CLONE_IMAGE_NAME="${CLONE_IMAGE_NAME:-}"
SNAPSHOT_NAME="${SNAPSHOT_NAME:-snap-$(date +%s)}"
TRANSPORT="${TRANSPORT:-tcp}"
BLOCK_SIZE="${BLOCK_SIZE:-4096}"
ALLOW_ANY_HOST="${ALLOW_ANY_HOST:-1}"
CONNECT_NVME="${CONNECT_NVME:-0}"
KEEP_EXPORTS="${KEEP_EXPORTS:-0}"
BASE_VOLUME_ID="${BASE_VOLUME_ID:-nvmf-smoke-base-$(date +%s)}"
CLONE_VOLUME_ID="${CLONE_VOLUME_ID:-nvmf-smoke-clone-$(date +%s)}"

usage() {
    cat <<'EOF'
Usage:
  scripts/nvmf-snapshot-smoke.sh run [options]

Actions:
  run      Export base image, create snapshot, protect, clone, export clone, optional nvme connect, rollback base, and flatten clone.

Options:
  --exporter-endpoint <url>   Exporter HTTP endpoint (default: http://127.0.0.1:9500)
  --pool <name>               Pool name (default: fb)
  --image <name>              Existing base image name
  --clone-image <name>        Clone image name (default: <image>-clone-<ts>)
  --snapshot <name>           Snapshot name
  --transport <tcp|rdma>      Export transport (default: tcp)
  --block-size <bytes>        Export block size (default: 4096)
  --base-volume-id <id>       Exporter volume id for base export
  --clone-volume-id <id>      Exporter volume id for clone export
  --allow-any-host <0|1>      Create export with allow_any_host (default: 1)
  --connect                   Run nvme discover/connect for clone export
  --keep-exports              Keep base/clone exports after the flow
  -h, --help                  Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --exporter-endpoint)
            EXPORTER_ENDPOINT="$2"
            shift 2
            ;;
        --pool)
            POOL_NAME="$2"
            shift 2
            ;;
        --image)
            IMAGE_NAME="$2"
            shift 2
            ;;
        --clone-image)
            CLONE_IMAGE_NAME="$2"
            shift 2
            ;;
        --snapshot)
            SNAPSHOT_NAME="$2"
            shift 2
            ;;
        --transport)
            TRANSPORT="$2"
            shift 2
            ;;
        --block-size)
            BLOCK_SIZE="$2"
            shift 2
            ;;
        --base-volume-id)
            BASE_VOLUME_ID="$2"
            shift 2
            ;;
        --clone-volume-id)
            CLONE_VOLUME_ID="$2"
            shift 2
            ;;
        --allow-any-host)
            ALLOW_ANY_HOST="$2"
            shift 2
            ;;
        --connect)
            CONNECT_NVME=1
            shift
            ;;
        --keep-exports)
            KEEP_EXPORTS=1
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
    printf '[nvmf-smoke] %s\n' "$*"
}

die() {
    printf '[nvmf-smoke] ERROR: %s\n' "$*" >&2
    exit 1
}

require_commands() {
    local missing=()
    local cmd
    for cmd in curl python3; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            missing+=("$cmd")
        fi
    done
    if [[ "$CONNECT_NVME" -eq 1 ]]; then
        for cmd in nvme modprobe; do
            if ! command -v "$cmd" >/dev/null 2>&1; then
                missing+=("$cmd")
            fi
        done
    fi
    if [[ ${#missing[@]} -ne 0 ]]; then
        die "missing required commands: ${missing[*]}"
    fi
}

ensure_inputs() {
    [[ -n "$IMAGE_NAME" ]] || die "--image is required"
    if [[ -z "$CLONE_IMAGE_NAME" ]]; then
        CLONE_IMAGE_NAME="${IMAGE_NAME}-clone-$(date +%s)"
    fi
    [[ "$TRANSPORT" == "tcp" || "$TRANSPORT" == "rdma" ]] || die "unsupported transport: $TRANSPORT"
}

ensure_hostnqn() {
    mkdir -p /etc/nvme
    if [[ -s /etc/nvme/hostnqn ]]; then
        return
    fi
    if command -v nvme >/dev/null 2>&1; then
        if nvme gen-hostnqn > /etc/nvme/hostnqn; then
            return
        fi
    fi
    python3 - <<'PY' >/etc/nvme/hostnqn
import uuid
print(f"nqn.2014-08.org.nvmexpress:uuid:{uuid.uuid4()}")
PY
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
data = json.load(sys.stdin)
value = data
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

create_export() {
    local volume_id="$1"
    local image_name="$2"
    local body
    body="$(python3 - "$volume_id" "$POOL_NAME" "$image_name" "$BLOCK_SIZE" "$TRANSPORT" "$ALLOW_ANY_HOST" <<'PY'
import json
import sys
volume_id, pool_name, image_name, block_size, transport, allow_any_host = sys.argv[1:7]
print(json.dumps({
    "volume_id": volume_id,
    "pool_name": pool_name,
    "image_name": image_name,
    "block_size": int(block_size),
    "transport": transport,
    "allow_any_host": allow_any_host == "1",
}))
PY
)"
    http_json POST /v1/exports "$body"
}

cleanup_exports() {
    if [[ "$KEEP_EXPORTS" -eq 1 ]]; then
        return
    fi
    http_json DELETE "/v1/exports/$(volume_export_id "$CLONE_VOLUME_ID")" || true
    http_json DELETE "/v1/exports/$(volume_export_id "$BASE_VOLUME_ID")" || true
}

create_snapshot() {
    local export_id="$1"
    local snapshot_name="$2"
    http_json POST "/v1/exports/$export_id/snapshots" "{\"snapshot_name\":\"$snapshot_name\"}" >/dev/null
}

protect_snapshot() {
    local export_id="$1"
    local snapshot_name="$2"
    http_json POST "/v1/exports/$export_id/snapshots/$snapshot_name/protect" >/dev/null
}

get_snapshot() {
    local export_id="$1"
    local snapshot_name="$2"
    http_json GET "/v1/exports/$export_id/snapshots/$snapshot_name"
}

create_clone() {
    local export_id="$1"
    local snapshot_name="$2"
    local clone_image_name="$3"
    http_json POST "/v1/exports/$export_id/snapshots/$snapshot_name/clone" "{\"clone_image_name\":\"$clone_image_name\"}" >/dev/null
}

flatten_export() {
    local export_id="$1"
    http_json POST "/v1/exports/$export_id/flatten" >/dev/null
}

rollback_snapshot() {
    local export_id="$1"
    local snapshot_name="$2"
    http_json POST "/v1/exports/$export_id/snapshots/$snapshot_name/rollback" >/dev/null
}

connect_clone_export() {
    ensure_hostnqn
    modprobe nvme-fabrics
    if [[ "$TRANSPORT" == "tcp" ]]; then
        modprobe nvme-tcp
    else
        modprobe nvme-rdma
    fi

    local export_json traddr trsvcid nqn
    export_json="$(http_json GET "/v1/exports/$(volume_export_id "$CLONE_VOLUME_ID")")"
    traddr="$(printf '%s' "$export_json" | json_get traddr)"
    trsvcid="$(printf '%s' "$export_json" | json_get trsvcid)"
    nqn="$(printf '%s' "$export_json" | json_get nqn)"

    log "nvme discover transport=$TRANSPORT traddr=$traddr trsvcid=$trsvcid"
    nvme discover -t "$TRANSPORT" -a "$traddr" -s "$trsvcid" >/dev/null
    log "nvme connect nqn=$nqn"
    nvme connect -t "$TRANSPORT" -n "$nqn" -a "$traddr" -s "$trsvcid"
}

disconnect_clone_export() {
    local export_json nqn
    export_json="$(http_json GET "/v1/exports/$(volume_export_id "$CLONE_VOLUME_ID")")"
    nqn="$(printf '%s' "$export_json" | json_get nqn)"
    if [[ -n "$nqn" ]]; then
        log "nvme disconnect nqn=$nqn"
        nvme disconnect -n "$nqn" || true
    fi
}

run_flow() {
    local base_export clone_export

    log "exporting base image $POOL_NAME/$IMAGE_NAME"
    base_export="$(create_export "$BASE_VOLUME_ID" "$IMAGE_NAME")"
    log "base export: $base_export"

    log "creating snapshot $SNAPSHOT_NAME"
    create_snapshot "$(volume_export_id "$BASE_VOLUME_ID")" "$SNAPSHOT_NAME"
    log "protecting snapshot $SNAPSHOT_NAME"
    protect_snapshot "$(volume_export_id "$BASE_VOLUME_ID")" "$SNAPSHOT_NAME"
    log "snapshot info"
    get_snapshot "$(volume_export_id "$BASE_VOLUME_ID")" "$SNAPSHOT_NAME"

    log "creating clone image $CLONE_IMAGE_NAME from snapshot $SNAPSHOT_NAME"
    create_clone "$(volume_export_id "$BASE_VOLUME_ID")" "$SNAPSHOT_NAME" "$CLONE_IMAGE_NAME"

    log "exporting clone image $POOL_NAME/$CLONE_IMAGE_NAME"
    clone_export="$(create_export "$CLONE_VOLUME_ID" "$CLONE_IMAGE_NAME")"
    log "clone export: $clone_export"

    log "current exports"
    http_json GET /v1/exports

    if [[ "$CONNECT_NVME" -eq 1 ]]; then
        connect_clone_export
    fi

    log "rolling back base export to snapshot $SNAPSHOT_NAME"
    rollback_snapshot "$(volume_export_id "$BASE_VOLUME_ID")" "$SNAPSHOT_NAME"

    log "flattening clone export"
    flatten_export "$(volume_export_id "$CLONE_VOLUME_ID")"

    if [[ "$CONNECT_NVME" -eq 1 ]]; then
        disconnect_clone_export
    fi
    cleanup_exports
    log "nvmf snapshot/clone flow finished"
}

case "$ACTION" in
    -h|--help)
        usage
        exit 0
        ;;
    run)
        require_commands
        ensure_inputs
        run_flow
        ;;
    *)
        usage
        die "unknown action: $ACTION"
        ;;
esac

#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ACTION="${1:-up}"
if [[ $# -gt 0 ]]; then
    shift
fi

LAB_NAME=".lab-ready"
LAB_ROOT="$ROOT/$LAB_NAME"
RUN_DIR=""
LOG_DIR=""
STATE_DIR=""
CONFIG_PATH=""

MONITOR_PORT=3333
EXPORTER_PORT=9500
TARGET_SERVICE_ID=4420
DRIVER_NAME="csi.fastblock.io"
NODE_ID="node-a"
POOL_NAME="fb"
PG_COUNT=8
PG_SIZE=1
RDMA_NAME="rdmanic"
SMOKE_TRANSPORT="rdma"
HUGEPAGES=1024
NVMF_MEM_MB=2048
AIO_FILE_SIZE="100G"

HOST_IP="${HOST_IP:-}"
NETDEV="${NETDEV:-}"

BUILD=1
RUN_SMOKE=1
FRESH=1

MONITOR_BIN="$ROOT/monitor/fastblock-mon"
CLIENT_BIN="$ROOT/monitor/fastblock-client"
OSD_BIN="$ROOT/build/src/osd/fastblock-osd"
NVMF_BIN="$ROOT/build/src/bdev/fastblock-nvmf-tgt"
EXPORTER_BIN="$ROOT/exporter/bin/fastblock-exporter"
CONTROLLER_BIN="$ROOT/csi/bin/fastblock-csi-controller"
NODE_BIN="$ROOT/csi/bin/fastblock-csi-node"
SMOKE_BIN="$ROOT/csi/bin/fastblock-csi-smoke"
RDMA_SCRIPT="$ROOT/scripts/create-rdma-rxe.sh"

usage() {
    cat <<'EOF'
Usage:
  scripts/csi-lab.sh [up|down|status] [options]

Actions:
  up       Build binaries, prepare host, deploy the single-OSD CSI lab, and run smoke validation.
  down     Stop lab daemons and clean sockets/runtime state.
  status   Show process, pool, transport, and endpoint status.

Options:
  --lab-root <path>         Override lab state root (default: .lab-ready under repo root)
  --host-ip <ip>            Monitor/exporter/target address (auto-detect by default)
  --netdev <name>           Linux netdev used to create the RXE RDMA NIC
  --pool <name>             Pool name to ensure (default: fb)
  --pg-count <n>            Pool PG count (default: 8)
  --pg-size <n>             Pool PG size (default: 1)
  --node-id <id>            CSI node id (default: node-a)
  --rdma-name <name>        RXE RDMA device name (default: rdmanic)
  --hugepages <n>           Huge pages to reserve for RDMA target (default: 1024)
  --nvmf-mem-mb <n>         SPDK target memory size in MB (default: 2048)
  --smoke-transport <name>  Smoke transport: rdma, tcp, or none (default: rdma)
  --aio-file-size <size>    Sparse file size for single-OSD aio backend (default: 100G)
  --skip-build              Reuse existing binaries instead of rebuilding
  --skip-smoke              Start environment without running smoke validation
  --reuse-state             Keep lab root and stored OSD identity instead of resetting from zero
  -h, --help                Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --lab-root)
            LAB_ROOT="$2"
            shift 2
            ;;
        --host-ip)
            HOST_IP="$2"
            shift 2
            ;;
        --netdev)
            NETDEV="$2"
            shift 2
            ;;
        --pool)
            POOL_NAME="$2"
            shift 2
            ;;
        --pg-count)
            PG_COUNT="$2"
            shift 2
            ;;
        --pg-size)
            PG_SIZE="$2"
            shift 2
            ;;
        --node-id)
            NODE_ID="$2"
            shift 2
            ;;
        --rdma-name)
            RDMA_NAME="$2"
            shift 2
            ;;
        --hugepages)
            HUGEPAGES="$2"
            shift 2
            ;;
        --nvmf-mem-mb)
            NVMF_MEM_MB="$2"
            shift 2
            ;;
        --smoke-transport)
            SMOKE_TRANSPORT="$2"
            shift 2
            ;;
        --aio-file-size)
            AIO_FILE_SIZE="$2"
            shift 2
            ;;
        --skip-build)
            BUILD=0
            shift
            ;;
        --skip-smoke)
            RUN_SMOKE=0
            shift
            ;;
        --reuse-state)
            FRESH=0
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

RUN_DIR="$LAB_ROOT/run"
LOG_DIR="$LAB_ROOT/var/log/fastblock"
STATE_DIR="$LAB_ROOT/var/lib/fastblock"
CONFIG_PATH="$LAB_ROOT/etc/fastblock/fastblock.json"

log() {
    printf '[csi-lab] %s\n' "$*"
}

die() {
    printf '[csi-lab] ERROR: %s\n' "$*" >&2
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
    for cmd in awk bash curl grep ip modprobe nohup python3 rdma sed ss uuidgen; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            missing+=("$cmd")
        fi
    done
    if [[ ${#missing[@]} -ne 0 ]]; then
        die "missing required commands: ${missing[*]}"
    fi
}

ensure_host_ip() {
    if [[ -n "$HOST_IP" ]]; then
        return
    fi
    HOST_IP="$(ip route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i <= NF; i++) if ($i == "src") {print $(i+1); exit}}')"
    if [[ -z "$HOST_IP" ]]; then
        HOST_IP="$(ip -o -4 addr show up scope global | awk '{split($4, a, "/"); print a[1]; exit}')"
    fi
    [[ -n "$HOST_IP" ]] || die "failed to detect host IPv4 address"
}

ensure_netdev() {
    if [[ -n "$NETDEV" ]]; then
        ip link show "$NETDEV" >/dev/null 2>&1 || die "netdev does not exist: $NETDEV"
        return
    fi
    NETDEV="$(ip route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i <= NF; i++) if ($i == "dev") {print $(i+1); exit}}')"
    if [[ -z "$NETDEV" ]]; then
        NETDEV="$(ip -o link show up | awk -F': ' '$2 != "lo" {print $2; exit}' | cut -d@ -f1)"
    fi
    [[ -n "$NETDEV" ]] || die "failed to detect usable netdev"
}

monitor_client() {
    "$CLIENT_BIN" -conf="$CONFIG_PATH" "$@"
}

new_uuid() {
    uuidgen | tr 'A-Z' 'a-z'
}

wait_for_pid() {
    local pid="$1"
    local name="$2"
    local attempts="${3:-30}"
    local i
    for ((i = 0; i < attempts; i++)); do
        if kill -0 "$pid" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    die "$name failed to stay alive"
}

wait_for_tcp() {
    local host="$1"
    local port="$2"
    local name="$3"
    local attempts="${4:-60}"
    local i
    for ((i = 0; i < attempts; i++)); do
        if python3 - "$host" "$port" <<'PY' >/dev/null 2>&1
import socket, sys
host = sys.argv[1]
port = int(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(1)
try:
    s.connect((host, port))
except OSError:
    sys.exit(1)
finally:
    s.close()
PY
        then
            return 0
        fi
        sleep 1
    done
    die "timed out waiting for $name on $host:$port"
}

wait_for_path() {
    local path="$1"
    local name="$2"
    local attempts="${3:-60}"
    local i
    for ((i = 0; i < attempts; i++)); do
        if [[ -e "$path" ]]; then
            return 0
        fi
        sleep 1
    done
    die "timed out waiting for $name at $path"
}

spawn() {
    local name="$1"
    shift
    local pidfile="$RUN_DIR/$name.pid"
    local logfile="$LOG_DIR/$name.log"
    mkdir -p "$RUN_DIR" "$LOG_DIR"
    : >"$logfile"
    (
        cd "$ROOT"
        nohup "$@" >>"$logfile" 2>&1 </dev/null &
        echo $! >"$pidfile"
    )
    local pid
    pid="$(cat "$pidfile")"
    wait_for_pid "$pid" "$name"
    log "started $name (pid=$pid)"
}

stop_pidfile() {
    local pidfile="$1"
    local name="$2"
    if [[ ! -f "$pidfile" ]]; then
        return
    fi
    local pid
    pid="$(cat "$pidfile" 2>/dev/null || true)"
    rm -f "$pidfile"
    if [[ -z "$pid" ]]; then
        return
    fi
    if kill -0 "$pid" >/dev/null 2>&1; then
        kill "$pid" >/dev/null 2>&1 || true
        local i
        for ((i = 0; i < 10; i++)); do
            if ! kill -0 "$pid" >/dev/null 2>&1; then
                break
            fi
            sleep 1
        done
        if kill -0 "$pid" >/dev/null 2>&1; then
            kill -9 "$pid" >/dev/null 2>&1 || true
        fi
        log "stopped $name"
    fi
}

kill_repo_processes() {
    local pattern
    for pattern in \
        "$NODE_BIN" \
        "$CONTROLLER_BIN" \
        "$EXPORTER_BIN" \
        "$NVMF_BIN" \
        "$OSD_BIN" \
        "$MONITOR_BIN" \
        "fastblock-csi-node" \
        "fastblock-csi-controller" \
        "fastblock-exporter" \
        "fastblock-nvmf-tgt" \
        "fastblock-osd" \
        "fastblock-mon"; do
        pgrep -f "$pattern" | while read -r pid; do
            [[ -n "$pid" ]] || continue
            kill "$pid" >/dev/null 2>&1 || true
        done
    done
    sleep 1
    for pattern in \
        "$NODE_BIN" \
        "$CONTROLLER_BIN" \
        "$EXPORTER_BIN" \
        "$NVMF_BIN" \
        "$OSD_BIN" \
        "$MONITOR_BIN" \
        "fastblock-csi-node" \
        "fastblock-csi-controller" \
        "fastblock-exporter" \
        "fastblock-nvmf-tgt" \
        "fastblock-osd" \
        "fastblock-mon"; do
        pgrep -f "$pattern" | while read -r pid; do
            [[ -n "$pid" ]] || continue
            kill -9 "$pid" >/dev/null 2>&1 || true
        done
    done
}

write_config() {
    mkdir -p "$(dirname "$CONFIG_PATH")" "$STATE_DIR" "$LOG_DIR"
    cat >"$CONFIG_PATH" <<EOF
{
  "msg_rdma_cq_num_entries": 1024,
  "msg_server_metadata_memory_pool_capacity": 256,
  "msg_server_data_memory_pool_capacity": 2048,
  "msg_client_metadata_memory_pool_capacity": 256,
  "msg_client_data_memory_pool_capacity": 2048,
  "msg_server_metadata_memory_pool_element_size": 512,
  "msg_server_data_memory_pool_element_size": 5120,
  "msg_client_metadata_memory_pool_element_size": 512,
  "msg_client_data_memory_pool_element_size": 5120,
  "msg_client_per_post_recv_num": 64,
  "msg_server_per_post_recv_num": 64,
  "msg_client_rpc_timeout_us": 600000000,
  "msg_server_rpc_timeout_us": 600000000,
  "monitors": [
    "$HOST_IP"
  ],
  "mon_host": [
    "$HOST_IP"
  ],
  "mon_rpc_address": "$HOST_IP",
  "mon_rpc_port": $MONITOR_PORT,
  "data_dir": "$STATE_DIR/mon_$HOST_IP",
  "osd_data_dir": "$STATE_DIR",
  "osd_no_huge": true,
  "msg_server_bind_address": "$HOST_IP",
  "osd_mem_size_mb": 1024,
  "osd_iobuf_small_pool_count": 1024,
  "osd_iobuf_large_pool_count": 64,
  "log_path": "$LOG_DIR/monitor.log",
  "rdma_device_name": "$RDMA_NAME"
}
EOF
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
    printf 'nqn.2014-08.org.nvmexpress:uuid:%s\n' "$(new_uuid)" > /etc/nvme/hostnqn
}

ensure_modules() {
    modprobe nvme-fabrics
    modprobe nvme-tcp
    modprobe nvme-rdma
}

ensure_hugepages() {
    local current
    current="$(cat /proc/sys/vm/nr_hugepages)"
    if [[ "$current" -lt "$HUGEPAGES" ]]; then
        echo "$HUGEPAGES" > /proc/sys/vm/nr_hugepages
    fi
    current="$(cat /proc/sys/vm/nr_hugepages)"
    [[ "$current" -ge "$HUGEPAGES" ]] || die "failed to reserve hugepages, current=$current expected>=$HUGEPAGES"
}

ensure_rdma() {
    "$RDMA_SCRIPT" -d "$NETDEV" -n "$RDMA_NAME" >/dev/null
}

build_binaries() {
    log "building monitor binaries"
    "$ROOT/build.sh" -c monitor
    log "building osd/nvmf binaries"
    "$ROOT/build.sh" -c osd
    log "building exporter binary"
    (
        cd "$ROOT/exporter"
        go build -o bin/fastblock-exporter ./cmd/fastblock-exporter
    )
    log "building csi binaries"
    (
        cd "$ROOT/csi"
        go build -o bin/fastblock-csi-controller ./cmd/controller
        go build -o bin/fastblock-csi-node ./cmd/node
        go build -o bin/fastblock-csi-smoke ./cmd/smoke
    )
}

ensure_binaries_exist() {
    local bin
    for bin in \
        "$MONITOR_BIN" \
        "$CLIENT_BIN" \
        "$OSD_BIN" \
        "$NVMF_BIN" \
        "$EXPORTER_BIN" \
        "$CONTROLLER_BIN" \
        "$NODE_BIN" \
        "$SMOKE_BIN"; do
        [[ -x "$bin" ]] || die "missing binary: $bin"
    done
}

spdk_rpc_sock() {
    cat "$RUN_DIR/spdk_rpc.sock"
}

spdk_rpc() {
    local method="$1"
    local params_json="${2:-null}"
    python3 - "$(spdk_rpc_sock)" "$method" "$params_json" <<'PY'
import json
import socket
import sys

sock_path, method, params_json = sys.argv[1:4]
req = {"jsonrpc": "2.0", "id": 1, "method": method}
if params_json and params_json != "null":
    req["params"] = json.loads(params_json)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(15)
s.connect(sock_path)
s.sendall((json.dumps(req) + "\n").encode())
data = b""
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    data += chunk
    if b"\n" in chunk:
        break
s.close()

resp = json.loads(data.decode())
if resp.get("error"):
    err = resp["error"]
    print(f"spdk rpc error {err.get('code')}: {err.get('message')}", file=sys.stderr)
    sys.exit(1)
print(json.dumps(resp.get("result")))
PY
}

ensure_transport() {
    local trtype="$1"
    local params_json="$2"
    local exists
    exists="$(spdk_rpc nvmf_get_transports '{}')"
    if python3 - "$trtype" "$exists" <<'PY'
import json
import sys

want = sys.argv[1].upper()
items = json.loads(sys.argv[2])
for item in items:
    if item.get("trtype", "").upper() == want:
        sys.exit(0)
sys.exit(1)
PY
    then
        log "$trtype transport already exists"
        return
    fi
    spdk_rpc nvmf_create_transport "$params_json" >/dev/null
    log "created $trtype transport"
}

wait_for_spdk_sock() {
    local sock=""
    local i
    for ((i = 0; i < 60; i++)); do
        sock="$(ls -t /var/tmp/fastblock_nvmf_tgt*.sock 2>/dev/null | head -n1 || true)"
        if [[ -n "$sock" && -S "$sock" ]]; then
            printf '%s\n' "$sock" > "$RUN_DIR/spdk_rpc.sock"
            return 0
        fi
        sleep 1
    done
    die "timed out waiting for SPDK RPC socket"
}

ensure_osd_identity() {
    local uuid_file="$RUN_DIR/osd.uuid"
    local id_file="$RUN_DIR/osd.id"
    if [[ -f "$uuid_file" && -f "$id_file" ]]; then
        return
    fi
    mkdir -p "$RUN_DIR"
    local osd_uuid osd_id
    osd_uuid="$(new_uuid)"
    osd_id="$(monitor_client -op=fakeapplyid -uuid "$osd_uuid" | tr -cd '0-9\n' | head -n1)"
    [[ -n "$osd_id" ]] || die "failed to allocate OSD id"
    printf '%s\n' "$osd_uuid" > "$uuid_file"
    printf '%s\n' "$osd_id" > "$id_file"
}

osd_id() {
    cat "$RUN_DIR/osd.id"
}

osd_uuid() {
    cat "$RUN_DIR/osd.uuid"
}

prepare_osd_layout() {
    local id
    id="$(osd_id)"
    local osd_dir="$STATE_DIR/osd-$id"
    local aio_file="$osd_dir/file"
    local disk_file="$osd_dir/disk"
    local bdev_type_file="$osd_dir/bdev_type"
    mkdir -p "$osd_dir"
    printf 'aio\n' > "$bdev_type_file"
    if [[ ! -f "$aio_file" ]]; then
        truncate -s "$AIO_FILE_SIZE" "$aio_file"
    fi
    printf '%s\n0 %s\n' "$aio_file" "$(stat -Lc %s "$aio_file")" > "$disk_file"
}

mkfs_osd_if_needed() {
    local id
    id="$(osd_id)"
    if [[ -e "$STATE_DIR/osd-$id/core_size" ]]; then
        return
    fi
    prepare_osd_layout
    log "creating OSD data for osd.$id"
    "$OSD_BIN" -C "$CONFIG_PATH" --id "$id" --mkfs --force --uuid "$(osd_uuid)" -S 1 >/dev/null
}

ensure_pool() {
    local pools
    pools="$(monitor_client -op=listpools 2>&1 || true)"
    if grep -q "name: $POOL_NAME" <<<"$pools"; then
        log "pool $POOL_NAME already exists"
    else
        monitor_client -op=createpool -poolname="$POOL_NAME" -pgcount="$PG_COUNT" -pgsize="$PG_SIZE" >/dev/null
        log "created pool $POOL_NAME"
    fi
    wait_for_pool_ready
}

wait_for_pool_ready() {
    local i pools status
    for ((i = 0; i < 60; i++)); do
        pools="$(monitor_client -op=listpools 2>&1 || true)"
        status="$(monitor_client -op=status 2>&1 || true)"
        if grep -q "name: $POOL_NAME" <<<"$pools" && grep -q "pools  : 1 pools" <<<"$status" && grep -q "active" <<<"$status"; then
            log "pool $POOL_NAME is active"
            return 0
        fi
        sleep 1
    done
    die "pool $POOL_NAME did not become active"
}

wait_for_osd_up() {
    local id="$1"
    local i
    for ((i = 0; i < 60; i++)); do
        if monitor_client -op=status 2>/dev/null | grep -q "1 osds: 1 up"; then
            monitor_client -op=inosd -osdid="$id" >/dev/null || true
            if monitor_client -op=status 2>/dev/null | grep -q "1 osds: 1 up, 1 in"; then
                return 0
            fi
        fi
        sleep 1
    done
    die "OSD did not reach up/in state"
}

start_monitor() {
    spawn monitor "$MONITOR_BIN" -conf="$CONFIG_PATH" -id="$HOST_IP"
    wait_for_tcp "$HOST_IP" "$MONITOR_PORT" "monitor"
}

start_osd() {
    local id
    id="$(osd_id)"
    spawn osd "$OSD_BIN" -C "$CONFIG_PATH" --id "$id" -N 0
    wait_for_osd_up "$id"
}

start_nvmf() {
    rm -f /var/tmp/fastblock_nvmf_tgt*.sock
    spawn nvmf_tgt "$NVMF_BIN" -s "$NVMF_MEM_MB" -C "$CONFIG_PATH" -S 1
    wait_for_spdk_sock
    ensure_transport RDMA '{"trtype":"RDMA","max_queue_depth":128,"max_io_qpairs_per_ctrlr":8,"max_io_size":131072,"in_capsule_data_size":8192}'
    ensure_transport TCP '{"trtype":"TCP","max_io_qpairs_per_ctrlr":8,"max_io_size":131072,"in_capsule_data_size":8192}'
}

start_exporter() {
    spawn exporter \
        "$EXPORTER_BIN" \
        -listen ":$EXPORTER_PORT" \
        -monitor-address "$HOST_IP:$MONITOR_PORT" \
        -node-name "$NODE_ID" \
        -spdk-rpc-sock "$(spdk_rpc_sock)" \
        -target-address "$HOST_IP" \
        -target-service-id "$TARGET_SERVICE_ID" \
        -nqn-prefix "nqn.2026-04.io.fastblock"
    local i
    for ((i = 0; i < 30; i++)); do
        if curl -fsS "http://127.0.0.1:$EXPORTER_PORT/healthz" >/dev/null 2>&1; then
            return
        fi
        sleep 1
    done
    die "exporter health check failed"
}

start_controller() {
    rm -f /tmp/fastblock-csi-controller.sock
    spawn controller \
        "$CONTROLLER_BIN" \
        -endpoint "unix:///tmp/fastblock-csi-controller.sock" \
        -driver-name "$DRIVER_NAME" \
        -monitor-address "$HOST_IP:$MONITOR_PORT" \
        -exporter-endpoint "http://127.0.0.1:$EXPORTER_PORT"
    wait_for_path /tmp/fastblock-csi-controller.sock "controller socket"
}

start_node() {
    rm -f /tmp/fastblock-csi-node.sock
    spawn node \
        "$NODE_BIN" \
        -endpoint "unix:///tmp/fastblock-csi-node.sock" \
        -driver-name "$DRIVER_NAME" \
        -node-id "$NODE_ID"
    wait_for_path /tmp/fastblock-csi-node.sock "node socket"
}

run_smoke() {
    if [[ "$RUN_SMOKE" -eq 0 || "$SMOKE_TRANSPORT" == "none" ]]; then
        return
    fi
    ensure_hostnqn
    local attempt volume_name output rc
    for attempt in 1 2 3 4 5; do
        volume_name="bootstrap-$(date +%s)-$attempt"
        log "running $SMOKE_TRANSPORT smoke validation with volume $volume_name (attempt $attempt/5)"
        set +e
        output="$(
            "$SMOKE_BIN" \
                -controller-endpoint "unix:///tmp/fastblock-csi-controller.sock" \
                -node-endpoint "unix:///tmp/fastblock-csi-node.sock" \
                -node-id "$NODE_ID" \
                -host-nqn "$(cat /etc/nvme/hostnqn)" \
                -volume-name "$volume_name" \
                -pool "$POOL_NAME" \
                -transport "$SMOKE_TRANSPORT" \
                -size-bytes 16777216 \
                -object-size 4194304 \
                -block-size 4096 2>&1
        )"
        rc=$?
        set -e
        printf '%s\n' "$output"
        if [[ "$rc" -eq 0 ]]; then
            return
        fi
        sleep 3
    done
    die "$SMOKE_TRANSPORT smoke validation did not succeed after retries"
}

show_status() {
    local sock=""
    log "lab root: $LAB_ROOT"
    log "host ip: ${HOST_IP:-unknown}"
    log "netdev: ${NETDEV:-unknown}"
    ps -ef | grep -E "fastblock-(mon|osd|nvmf-tgt|exporter|csi-controller|csi-node)" | grep -v grep || true
    if [[ -f "$CONFIG_PATH" && -x "$CLIENT_BIN" ]]; then
        monitor_client -op=status || true
        monitor_client -op=listpools || true
    fi
    if [[ -f "$RUN_DIR/spdk_rpc.sock" ]]; then
        sock="$(spdk_rpc_sock)"
        log "SPDK RPC socket: $sock"
        spdk_rpc nvmf_get_transports '{}' || true
    fi
    curl -fsS "http://127.0.0.1:$EXPORTER_PORT/healthz" || true
    printf '\n'
    log "controller socket: /tmp/fastblock-csi-controller.sock"
    log "node socket: /tmp/fastblock-csi-node.sock"
    log "sample RDMA smoke:"
    printf '  %s -controller-endpoint unix:///tmp/fastblock-csi-controller.sock -node-endpoint unix:///tmp/fastblock-csi-node.sock -node-id %s -host-nqn "$(cat /etc/nvme/hostnqn)" -volume-name smoke-rdma -pool %s -transport rdma -size-bytes 16777216 -object-size 4194304 -block-size 4096\n' "$SMOKE_BIN" "$NODE_ID" "$POOL_NAME"
}

down() {
    stop_pidfile "$RUN_DIR/node.pid" node
    stop_pidfile "$RUN_DIR/controller.pid" controller
    stop_pidfile "$RUN_DIR/exporter.pid" exporter
    stop_pidfile "$RUN_DIR/nvmf_tgt.pid" nvmf_tgt
    stop_pidfile "$RUN_DIR/osd.pid" osd
    stop_pidfile "$RUN_DIR/monitor.pid" monitor
    kill_repo_processes
    rm -f /tmp/fastblock-csi-controller.sock /tmp/fastblock-csi-node.sock
    rm -f /var/tmp/fastblock_nvmf_tgt*.sock
    rm -f "$RUN_DIR/spdk_rpc.sock"
}

up() {
    require_root
    require_commands
    ensure_host_ip
    ensure_netdev
    if [[ "$FRESH" -eq 1 ]]; then
        down || true
        rm -rf "$LAB_ROOT"
    fi
    mkdir -p "$RUN_DIR" "$LOG_DIR" "$STATE_DIR"
    write_config
    ensure_modules
    ensure_rdma
    ensure_hugepages
    ensure_hostnqn
    if [[ "$BUILD" -eq 1 ]]; then
        build_binaries
    fi
    ensure_binaries_exist
    start_monitor
    ensure_osd_identity
    mkfs_osd_if_needed
    start_osd
    ensure_pool
    start_nvmf
    start_exporter
    start_controller
    start_node
    run_smoke
    show_status
}

case "$ACTION" in
    up)
        up
        ;;
    down)
        require_root
        down
        ;;
    status)
        ensure_host_ip
        ensure_netdev
        show_status
        ;;
    *)
        usage
        die "unknown action: $ACTION"
        ;;
esac

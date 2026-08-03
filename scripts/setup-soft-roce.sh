#!/usr/bin/env bash
# scripts：Soft-RoCE (rdma_rxe) 本地 e2e 准备脚本
# 依赖 create-rdma-rxe.sh；额外检查内核模块、设备与基本 verbs 可用性。
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

netdev=""
rdma_name="rdmanic"
skip_create=0

usage() {
    cat <<EOF
Usage: $0 [-d|--netdev <netdev>] [-n|--name <rdma_name>] [--skip-create]
  Prepare Soft-RoCE (rxe) for raw-over-RDMA OSD / kfastblock e2e.

  -d, --netdev     Netdev to attach rxe to (default: first non-lo up device)
  -n, --name       RDMA device name (default: rdmanic)
  --skip-create    Only verify; do not create rxe link
EOF
    exit 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -d|--netdev) shift; netdev="${1:-}" ;;
        -n|--name) shift; rdma_name="${1:-}" ;;
        --skip-create) skip_create=1 ;;
        -h|--help) usage ;;
        *) echo "Unknown argument: $1"; usage ;;
    esac
    shift
done

if [ "$(id -u)" -ne 0 ]; then
    echo "scripts：需要 root 运行 Soft-RoCE 准备（modprobe / rdma link）"
    exit 1
fi

for cmd in rdma ip modprobe; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "scripts：缺少命令 $cmd（安装 iproute2 / rdma-core）"
        exit 1
    fi
done

echo "scripts：加载 rdma_rxe 模块"
modprobe rdma_rxe || {
    echo "scripts：modprobe rdma_rxe 失败（内核需 CONFIG_RDMA_RXE）"
    exit 1
}

if [ "$skip_create" -eq 0 ]; then
    create_args=()
    [ -n "$netdev" ] && create_args+=(-d "$netdev")
    create_args+=(-n "$rdma_name")
    echo "scripts：创建/复用 rxe 设备 ${rdma_name}"
    "$repo_root/scripts/create-rdma-rxe.sh" "${create_args[@]}"
fi

echo "scripts：当前 RDMA 链路"
rdma link show || true

if ! rdma link show | grep -qE "link ${rdma_name}/|netdev"; then
    echo "scripts：警告：未看到期望的 rxe 设备名 ${rdma_name}"
fi

if command -v ibv_devices >/dev/null 2>&1; then
    echo "scripts：ibv_devices"
    ibv_devices || true
else
    echo "scripts：无 ibv_devices（可选，来自 libibverbs-utils）"
fi

echo "scripts：Soft-RoCE 准备完成。后续："
echo "  1) 启动 monitor / OSD（enable_raw_rdma=true）"
echo "  2) 确认 OSD 日志 raw RDMA server started ... ports=[...]"
echo "  3) kfastblock 挂载并偏好 RDMA（见 docs/raw_rdma_e2e_checklist.md）"
echo "  4) 观察 accept/reject/dispatch_err 累计（OSD stop 日志）"
exit 0

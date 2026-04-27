#!/bin/bash
set -euo pipefail

rdma_name="rdmanic"
netdev=""

usage() {
    echo "Usage: $0 [-d|--netdev <netdev>] [-n|--name <rdma_name>]"
    echo "  -d, --netdev   Linux netdev used to create the RXE RDMA NIC"
    echo "  -n, --name     RDMA device name to create, default: rdmanic"
    exit 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -d|--netdev)
            shift
            netdev="${1:-}"
            ;;
        -n|--name)
            shift
            rdma_name="${1:-}"
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown argument: $1"
            usage
            ;;
    esac
    shift
done

if [ "$(id -u)" -ne 0 ]; then
    echo "This script must run as root. Example:"
    echo "  sudo $0 ${netdev:+-d $netdev }${rdma_name:+-n $rdma_name}"
    exit 1
fi

for cmd in rdma ip modprobe; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Missing required command: $cmd"
        exit 1
    fi
done

if [ -z "$netdev" ]; then
    netdev=$(ip -o link show up | awk -F': ' '$2 != "lo" {print $2; exit}' | cut -d@ -f1)
fi

if [ -z "$netdev" ]; then
    echo "Failed to detect a usable netdev"
    exit 1
fi

if ! ip link show "$netdev" >/dev/null 2>&1; then
    echo "Netdev does not exist: $netdev"
    exit 1
fi

modprobe rdma_rxe

existing_line=$(rdma link show | awk -v name="$rdma_name" '$1 == "link" {split($2, a, "/"); if (a[1] == name) print $0}')
if [ -n "$existing_line" ]; then
    echo "RDMA device already exists: $existing_line"
    exit 0
fi

rdma link add "$rdma_name" type rxe netdev "$netdev"
echo "Created RDMA device:"
rdma link show | awk -v name="$rdma_name" '$1 == "link" {split($2, a, "/"); if (a[1] == name) print $0}'

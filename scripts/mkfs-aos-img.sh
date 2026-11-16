#!/usr/bin/env bash
# Create a zero-filled AOS backing file for one OSD (dev / manual deploy).
# Production mkfs of superblock+manifest is still: fastblock-osd --mkfs
set -euo pipefail

usage() {
  echo "Usage: $0 <path> [size_mib]"
  echo "  path      AOS image path (e.g. /var/lib/fastblock/osd-1/aos.img)"
  echo "  size_mib  size in MiB (default 2048)"
  exit 1
}

[[ $# -ge 1 ]] || usage
PATH_IMG="$1"
SIZE_MIB="${2:-2048}"

mkdir -p "$(dirname "$PATH_IMG")"
if [[ -e "$PATH_IMG" ]]; then
  echo "exists: $PATH_IMG (leave unchanged)"
  ls -lh "$PATH_IMG"
  exit 0
fi

dd if=/dev/zero of="$PATH_IMG" bs=1M count="$SIZE_MIB" status=progress
echo "created $PATH_IMG (${SIZE_MIB} MiB)"
echo "Next: configure aos_disk and run: fastblock-osd -C <conf> --mkfs --id <id> --uuid <uuid>"

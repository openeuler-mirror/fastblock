#!/bin/bash
set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
config_file="${1:-$script_root/.vstart/etc/fastblock/block_bench.dev.json}"
cpu_mask="${BLOCK_BENCH_CPU_MASK:-0x4}"
image_name="${BLOCK_BENCH_IMAGE_NAME:-dev-verify-$(date +%s)}"
bench_bin="$script_root/build/src/tools/block_bench/block_bench"

if [ ! -f "$config_file" ]; then
    echo "config file not found: $config_file" >&2
    exit 1
fi

if [ ! -x "$bench_bin" ]; then
    echo "block_bench binary not found: $bench_bin" >&2
    exit 1
fi

write_cfg="$(mktemp)"
read_cfg="$(mktemp)"
cleanup() {
    rm -f "$write_cfg" "$read_cfg"
}
trap cleanup EXIT

jq --arg image "$image_name" \
   '.image_name = $image | .io_type = "write" | .sequential_offsets = true | .deferred_time = 0' \
   "$config_file" > "$write_cfg"

jq --arg image "$image_name" \
   '.image_name = $image | .io_type = "read" | .verify_read_data = true | .sequential_offsets = true | .deferred_time = 0' \
   "$config_file" > "$read_cfg"

echo "Running write benchmark for image: $image_name"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
    "$bench_bin" -m "$cpu_mask" -C "$write_cfg"

echo "Running read-verify benchmark for image: $image_name"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
    "$bench_bin" -m "$cpu_mask" -C "$read_cfg"

echo "Write-read-verify completed for image: $image_name"

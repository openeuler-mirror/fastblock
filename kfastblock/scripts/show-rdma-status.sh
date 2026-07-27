#!/usr/bin/env bash
# Dump kfastblock RDMA-related module params and per-volume transport preference.
set -euo pipefail

PARAM_ROOT=/sys/module/kfastblock/parameters
DEV_ROOT=/sys/bus/kfastblock/devices
PREFIX=kfastblock-vol-

if [[ ! -d "$PARAM_ROOT" ]]; then
	echo "kfastblock module not loaded (missing $PARAM_ROOT)" >&2
	exit 1
fi

echo "=== module parameters (rdma/xport) ==="
shopt -s nullglob
for f in "$PARAM_ROOT"/rdma_* "$PARAM_ROOT"/xport_*; do
	name=$(basename "$f")
	# shellcheck disable=SC2002
	val=$(tr -d '\n' <"$f" || true)
	printf '%s=%s\n' "$name" "$val"
done

if [[ -d "$DEV_ROOT" ]]; then
	echo "=== volumes osd_transport / rdma_cache_stats ==="
	for d in "$DEV_ROOT"/"$PREFIX"*; do
		[[ -d "$d" ]] || continue
		base=$(basename "$d")
		pool=$(tr -d '\n' <"$d/pool_name" 2>/dev/null || echo "?")
		image=$(tr -d '\n' <"$d/image_name" 2>/dev/null || echo "?")
		xport=$(tr -d '\n' <"$d/osd_transport" 2>/dev/null || echo "n/a")
		cache=$(tr -d '\n' <"$d/rdma_cache_stats" 2>/dev/null || echo "n/a")
		printf '%s pool=%s image=%s osd_transport=%s rdma_cache_stats=%s\n' \
			"$base" "$pool" "$image" "$xport" "$cache"
	done
fi

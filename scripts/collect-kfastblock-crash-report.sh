#!/bin/bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact_root="${KFASTBLOCK_TEST_ARTIFACT_ROOT:-$repo_root/.artifacts/kfastblock-tests}"
run_dir="$artifact_root/crash-report-$(date +%Y%m%d-%H%M%S)"
config_file="$repo_root/.vstart/etc/fastblock/fastblock.json"
log_dir="$repo_root/.vstart/var/log/fastblock"

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root" >&2
    exit 1
fi

mkdir -p "$run_dir"

date > "$run_dir/date.txt"
uname -a > "$run_dir/uname.txt"
uptime > "$run_dir/uptime.txt"
last -x > "$run_dir/last-x.txt" 2>&1 || true
journalctl --list-boots > "$run_dir/journal-boots.txt" 2>&1 || true
journalctl -k -b > "$run_dir/journal-kernel-current.txt" 2>&1 || true
journalctl -k -b -1 > "$run_dir/journal-kernel-prev.txt" 2>&1 || true
dmesg > "$run_dir/dmesg.txt" 2>&1 || true
lsmod > "$run_dir/lsmod.txt" 2>&1 || true

if [ -f "$config_file" ]; then
    cp "$config_file" "$run_dir/fastblock.json"
fi

if ls /sys/module/kfastblock/parameters >/dev/null 2>&1; then
    for param_path in /sys/module/kfastblock/parameters/*; do
        [ -e "$param_path" ] || continue
        printf '%s=%s\n' "$(basename "$param_path")" "$(cat "$param_path")"
    done > "$run_dir/kfastblock-module-params.txt"
fi

if [ -d /sys/fs/pstore ]; then
    mkdir -p "$run_dir/pstore"
    cp -a /sys/fs/pstore/. "$run_dir/pstore/" 2>/dev/null || true
fi

if [ -d "$log_dir" ]; then
    mkdir -p "$run_dir/fastblock-logs"
    for log_file in monitor.log osd1.log osd2.log osd3.log; do
        if [ -f "$log_dir/$log_file" ]; then
            cp "$log_dir/$log_file" "$run_dir/fastblock-logs/$log_file"
            tail -n 200 "$log_dir/$log_file" \
                > "$run_dir/fastblock-logs/$log_file.tail" || true
        fi
    done
fi

echo "crash report collected: $run_dir"

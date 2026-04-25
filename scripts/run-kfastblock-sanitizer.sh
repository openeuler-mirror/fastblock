#!/bin/bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/kfastblock-test-common.sh"

repo_root="$(kfastblock_repo_root)"
run_dir="$(kfastblock_artifact_dir "$repo_root" sanitizer)"
log_file="$run_dir/sanitizer.log"
runner="${KFASTBLOCK_SANITIZER_RUNNER:-}"
target_script="${KFASTBLOCK_SANITIZER_TARGET_SCRIPT:-$repo_root/scripts/run-kfastblock-dev-smoke.sh}"
require_runtime="${KFASTBLOCK_SANITIZER_REQUIRE_RUNTIME:-0}"
kernel_config="/boot/config-$(uname -r)"
runtime_mode="skip"

sanitizer_kernel_enabled() {
    if [ ! -r "$kernel_config" ]; then
        return 1
    fi

    grep -Eq '^CONFIG_KASAN=y|^CONFIG_KCSAN=y' "$kernel_config"
}

kfastblock_require_root
kfastblock_start_logging "$log_file"
echo "artifact_dir=$run_dir"
kfastblock_capture_context "$repo_root/.vstart/etc/fastblock/fastblock.json" "$run_dir"

make -C "$repo_root/kfastblock" clean all >/dev/null
echo "module build ok"

if [ -n "$runner" ]; then
    runtime_mode="external-runner"
    echo "running external sanitizer runner"
    KFASTBLOCK_SANITIZER_ARTIFACT_DIR="$run_dir" \
        KFASTBLOCK_SANITIZER_TARGET_SCRIPT="$target_script" \
        bash -lc "$runner"
elif sanitizer_kernel_enabled; then
    runtime_mode="local-kernel"
    echo "detected sanitizer-enabled kernel, running target: $target_script"
    "$target_script"
else
    echo "sanitizer runtime skipped: no sanitizer-enabled kernel or external runner configured"
    if [ "$require_runtime" = "1" ]; then
        echo "runtime sanitizer execution required but unavailable" >&2
        exit 1
    fi
fi

echo "sanitizer entry ok: mode=$runtime_mode"

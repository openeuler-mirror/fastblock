#!/usr/bin/env bash
# Helper: build a target, then commit if dirty. Used by autonomous RDMA work.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

target="${1:-osd}" # osd | kfastblock
msg="${2:-}"

if [ -z "$msg" ]; then
  echo "usage: $0 <osd|kfastblock> <commit message>" >&2
  exit 2
fi

if [ "$target" = "osd" ]; then
  cmake --build build --target fastblock-osd -j"$(nproc)"
elif [ "$target" = "kfastblock" ]; then
  make -C kfastblock KDIR=/root/kernel/.hostbuild modules
elif [ "$target" = "monitor" ]; then
  ./build.sh -c monitor
else
  echo "unknown target: $target" >&2
  exit 2
fi

if git diff --quiet && git diff --cached --quiet; then
  echo "no changes to commit"
  exit 0
fi

git add -A -- ':!kfastblock/*.o' ':!kfastblock/*.ko' ':!kfastblock/*.mod' \
  ':!kfastblock/*.cmd' ':!kfastblock/Module.symvers' ':!kfastblock/modules.order' \
  ':!kfastblock/kfastblock.mod.c' ':!kfastblock/tool/kfastblock-*' \
  ':!build' 2>/dev/null || true

# Stage only source files under src/ kfastblock/ monitor/ proto/
git add -u
git add src/ kfastblock/include/ kfastblock/src/ kfastblock/Makefile \
  kfastblock/tool/ monitor/ proto/ 2>/dev/null || true

if git diff --cached --quiet; then
  echo "nothing staged"
  exit 0
fi

git commit -m "$msg"
echo "COUNT=$(git rev-list --count origin/master..HEAD)"

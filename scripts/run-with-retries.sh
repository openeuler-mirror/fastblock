#!/bin/bash
# Run a command up to N times until success (for flaky smoke environments).
set -euo pipefail
N="${1:-3}"
shift
i=1
while [ "$i" -le "$N" ]; do
  echo "attempt $i/$N: $*"
  if "$@"; then
    echo "SUCCESS on attempt $i"
    exit 0
  fi
  i=$((i+1))
  sleep 1
done
echo "FAILED after $N attempts" >&2
exit 1

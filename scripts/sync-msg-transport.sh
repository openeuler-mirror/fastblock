#!/bin/bash
# Copy msg_transport from the OSD json into nvmf/vhost json files.
# vstart only writes fastblock.json; nvmf/vhost must use the same backend.
set -euo pipefail

src="${1:-/etc/fastblock/fastblock.json}"
if [ "$#" -gt 0 ]; then
    shift
fi
if [ "$#" -eq 0 ]; then
    set -- /etc/fastblock/nvmf_tgt.json /etc/fastblock/vhost.json
fi

if [ ! -f "$src" ]; then
    echo "source json not found: $src" >&2
    exit 1
fi

transport=$(jq -r '.msg_transport // empty' "$src")
if [ -z "$transport" ]; then
    echo "no msg_transport in $src" >&2
    exit 1
fi

for f in "$@"; do
    if [ ! -f "$f" ]; then
        echo "skip missing $f"
        continue
    fi
    tmp=$(mktemp)
    jq --arg t "$transport" '.msg_transport = $t' "$f" > "$tmp"
    mv "$tmp" "$f"
    echo "set msg_transport=$transport in $f"
done

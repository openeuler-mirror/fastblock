#!/bin/bash
set -euo pipefail

export GOPROXY="${GOPROXY:-https://goproxy.cn|https://goproxy.io|direct}"

./install-deps.sh

./build.sh -t Release -c monitor

./build.sh -t Release -c osd

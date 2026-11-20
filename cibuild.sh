#!/bin/bash
# Copyright (c) 2023-2024 ChinaUnicom
# fastblock is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
set -euo pipefail

if [ "${KFASTBLOCK_CI_SKIP_BUILD:-1}" != "0" ]; then
    echo "[cibuild] real build temporarily skipped (set KFASTBLOCK_CI_SKIP_BUILD=0 to enable)"
    exit 0
fi

export GOPROXY="${GOPROXY:-https://goproxy.cn|https://goproxy.io|direct}"

./install-deps.sh

./build.sh -t Release -c osd

./scripts/run-kfastblock-ci.sh

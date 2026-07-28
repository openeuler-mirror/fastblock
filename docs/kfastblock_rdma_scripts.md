# kfastblock RDMA 回归脚本一览

| 脚本 | 用途 |
|------|------|
| `scripts/run-kfastblock-rdma-4k-verify.sh` | 单次 4K RDMA 写读 |
| `scripts/run-kfastblock-rdma-multi-io.sh` | 多轮同连接 exchange |
| `scripts/run-kfastblock-rdma-parallel-4k.sh` | 多进程并发 4K |
| `scripts/run-kfastblock-rdma-idle-age-smoke.sh` | idle_max_age 重连 |
| `scripts/run-kfastblock-transport-matrix.sh` | tcp/rdma/auto 矩阵 |
| `scripts/run-kfastblock-tcp-4k-verify.sh` | TCP 对照 |
| `scripts/run-kfastblock-auto-4k-verify.sh` | auto transport |
| `scripts/run-kfastblock-rdma-seq-64k.sh` | 顺序多 4K 块 |
| `scripts/run-kfastblock-rdma-write-only.sh` | 仅写 |
| `scripts/run-kfastblock-rdma-read-only.sh` | 写后多次读 |
| `scripts/run-kfastblock-rdma-bs-sweep.sh` | 4K/8K 扫测 |
| `scripts/post-reboot-rdma-smoke.sh` | 冷启动全路径 |
| `scripts/check-kfastblock-rdma-params.sh` | 模块参数存在性 |
| `scripts/run-kfastblock-rdma-suite.sh` | 最小套件入口 |
| `scripts/run-kfastblock-rdma-ci-smoke.sh` | reload+suite |
| `scripts/run-kfastblock-rdma-suite-help.sh` | suite 步骤说明 |
| `scripts/kfastblock-reload-module.sh` | 重编并加载模块 |
| `scripts/kfastblock-detach-all.sh` | 卸掉所有卷 |
| `scripts/kfastblock-rdma-env-help.sh` | 环境变量说明 |

集群需已 `vstart`（或由脚本拉起），Soft-RoCE 见 `scripts/setup-soft-roce.sh`。

运行 `bash scripts/kfastblock-rdma-env-help.sh` 可查看常用环境变量。

可选环境：`KFASTBLOCK_SUITE_PARALLEL=1`、`KFASTBLOCK_SUITE_SEQ=1`。

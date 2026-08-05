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
| `scripts/post-reboot-rdma-smoke.sh` | 冷启动全路径 |
| `scripts/check-kfastblock-rdma-params.sh` | 模块参数存在性 |
| `scripts/run-kfastblock-rdma-suite.sh` | 最小套件入口 |
| `scripts/kfastblock-reload-module.sh` | 重编并加载模块 |
| `scripts/kfastblock-detach-all.sh` | 卸掉所有卷 |

集群需已 `vstart`（或由脚本拉起），Soft-RoCE 见 `scripts/setup-soft-roce.sh`。

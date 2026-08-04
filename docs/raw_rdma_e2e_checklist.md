# raw-over-RDMA e2e 检查清单

面向 **Soft-RoCE (rxe)** 或真实 RNIC 上的 kfastblock ↔ OSD raw RDMA 数据面联调。

## 1. 主机准备

```bash
# Soft-RoCE
sudo scripts/setup-soft-roce.sh
# 或仅创建 rxe 设备
sudo scripts/create-rdma-rxe.sh -d eth0 -n rdmanic

rdma link show
ibv_devices   # 可选
```

确认内核模块 `rdma_rxe`（或厂商驱动）已加载，且 `rdma link` 可见设备。

## 2. 集群与配置

1. 启动 monitor（需支持 `ShardCore.raw_rdma_port` 持久化与下发）。
2. OSD 配置 JSON 保持默认或显式：

```json
"enable_raw_rdma": true
```

3. 启动 OSD 后在日志中确认类似：

```
raw RDMA server started on <ip> shards=N ports=[p0,p1,...]
raw RDMA active running=1 ... accept=... ports=[...]
```

4. boot 后 monitor `GetOsdMap` / raw cluster map 中各 shard 的 `RdmaPort` / `raw_rdma_port` 非 0。

## 3. 客户端

1. 加载 kfastblock，偏好 RDMA transport（见 kfastblock admin / xport 参数）。
2. 挂载 volume，观察 probe/conn pool 是否出现 RDMA ready。
3. 执行写读校验（推荐）：

```bash
# 复用已运行集群的快速 4K RDMA 校验
sudo bash scripts/run-kfastblock-rdma-4k-verify.sh
# 多轮同连接 exchange（防 recv_done 回归）
sudo KFASTBLOCK_RDMA_IO_ROUNDS=8 bash scripts/run-kfastblock-rdma-multi-io.sh
# 并行 4K（多 worker）
sudo bash scripts/run-kfastblock-rdma-parallel-4k.sh
# tcp/rdma/auto 矩阵
sudo bash scripts/run-kfastblock-transport-matrix.sh
# 或冷启动 post-reboot 全路径
sudo bash scripts/post-reboot-rdma-smoke.sh
```

期望：`RDMA_4K_VERIFY_OK` / `RDMA_MULTI_IO_OK` / `SMOKE_OK`，且
`/sys/module/kfastblock/parameters/rdma_exchange_err` 在跑测期间不增加；
`osd_transport=rdma`，`io_failed=0`。

## 4. 失败排查

| 现象 | 检查项 |
|------|--------|
| OSD 未监听 RDMA | `enable_raw_rdma`、rdma 设备、日志 `start rejected` |
| map 中 port=0 | monitor boot 是否写入 `RawRdmaPort`；OSD 是否 running |
| 连接拒绝 | stop 过程中 reject；`max_connections`；shard 不匹配 |
| RECV 停滞 | multi-slot re-post；CQ 错误后是否 re-arm |
| 停机丢响应 | destroy 前 `drain_send_queue` 超时日志 |

## 5. 停止与统计

OSD 优雅退出应打印：

```
raw RDMA server stopping running=0 ... accept=N reject=M dispatch_err=K ...
raw RDMA server stopped
```

全局 `accept/reject/dispatch_err` 用于回归对比，不因连接关闭而清零。

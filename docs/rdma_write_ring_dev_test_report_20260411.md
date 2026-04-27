# RDMA Write-Ring 开发环境测试报告

## 1. 文档信息
- 测试主题: RDMA write-ring 数据 RPC 功能验证
- 测试日期: 2026-04-11
- 测试执行: Codex
- 仓库对象格式: `sha1`
- 受测代码版本: `43b949509b3189e966a823daa3d3ea57be0a909f`
- 报告范围: 从环境搭建到写 IO 跑通的开发环境验证

说明:
- 当前仓库使用的是 Git `sha1` 对象格式，因此基于 Git commit 的版本号是 SHA-1 commit id，不是 SHA-256。

## 2. 测试目标
- 验证 data RPC 已可运行在新的 RDMA write-ring 传输路径上。
- 验证 3 OSD 开发集群可按单核 OSD 方式启动并保持存活。
- 验证客户端写 IO 可通过 write-ring 路径端到端完成。
- 沉淀一份可复现、可追溯的测试步骤和测试结论，供公司内部归档。

## 3. 测试范围
- 本次纳入:
  - monitor 启动
  - 3 OSD 开发集群启动
  - 单核 OSD 验证
  - PG `active` 验证
  - `block_bench` 4 KiB 写 IO 验证
  - RDMA write-ring 连接证据采集
- 本次不纳入:
  - 生产环境部署验证
  - 长时间稳定性压测
  - delete/read 全量回归
  - raft RPC transport 改造验证
  - 性能签收

## 4. 测试环境

### 4.1 主机环境
- 操作系统: Ubuntu 22.04.5 LTS
- 内核: `Linux fcojo919u2bho9b 6.6.0-azfs+ #1 SMP PREEMPT_DYNAMIC Wed Apr 1 20:53:05 CST 2026 x86_64`
- CPU 逻辑核数: `8`
- RDMA 设备名: `rdmanic`
- 工作目录: `/home/wuxy158/fastblock`

### 4.2 集群拓扑
- monitor 数量: `1`
- OSD 数量: `3`
- 副本数: `3`
- PG 数量: `8`
- 部署模式: 本地 dev cluster
- monitor 地址: `198.19.11.233`
- monitor RPC 端口: `3333`

### 4.3 存储与 OSD 参数
- 后端类型: AIO 文件
- AIO 文件预分配: 开启
- AIO 文件大小: `16G`
- 每个 OSD 核数: `1`
- OSD 内存: `1024 MB`
- hugepage: 关闭

### 4.4 IO 测试参数
- 配置文件: `/tmp/block_bench.tail.json`
- IO 类型: `write`
- IO 大小: `4096`
- IO 数量: `256`
- 队列深度: `4`
- 镜像名: `dev-image`
- 镜像大小: `16777216`
- pool 名称: `fb`
- RDMA 设备: `rdmanic`

## 5. 本次代码整理摘要
- 客户端:
  - 增加 write-ring 连接生命周期管理
  - 增加 leader 变更、临时传输错误下的重试处理
  - 增加重连后的 write-ring 重新获取逻辑
- OSD:
  - 增加 write-ring lease 回收轮询
  - 增加 OSD 停止时 write-ring 服务的有序退出
- Monitor:
  - 调整 OSD down 处理时机，避免在全局 OSD 锁内直接执行下线处理
- 本地存储与工具:
  - 增加 slow object_store write 观测
  - 降低高频逐 IO 日志噪声
  - 修复 `block_bench` inflight 计数
  - 修复 core 枚举路径中的资源释放
- 开发脚本:
  - `vstart.sh` 默认 dev OSD 核数调整为 `1`
  - 增加 OSD id 申请重试
  - 增加 AIO 文件预分配参数
- 仓库整理:
  - 忽略 `.vstart/` 与 `core.*` 生成产物

## 6. 测试前提
- 本地已生成以下二进制:
  - `./build/src/osd/fastblock-osd`
  - `./build/src/tools/block_bench/block_bench`
  - `./monitor/fastblock-mon`
  - `./monitor/fastblock-client`
- 本地已有以下配置:
  - `.vstart/etc/fastblock/fastblock.json`
  - `.vstart/etc/fastblock/block_bench.dev.json`
- pool `fb` 已创建成功。

## 7. 测试步骤与结果

### 7.1 代码基线固定
步骤:
1. 整理源码改动与生成产物。
2. 提交源码改动。
3. 记录受测版本号。

期望结果:
- 源码树可提交、可追溯
- 受测代码版本固定

实际结果:
- 源码已提交成功
- 受测代码版本固定为 `43b949509b3189e966a823daa3d3ea57be0a909f`

测试结果:
- PASS

### 7.2 开发集群搭建
执行命令:
```bash
env FASTBLOCK_VSTART_AIO_PREALLOCATE=1 FASTBLOCK_VSTART_AIO_FILE_SIZE=16G \
  ./vstart.sh -m dev -c 3 -r 3 -C 1 -n rdmanic
```

期望结果:
- monitor 启动成功
- 3 个 OSD 可被拉起
- pool 元数据可正常访问

实际结果:
- 开发环境元数据与 pool 创建成功
- 为了直接观测 OSD 运行状态，最终验证阶段采用了前台 OSD 进程
- 最终存活进程如下:
```text
703303 ./monitor/fastblock-mon -conf=/home/wuxy158/fastblock/.vstart/etc/fastblock/fastblock.json -id=198.19.11.233
788133 ./build/src/osd/fastblock-osd -C /home/wuxy158/fastblock/.vstart/etc/fastblock/fastblock.json --id 1 -N 0
830613 ./build/src/osd/fastblock-osd -C /home/wuxy158/fastblock/.vstart/etc/fastblock/fastblock.json --id 2 -N 0
830872 ./build/src/osd/fastblock-osd -C /home/wuxy158/fastblock/.vstart/etc/fastblock/fastblock.json --id 3 -N 0
```

测试结果:
- PASS

### 7.3 单核 OSD 验证
OSD 启动日志证据:
```text
osd1.log:
Total cores available: 1
Reactor started on core 0
osd 1 is booted

osd2.log:
Total cores available: 1
Reactor started on core 2
osd 2 is booted

osd3.log:
Total cores available: 1
Reactor started on core 1
osd 3 is booted
```

期望结果:
- 每个 OSD 仅声明 1 个可用核心

实际结果:
- 3 个 OSD 均打印 `Total cores available: 1`

测试结果:
- PASS

### 7.4 PG 激活验证
执行命令:
```bash
./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=getpgmap
```

实际结果:
```text
PGID         STATE
1.0          active
1.1          active
1.2          active
1.3          active
1.4          active
1.5          active
1.6          active
1.7          active
```

期望结果:
- pool `fb` 下所有 PG 均进入 `active`

实际结果:
- 8 个 PG 全部为 `active`

测试结果:
- PASS

### 7.5 RDMA write-ring 数据路径验证
执行命令:
```bash
./build/src/tools/block_bench/block_bench -m 0x8 -C /tmp/block_bench.tail.json
```

说明:
- 首次未指定 `-m 0x8` 时，`block_bench` 默认尝试占用 `core 0`，与 `osd1` 的 SPDK core lock 冲突。
- 该问题属于测试环境绑核冲突，不属于 write-ring 功能失败。

期望结果:
- 客户端成功连接
- 成功重新获取 write ring
- 写 IO 端到端完成

客户端观测到的关键证据:
```text
connected to 198.19.11.233:9589, reacquiring write ring
connected to 198.19.11.233:9555, reacquiring write ring
```

`block_bench` 汇总结果:
```text
===============================[write latency]========================================
mean: 84451.3us, p0.1: 20958.2us, p0.5: 21278.7us, p0.9: 21767.5us, p0.95: 31887.1us, p0.99: 39296.8us, p0.999: 40511.3us, iops: 47.1427, iops_dur_sec: 5.43032s, total_io_count: 256
======================================================================================
```

实际结果:
- 客户端成功连上目标 OSD 并重新获取 write ring
- 256 个写 IO 全部完成
- bench 结束后集群仍保持存活

测试结果:
- PASS

### 7.6 测试后集群健康检查
执行命令:
```bash
pgrep -af fastblock-osd
./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=getpgmap
```

期望结果:
- 3 个 OSD 均保持存活
- bench 退出后 PG 仍保持 `active`

实际结果:
- 3 个 OSD 均保持存活
- 8 个 PG 均保持 `active`

测试结果:
- PASS

## 8. 关键证据

### 8.1 功能性证据
- OSD 存活:
  - 验证时刻可见 3 个 OSD 进程。
- PG 状态:
  - 8 个 PG 均为 `active`。
- write-ring 证据:
  - 客户端打印 `connected to ... reacquiring write ring`。
- IO 成功证据:
  - `block_bench` 成功完成 `256` 次写 IO。

### 8.2 性能观测
- 本次以功能验证为主，不作为性能签收数据。
- 成功写入期间，在前台 OSD 输出中观测到如下慢写:
```text
slow object_store write object 1__blk_data___dev-image2 offset 2760704 len 4096 dur 127698us core 0
slow object_store write object 1__blk_data___dev-image3 offset 458752 len 4096 dur 139300us core 0
slow object_store write object 1__blk_data___dev-image2 offset 3469312 len 4096 dur 374741us core 0
```
- 结论:
  - 当前开发环境下的主要慢点在本地 object_store 写入，而不是 write-ring 主路径无法工作。

### 8.3 连接收尾现象
- `block_bench` 退出时，前台 OSD 输出出现 `RDMA_CM_EVENT_DISCONNECTED`。
- 该现象发生在客户端正常断连收尾阶段，没有导致 OSD 退出，也没有导致 PG 变为 `down`。

## 9. 风险与限制
- 本报告只覆盖 data RPC 的 write 路径。
- 当前环境使用虚拟 RDMA 网卡和 AIO 文件后端，延迟与 IOPS 结果不代表生产性能。
- 最终验证使用的是前台 OSD 进程，主要目的是直接观测启动与收尾行为。
- 本报告未覆盖 delete RPC 与 raft RPC 是否已全部迁移到 write 路径。
- 本报告未覆盖长时间 soak、故障注入和恢复回归。

## 10. 测试结论
- 受测版本 `43b949509b3189e966a823daa3d3ea57be0a909f` 已通过开发环境下的 RDMA write-ring data RPC 功能验证。
- 3 OSD 单核开发集群能够启动并保持健康。
- 目标 pool 下的 8 个 PG 均进入并保持 `active`。
- 客户端已能连接并重新获取 RDMA write ring。
- 4 KiB 写 IO 已实现端到端跑通，本次成功完成 `256` 次写入。
- 当前观测到的慢点主要来自本地 object_store 写入，不是 RDMA write-ring 主流程无法运行。

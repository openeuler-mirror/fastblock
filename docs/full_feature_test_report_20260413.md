# fastblock 开发功能综合测试报告

## 1. 文档信息
- 文档名称: fastblock 开发功能综合测试报告
- 编制日期: 2026-04-13
- 编制时间: 2026-04-13 20:23:21 CST
- 编制方式: 汇总既有测试报告并补充本次开发环境验证
- 基线版本: `3a04472a78649417beaee1017f6c651378b42bd5`
- 报告目的: 形成一份覆盖 fastblock 已开发主要功能的统一测试视图，明确已有证据、当前结论与待补项

## 2. 编制说明
- 本报告不是单次大而全重新测试的原始记录，而是对 `docs/` 中既有测试材料的归并整理。
- 本报告在汇总历史结论的基础上，补充纳入了 2026-04-13 在本地开发环境完成的构建、3 OSD dev cluster 启动、PG 激活、基本写读校验以及 RDMA write-ring 路径验证结果。
- 本报告进一步补充纳入了 2026-04-15 完成的 raft 成员变更专项验证结果，确认基础 `change_nodes` 端到端路径已可工作。
- 本报告只覆盖 README 中已经开发完成并有可追溯证据支撑的功能；对仍缺独立测试证据的功能，明确标记为“需补充”。

## 3. 功能范围
依据 [README.md](../README.md) 和现有测试文档，当前纳入综合测试视图的功能域包括：
- 编译与安装
- monitor 集群元数据管理与 OSD 注册
- OSD 启动、mkfs、load、本地存储初始化
- block 客户端基础 IO
- RDMA data RPC / write-ring 路径
- vhost 对接 QEMU
- NVMe-oF target 导出与主机接入
- 单副本与三副本性能
- PG remap、OSD 重启、故障恢复
- RPC 参数兼容性
- 基础 raft 运行能力（选主、日志复制、PG 激活相关路径）

以下功能当前不纳入“已验证完成”结论：
- raft 成员变更复杂场景与完整签收
- raft recovery / snapshot 的独立测试签收
- raft 高级能力的完整开发完成度签收
- 长稳压测
- CI 自动化回归覆盖证明

## 4. 证据来源
- [docs/integrate_test.md](./integrate_test.md)
- [docs/qemu_vhost_test.md](./qemu_vhost_test.md)
- [docs/nvmf_tgt.md](./nvmf_tgt.md)
- [docs/performance_test_20231012.md](./performance_test_20231012.md)
- [docs/performance_failover_test_20240628.md](./performance_failover_test_20240628.md)
- [docs/performance_test_20240731.md](./performance_test_20240731.md)
- [docs/vhost_performance_test_240919.md](./vhost_performance_test_240919.md)
- [docs/performance_test_241210.md](./performance_test_241210.md)
- [docs/rpc_memory_parameter.md](./rpc_memory_parameter.md)
- [docs/rdma_write_ring_dev_test_report_20260411.md](./rdma_write_ring_dev_test_report_20260411.md)
- [docs/raft_membership_test_report_20260414.md](./raft_membership_test_report_20260414.md)
- 2026-04-13 本次补充验证：
  - `./install-deps.sh`
  - `./build.sh -t Release -c monitor`
  - `./build.sh -t Release -c osd`
  - `cd build && make install`
  - `./scripts/create-rdma-rxe.sh -n rdmanic`
  - `./vstart.sh -m dev -c 3 -r 3 -C 1 -n rdmanic`
  - `./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=getpgmap`
  - `BLOCK_BENCH_CPU_MASK=0x8 ./scripts/run-dev-write-read-verify.sh`

## 5. 综合结论
- fastblock 已具备从 monitor、OSD、客户端、vhost、nvmf-tgt 到 block_bench/bdevperf 的主要开发功能。
- 现有文档已对集群部署、QEMU/vhost 对接、NVMe-oF 导出、性能、故障恢复、RDMA write-ring 数据路径等关键能力形成较完整证据链。
- 本次补充验证进一步确认：在带 `rdma_rxe` 的开发内核上，可成功完成依赖安装、完整编译、3 OSD dev cluster 启动、PG 激活、基础写读校验，并确认客户端已走到 RDMA write-ring 获取与使用路径。
- 目前可以确认“基础 raft 相关运行路径可工作”，其中成员变更基础 `change_nodes` 场景已经通过专项验证，但仍不能把 raft 整体表述为“已完整开发完成”。
- 当前最主要的缺口集中在 raft recovery / snapshot 的复杂场景、成员变更复杂场景以及更长时间稳定性测试。

## 6. 功能覆盖矩阵

| 功能域 | 功能项 | 当前结论 | 主要证据 | 本次是否补充验证 | 说明 |
| --- | --- | --- | --- | --- | --- |
| 构建与安装 | 依赖安装、编译 monitor/osd、安装二进制 | 已验证 | [README.md](../README.md) | 是 | 2026-04-13 已完成 `install-deps.sh`、`build.sh`、`make install` |
| 集群基础能力 | monitor 启动、OSD 注册、pool 创建、PG 激活 | 已验证 | [docs/integrate_test.md](./integrate_test.md), [docs/performance_failover_test_20240628.md](./performance_failover_test_20240628.md) | 是 | 本次 dev cluster 中 8 个 PG 均为 `active` |
| OSD 生命周期 | OSD `--mkfs`、启动、load、本地文件后端 | 已验证 | [docs/integrate_test.md](./integrate_test.md), [docs/osd_load.md](./osd_load.md) | 是 | 本次本地 3 OSD 已完成 mkfs 和启动 |
| 本地存储基础 | localstore 初始化与对象/kv/log 落盘路径 | 已验证 | [docs/osd_load.md](./osd_load.md), [docs/performance_failover_test_20240628.md](./performance_failover_test_20240628.md) | 间接 | block_bench 写读校验可作为端到端侧证 |
| 基础 block IO | `block_bench` 写 IO、读回校验 | 已验证 | [docs/performance_test_20240731.md](./performance_test_20240731.md), [docs/rdma_write_ring_dev_test_report_20260411.md](./rdma_write_ring_dev_test_report_20260411.md) | 是 | 本次 `run-dev-write-read-verify.sh` 已成功完成写后读校验 |
| RDMA data RPC | 客户端与 OSD 的 RDMA 数据 RPC | 已验证 | [docs/rdma_write_ring_dev_test_report_20260411.md](./rdma_write_ring_dev_test_report_20260411.md) | 是 | 本次运行日志再次出现 `reacquiring write ring` |
| RDMA write-ring | write-ring 获取、使用、租约回收 | 已验证 | [docs/rdma_write_ring_dev_test_report_20260411.md](./rdma_write_ring_dev_test_report_20260411.md) | 是 | 本次客户端日志和 OSD write-ring 回收日志均提供了证据 |
| vhost 功能 | `fastblock-vhost` 启动、创建设备、QEMU 对接 | 已验证 | [docs/qemu_vhost_test.md](./qemu_vhost_test.md) | 否 | 已有独立功能测试文档 |
| 虚拟机数据一致性 | 客户机内 mkfs、拷贝、md5 一致性校验 | 已验证 | [docs/qemu_vhost_test.md](./qemu_vhost_test.md) | 否 | 已有完整步骤与一致性验证 |
| vhost 性能 | vhost + QEMU 单线程/多线程性能 | 已验证 | [docs/vhost_performance_test_240919.md](./vhost_performance_test_240919.md) | 否 | 已有独立性能报告 |
| nvmf 功能 | `fastblock-nvmf-tgt` 启动、bdev 导出、TCP/RDMA listener、主机发现与连接 | 已验证 | [docs/nvmf_tgt.md](./nvmf_tgt.md) | 否 | 已有独立功能文档 |
| nvmf 性能 | `spdk_nvme_perf` TCP/RDMA、内核 initiator、bdevperf | 已验证 | [docs/performance_test_241210.md](./performance_test_241210.md) | 否 | 已覆盖单副本与三副本场景 |
| block 性能 | `block_bench` 单副本、多 PG、单副本/三副本 | 已验证 | [docs/performance_test_20240731.md](./performance_test_20240731.md), [docs/performance_test_241210.md](./performance_test_241210.md), [docs/performance_failover_test_20240628.md](./performance_failover_test_20240628.md) | 否 | 已有多轮性能数据 |
| 故障恢复 | kill OSD、out OSD、重启、PG remap、异常场景 | 已验证 | [docs/performance_failover_test_20240628.md](./performance_failover_test_20240628.md) | 否 | 已有专门的故障恢复测试记录 |
| 基础 raft 能力 | 选主、日志复制、PG 激活、基础故障恢复路径 | 已验证 | [docs/performance_failover_test_20240628.md](./performance_failover_test_20240628.md), [docs/rdma_write_ring_dev_test_report_20260411.md](./rdma_write_ring_dev_test_report_20260411.md) | 是 | 这里仅指基础运行路径，不代表 raft 全部开发完成 |
| RPC 参数兼容性 | 大 IO、mkfs、vhost、fio 的参数匹配约束 | 已验证 | [docs/rpc_memory_parameter.md](./rpc_memory_parameter.md) | 否 | 可作为部署参数约束证据 |
| 日志与运维配置 | OSD/vhost 日志级别与 systemd 配置 | 已有配置文档 | [docs/osd_log_config.md](./osd_log_config.md) | 否 | 属于运维配置说明，不作为功能签收主项 |
| raft recovery / snapshot | recovery、snapshot 机制与恢复闭环 | 已验证（基础场景） | [docs/raft_recovery_and_snapshot.md](./raft_recovery_and_snapshot.md), [docs/system_self_test_report_20260415.md](./system_self_test_report_20260415.md) | 是 | 已验证落后 follower 在 leader 旧日志被 trim 后，通过 `snapshot_check + installsnapshot` 完成基础恢复；从 0 bootstrap、复杂故障联动和长稳仍需补测 |
| raft 成员变更 | 成员变更算法、`change_nodes` 端到端路径 | 已验证（基础场景） | [docs/raft_change_membership.md](./raft_change_membership.md), [docs/raft_membership_test_report_20260414.md](./raft_membership_test_report_20260414.md) | 是 | 已验证无预创建 PG 的 `change_nodes`、monitor `pgmap` 更新和变更后 `4 up / 4 in / 8 active`；`add/remove` 独立接口、leader 切换中成员变更和长稳仍需补测 |

## 7. 本次补充验证摘要

### 7.1 测试环境
- 系统: `CUOS 4.0 (unicom)`
- 内核: `6.6.0-72.0.0.102.ule4.x86_64`
- RDMA 设备: `rdmanic` (`rdma_rxe`)
- 部署模式: 本地 dev cluster
- monitor 数量: `1`
- OSD 数量: `3`
- 副本数: `3`
- OSD 核数: `1`

### 7.2 测试步骤
1. 执行依赖安装与完整编译。
2. 通过 `rdma_rxe` 创建 `rdmanic` 设备。
3. 使用 `vstart.sh` 启动本地 3 OSD 开发集群。
4. 创建默认 pool `fb`。
5. 通过 `getpgmap` 确认 8 个 PG 全部为 `active`。
6. 使用 `run-dev-write-read-verify.sh` 执行写后读一致性校验。

### 7.3 测试结果
- `monitor`、`fastblock-osd`、`fastblock-client`、`block_bench` 均可正常运行。
- 开发集群内 8 个 PG 均为 `active`。
- 写阶段完成 64 次 IO。
- 读校验阶段完成 64 次 IO。
- 脚本最终输出 `Write-read-verify completed`。
- 运行日志中可见客户端 `connected to ... reacquiring write ring`，与 [docs/rdma_write_ring_dev_test_report_20260411.md](./rdma_write_ring_dev_test_report_20260411.md) 的历史结论一致。
- 本次结果可以支撑基础 raft 运行路径正常，但不足以支撑“raft 全功能开发完成”。

### 7.4 本次补充验证结论
- 本地开发环境下的 monitor、OSD、pool/PG 管理、基础对象写读路径均正常。
- RDMA write-ring 数据路径在开发环境中可重现并完成基础 IO。
- 当前版本具备继续开展更完整回归测试和总报告归档的条件。

## 8. 风险与缺口
- `raft recovery / snapshot` 的基础闭环已验证，但复杂场景、从 0 bootstrap、长稳与故障联动仍缺补测。
- `raft 成员变更` 的基础 `change_nodes` 路径已验证，但 `add/remove` 独立接口、leader 切换中的成员变更、长稳和异常回滚仍需补充。
- 历史性能和功能验证分散在多份报告中，若用于正式对外/对内评审，建议后续继续沉淀统一模板。
- 本报告已纳入本次开发环境验证，但不替代长时间稳定性压测和自动化回归结果。

## 9. 后续建议
- 在现有 [docs/raft_membership_test_report_20260414.md](./raft_membership_test_report_20260414.md) 的基础上，继续补充 `add_node`、`remove_node`、leader 切换中成员变更、异常回滚和长稳场景。
- 在系统总报告的基础上，继续补充 snapshot 的复杂场景，包括从 0 bootstrap、故障联动和长稳。
- 将后续新增能力统一纳入本报告的“功能覆盖矩阵”，避免测试资产继续分散。
- 对已存在的功能测试文档，建议逐步统一文档头、环境信息、测试范围、证据与结论格式。

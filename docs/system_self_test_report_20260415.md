# fastblock 系统级自测总报告

## 1. 文档定位
- 文档名称: fastblock 系统级自测总报告
- 文档日期: 2026-04-17
- 当前基线: `9f6dbbb`
- 文档目标: 以“分布式存储系统”的视角，对 fastblock 当前已验证能力、已知缺口、关键测试证据和后续补测优先级做统一归档
- 编制原则: 先定义系统级测试大纲，再按测试域逐项填充证据与结论；报告正文应尽量自包含，不要求读者再跳到其他专项文档才能理解主结论

## 2. 报告范围
本报告覆盖的不是“某一个功能是否能跑”，而是分布式存储系统应具备的几个核心维度：
- 可构建、可部署
- 控制面可工作
- 数据面可工作
- 基础一致性与正确性可验证
- 故障与恢复路径可验证
- 共识与副本管理能力可验证
- 对外协议能力可验证
- 基础性能有证据
- 运维与参数约束可说明

本报告当前不把以下项目表述为“已整体签收”：
- raft recovery / snapshot 复杂场景与完整签收
- raft 成员变更复杂场景
- 长稳压测
- 升级 / 回滚
- 自动化回归覆盖证明

## 3. 系统级自测大纲

### 3.1 构建与交付
关注点：
- 依赖安装是否可重复
- monitor / osd / client / vhost / nvmf-tgt / block_bench 是否可编译
- 安装后二进制是否完整可用

### 3.2 控制面
关注点：
- monitor 启动
- OSD 申请 ID / boot / 注册
- pool 创建
- pgmap 下发与收敛
- osdmap 下发与状态传播

### 3.3 数据面与正确性
关注点：
- block client 基础写读
- 写后读校验
- RDMA data path / write-ring 路径是否真正被走到
- 变更或故障后基础 IO 是否仍可工作

### 3.4 分布式一致性与副本管理
关注点：
- raft 基础选主
- raft 日志复制
- pg active 收敛
- raft 成员变更
- raft recovery / snapshot

### 3.5 故障处理与恢复
关注点：
- kill OSD
- out / in
- remap
- OSD 重启
- 故障后集群状态与 IO 恢复

### 3.6 对外接入协议
关注点：
- vhost + QEMU
- NVMe-oF TCP/RDMA
- 主机侧发现、连接、读写

### 3.7 性能基线
关注点：
- 单副本
- 三副本
- block_bench
- bdevperf
- vhost 性能
- nvmf 性能

### 3.8 运维与配置约束
关注点：
- RPC / 内存参数约束
- OSD / vhost 日志配置
- 本地 dev 环境与 systemd 运行方式说明

### 3.9 尚未覆盖的系统级项目
关注点：
- 长时间稳定性
- 升级 / 回滚
- 混合故障场景
- CI 自动化回归

## 4. 当前覆盖矩阵

| 测试域 | 关键项 | 当前状态 | 当前结论 | 主要缺口 |
| --- | --- | --- | --- | --- |
| 构建与交付 | 依赖安装、完整编译、安装二进制 | 已验证 | 当前代码基线可以在开发环境完成依赖安装、编译和安装 | 仍缺离线/内网交付链路专项验证 |
| 控制面 | monitor 启动、OSD boot、pool 创建、pgmap 收敛 | 已验证 | monitor、osd、pool、PG 基础管理可工作 | 复杂异常场景覆盖不足 |
| 数据面与正确性 | block 基础写读、写后读校验、RDMA write-ring | 已验证 | 基础 IO 与 RDMA write-ring 路径可工作 | 协议间交叉验证仍可补充 |
| 分布式一致性 | raft 选主、日志复制、pg active | 已验证（基础场景） | 基础 raft 运行路径可工作 | snapshot / recovery 复杂场景仍缺补测 |
| raft recovery / snapshot | 日志不足 recovery、snapshot_check、installsnapshot、恢复后继续追平 | 已验证（基础场景） | 已验证落后 follower 在 leader 旧日志被 trim 后，可通过 `snapshot_check + installsnapshot` 完成恢复并重新追平 | 仍未覆盖从 0 bootstrap、复杂故障联动、长稳 |
| raft 成员变更 | 无预创建 PG 的 `change_nodes`、monitor pgmap 更新、变更后基础 IO 回归 | 已验证（基础场景） | 基础成员变更路径已打通，变更后 pgmap 能切到新成员集合，且可继续完成基础写读校验 | leader 切换中的成员变更场景当前失败，长稳未补 |
| 故障处理与恢复 | kill OSD、out/in、remap、故障后基础 IO | 已验证（基础场景） | 基础故障恢复路径可工作 | 更复杂多故障场景未补 |
| 对外接入协议 | vhost、QEMU、NVMe-oF | 已验证 | 对外协议面已有功能证据 | 与成员变更/故障联动的协议层回归未补 |
| 性能基线 | 单副本、三副本、vhost、nvmf、block_bench | 已验证 | 性能已有基础数据集 | 长稳性能与回归基线未自动化 |
| 运维与配置约束 | RPC 参数、日志配置 | 已验证 / 已有文档沉淀 | 可支撑部署与调参说明 | 缺统一运维手册 |
| 系统级长期能力 | 长稳、升级、CI 回归 | 待验证 | 当前不能给出系统级签收结论 | 需要单独补测 |

## 5. 关键测试证据摘要

### 5.1 构建与交付证据
已完成以下关键动作：
- `./install-deps.sh`
- `./build.sh -t Release -c monitor`
- `./build.sh -t Release -c osd`
- `cd build && make install`

结果：
- `fastblock-mon`
- `fastblock-client`
- `fastblock-osd`
- `fastblock-vhost`
- `fastblock-nvmf-tgt`
- `block_bench`
均可生成并安装。

结论：
- 当前代码基线具备可构建、可安装的交付能力。

### 5.2 基础健康检查证据
系统级自测最前面应有一层“基础健康检查/可见性检查”，用于回答：
- monitor 是否可访问
- 集群状态是否可见
- pgmap 是否可见
- osdmap 是否可见
- PG 到 OSD 的映射是否可解释

当前可直接使用的检查命令包括：
- `./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=status`
- `./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=getpgmap`
- `./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=getosdmap`
- `./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=getpgosds -poolid=<pool_id>`
- `./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=listpools`

说明：
- 当前工具集中没有单独名为 “osd tree” 的直接命令。
- 但 `getosdmap` 可以展示 OSD 的 up/in、地址、端口等动态信息；`getpgosds` 和 `getpgmap` 可以展示 PG 到 OSD 的映射关系。
- 对当前这套系统，自测报告里可以把这三类输出作为“基础拓扑可见性”的实际证据。

建议口径：
- 基础健康检查通过后，才进入数据面、一致性、故障恢复和协议层验证。
- 如果基础健康检查都无法稳定返回，那么后续深层测试结论不应视为有效。

### 5.3 控制面证据
在本地 dev 环境中，已多次验证：
- monitor 可启动
- OSD 可完成 `applyid` / `boot`
- pool `fb` 可创建
- `getpgmap` 可正常返回
- PG 可收敛到 `active`

典型结果：
- `4 osds: 4 up, 4 in`
- `8 active`

结论：
- 控制面基础链路可工作。

### 5.4 数据面与正确性证据
已完成本地基础写读校验：
- 通过 `run-dev-write-read-verify.sh` 完成写后读校验
- 写阶段完成 64 次 IO
- 读阶段完成 64 次 IO
- 脚本最终输出 `Write-read-verify completed`

同时，开发环境再次确认客户端运行日志中出现 `reacquiring write ring`，说明 RDMA write-ring 路径确实被走到。

结论：
- 数据面基础写读链路可工作。
- RDMA write-ring 路径有直接运行证据。

### 5.5 故障与恢复证据
已有验证覆盖：
- kill OSD
- `outosd`
- PG remap
- 故障后基础 IO 恢复

此前已修复过 failover 路径中的 raft/连接清理问题，并通过基础 failover 烟测：
- 故障后集群可维持 `3 up / 3 in`
- PG 可保持 `active`
- 故障后写读校验可通过

结论：
- 基础故障恢复路径可工作。

### 5.6 对外协议与性能证据
已有资料显示以下能力具备独立证据：
- vhost + QEMU 功能验证
- NVMe-oF TCP/RDMA 功能验证
- 单副本 / 三副本性能数据
- vhost 性能数据
- nvmf 性能数据

结论：
- 当前系统不仅有控制面和数据面基础能力，也有对外协议和性能侧的初步证据支撑。

### 5.7 raft recovery / snapshot 证据
本轮新增完成了基于本地 RDMA dev cluster 的 snapshot 恢复专项验证，目标是验证：
- 当 follower 落后到 leader 已经 trim 掉旧日志后
- 系统是否会从普通 raft log recovery 切换到 `snapshot_check + installsnapshot`
- follower 是否能完成对象安装、推进 raft 索引并重新追平

测试方法：
- 使用 `3 OSD / 3 replica` 本地 RDMA dev cluster
- 选定 `PG 1.0`
- 下线一个 follower
- 使用 [snapshot_pg_stress.cc](./../src/test/snapshot_pg_stress.cc) 对 `PG 1.0` 的固定 object 持续写入
- 为了快速触发 trim，自测时临时下调了 raft log trim 阈值
- 拉回 follower
- 再打一笔新的 `PG 1.0` 写入触发 recovery

本轮同时确认并固化了本地 dev 环境下需要的消息参数：
- 小内存开发机场景下：
  - `msg_server_data_memory_pool_capacity = 2048`
  - `msg_client_data_memory_pool_capacity = 2048`
- 为避免 payload snapshot 被 RPC timeout 打断：
  - `msg_server_rpc_timeout_us = 600000000`
  - `msg_client_rpc_timeout_us = 600000000`

关键运行证据：
- leader 侧日志已出现：
  - `do_recovery`
  - `recovery by snapshot`
  - `send snapshot_check_request`
  - `send_snapshot`
  - `send installsnapshot_request`
  - `recvd installsnapshot_response`
- follower 侧日志已出现：
  - `recvd snapshot_check_request`
  - `snapshot_check ready`
  - `recvd installsnapshot_request ... object_size 1`
  - `installsnapshot write_fn done=0`
  - `recvd installsnapshot_request ... done=1 object_size 0`
  - `set _last_applied_idx, _current_idx, _commit_idx to ...`

最终结果：
- follower 对 payload snapshot 的第一批对象安装成功
- 随后收到收尾批次并推进：
  - `_last_applied_idx`
  - `_current_idx`
  - `_commit_idx`
- leader 收到 `installsnapshot_response success: 1`
- leader 日志出现 `snapshot is end.`
- 随后 follower 能继续接收并 apply 新的普通日志
- 集群最终恢复到：
  - `3 up / 3 in`
  - `8 active`

本轮结论：
- `raft recovery / snapshot` 的基础闭环已经验证通过
- 之前阻塞该场景的主要问题有两类：
  - recovery 期间 heartbeat 干扰与 `E_INVAL` 处理不当
  - 小内存 dev 配置下 `msg_*_data_memory_pool_capacity` 太小，导致 payload installsnapshot 无法发送
- 这两类问题已经分别通过：
  - [raft_server.cc](./../src/raft/raft_server.cc)
  - [vstart.sh](./../vstart.sh)
 进行修正和固化

仍未覆盖的 snapshot 范围：
- 完全“从 0 开始”的空节点 bootstrap
- snapshot 与成员变更联动
- snapshot 过程中的复杂故障
- 长时间稳定性

## 6. raft 成员变更专项结果摘录
本节是系统总报告的一部分，直接纳入成员变更专项验证结论，不要求读者再去看单独文档才能理解结论。

### 6.1 问题背景
最初的成员变更专项测试中，直接执行 `change_nodes` 失败，返回：
- `change membership in the pg 1.0 result: -144`

错误码解释：
- `-144 = RAFT_ERR_NOT_FOUND_PG`

关键现象：
- 新成员本地还没有目标 PG
- leader 已经开始向新成员发送 catch-up `append_entries`
- 新成员直接返回 `not find pg 1.0`

这说明当时成员变更主流程缺少一条关键联动：
- 在新成员上创建 PG
- 再进行 catch-up
- 再完成最终配置切换

### 6.2 修复点
针对成员变更这条链，本轮已经修掉以下问题：
- 在 raft 成员变更 catch-up 阶段，先对新成员自动执行 `create_pg`，再发送第一批 `append_entries`
- follower 收到配置变更 entry 后，立即更新本地成员视图，避免已被移出的旧成员继续对该 PG 参与选举
- monitor 侧修复 `ProcessPgMemberChangeFinish()` 的锁释放问题，避免成员变更完成后卡住后续 `GetClusterMapRequest`
- monitor 侧调整成员变更完成后的最终落盘逻辑，不再只依赖 `PgRemapped`；只要请求成功且最终成员集合与当前 `pgmap` 不一致，就按最终成员集合更新 `pgmap`
- OSD monitor client 增加 cluster-map 响应超时保护，避免 monitor 响应链路异常时长时间卡死

### 6.3 端到端验证场景
在 `4 OSD / 3 replica` 本地 dev cluster 中，选取 `PG 1.0` 做成员变更。

最终验证场景里：
- 变更前 `PG 1.0` 成员：`[1,4,2]`
- 目标成员集合：`[2,3,4]`

执行命令：
```bash
./build/src/test/raft_membership   --no-huge -s 1024   -m 0x10   -C .vstart/etc/fastblock/fastblock.json   -I 1   -o 10.211.55.29   -t 9818   -P 1   -G 0   -N change   -O '2:10.211.55.29:9087,3:10.211.55.29:9232,4:10.211.55.29:9965'
```

结果：
- `change membership in the pg 1.0 result: 0`
- monitor 收到 `PgMemberChangeFinishRequest, pg 1.0`
- 变更后再次执行 `getpgmap`，`PG 1.0` 变为：
  - `[2,4,3]`

说明：
- `[2,4,3]` 与目标成员集合 `[2,3,4]` 等价
- 说明 monitor 侧 `pgmap` 已正确收敛到新的成员集合
- 这次验证是在“新成员未预创建 PG”的前提下直接完成的

### 6.4 变更后系统状态与基础 IO 回归
成员变更完成后再次检查：
- `status` 仍显示 `4 osds: 4 up, 4 in`
- `8 active`
- `getpgmap` 仍可继续返回

随后继续执行基础写读校验：
```bash
BLOCK_BENCH_CPU_MASK=0x10 ./scripts/run-dev-write-read-verify.sh
```

结果：
- 写阶段完成 64 次 IO
- 读校验阶段完成 64 次 IO
- 脚本最终输出 `Write-read-verify completed`

结论：
- 成员变更完成后，集群基本状态保持正常
- monitor 不再出现之前那种响应卡住和误判 down 的问题
- 变更后基础 IO 仍可成功完成，说明支持面内的成员变更与基础数据面可以串起来

### 6.5 成员变更测试范围说明
当前系统级自测只把 `change_nodes` 视为对外支持、且需要验证的成员变更入口。

说明：
- `add_node` / `remove_node` 在代码层面存在，但不属于当前对外支持面的系统级能力。
- 因此系统级总报告不再把它们当作必须通过的测试项，也不以它们的行为定义对外口径。
- 当前系统级可验证、可对外表述的成员变更路径，应以 `change_nodes` 为准。

### 6.6 当前口径
当前可以明确说：
- raft 成员变更的基础 `change_nodes` 端到端路径已经验证通过
- 无预创建 PG 场景已打通
- monitor `pgmap` 最终成员落盘已验证
- 成员变更完成后基础 IO 回归已验证

但当前还不能说：
- 成员变更复杂场景已经全部签收

仍建议继续补测：
- 成员变更过程中的 leader 切换
- 成员变更后的长时间稳定性
- 与 snapshot/recovery 的联动场景

### 6.7 leader 切换中的成员变更结果
本轮新增补测了“`change_nodes` 过程中 leader 故障”场景，结果为失败。

测试环境：
- `4 OSD / 3 replica` 本地 RDMA dev cluster
- `PG 1.0` 初始成员为 `[1,4,3]`
- 日志确认 `osd3` 为 `PG 1.0` 当前 leader

测试动作：
- 向 leader `osd3` 发起 `change_nodes`，目标成员集合为 `[2,3,4]`
- `raft_membership` 日志显示：
  - `leader of the pg 1.0 is 3`
  - `change membership in the pg 1.0 result: 0`
- 在请求返回成功后约 `0.5s` 强制 `kill -9 osd3`

实际结果：
- monitor 收到 `PgMemberChangeFinishRequest, pg 1.0`
- `getpgmap` 显示 `PG 1.0` 的成员集合已切到 `[2,3,4]`
- 但集群没有收敛回健康状态，而是退化到：
  - `2 up / 3 in`
  - `PG 1.0` 长时间保持 `down`
- `getosdmap` 最终显示：
  - `osd2 down/out`
  - `osd3 down/out`
  - 仅 `osd1`、`osd4` 保持 `up/in`

额外证据：
- `raft_membership` 在 leader 被打掉后持续出现
  - `RDMA_CM_EVENT_DISCONNECTED`
  - `cant find the connection with dispatch id ...`
  最终超时退出
- monitor 日志可见：
  - `Received PgMemberChangeFinishRequest, pg 1.0`
  - 随后又出现新的 `LeaderBeElectedRequest, pg 1.0`
- 内核 `dmesg` 记录到 `osd2` 对应的 `reactor_1` 发生 segfault：
  - 崩溃地址再次落在 `google::protobuf::internal::TypeDefinedMapFieldBase<unsigned int, msg::ShardCore>::SpaceUsedExcludingSelfNoLockImpl(...)`

阶段结论：
- “基础 `change_nodes` 路径”已通过
- “leader 切换中的成员变更”当前未通过
- 这不是 monitor `pgmap` 未更新的问题，而是 leader 故障后系统稳定性问题，且伴随额外 OSD 崩溃

## 7. 当前可以使用的系统级结论口径
建议使用下面这组口径：
- fastblock 当前已经完成并验证了基础控制面、基础数据面、基础故障恢复、基础 raft 运行路径、基础 raft 成员变更路径、基础 raft recovery / snapshot 路径以及主要对外协议能力。
- 当前版本已经可以作为“开发阶段系统级可工作版本”进行汇报。
- 但若要给出“完整签收”或“发布级”结论，仍需补充 snapshot 复杂场景、成员变更复杂场景、长稳与升级回归。

不建议使用下面这类口径：
- “raft 全部完成”
- “分布式一致性能力全部签收”
- “系统已完成全量自测”

## 8. 后续补测优先级
建议按下面顺序继续补：

### P0
- 修复成员变更过程中 leader 切换失败问题
- leader 切换场景修复后的基础 IO 回归
- snapshot 的复杂场景补测

### P1
- 故障恢复与成员变更联动场景
- vhost / nvmf 在成员变更后的回归读写

### P2
- 长稳压测
- 升级 / 回滚
- 自动化回归矩阵

## 9. 最终判断
当前 fastblock 已经具备“系统级基础自测框架 + 多测试域证据 + 关键缺口明确”的状态。

换句话说：
- 现在已经不是“零散功能点证明”阶段
- 已经进入“系统级自测逐域收敛”阶段
- 下一步工作重点不是再证明某一个孤立功能点，而是按本报告的大纲把剩余关键测试域补齐

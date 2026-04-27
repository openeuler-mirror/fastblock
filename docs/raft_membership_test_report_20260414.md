# Raft 成员变更专项测试报告

## 1. 文档信息
- 测试主题: raft 成员变更专项验证
- 初始问题定位日期: 2026-04-14
- 最终验证日期: 2026-04-15
- 测试环境: 本地 dev cluster，`4 OSD / 3 replica`
- 基线版本: `213b504`
- 测试执行: Codex

## 2. 测试目标
- 验证当前代码中的 `change_raft_membership` 路径是否可用。
- 验证成员变更在“新成员未预创建 PG”的场景下能否直接完成。
- 验证 monitor 在收到 `PgMemberChangeFinishRequest` 后，能否正确把 `pgmap` 收敛到最终成员集合。
- 验证成员变更完成后，集群是否仍能维持 `active`、`up/in` 的基本可用状态。

## 3. 测试环境
- 系统: CUOS 4.0
- 内核: `6.6.0-72.0.0.102.ule4.x86_64`
- RDMA 设备: `rdmanic` (`rdma_rxe`)
- 部署方式: `./vstart.sh -m dev -c 4 -r 3 -C 1 -n rdmanic`
- 默认 pool: `fb`
- PG 数量: `8`
- 测试工具: `build/src/test/raft_membership`
- 测试工具启动参数说明:
  由于当前开发环境使用 `--no-huge`，`raft_membership` 需配合 `--no-huge -s 1024` 启动，避免 hugepage 和 bdev 初始化失败。

## 4. 问题定位与修复摘要

### 4.1 初始失败现象
2026-04-14 的第一轮专项测试中，直接执行 `change_nodes` 失败，返回:
- `change membership in the pg 1.0 result: -144`

错误码解释:
- `-144 = RAFT_ERR_NOT_FOUND_PG`

关键日志证据:
- leader 侧日志:
  `handle append entry request of pg 1.0 at node 4 failed: no pg found`
- 新成员侧日志:
  `not find pg 1.0`

这说明当新成员节点本地尚未创建目标 PG 时，leader 已经开始向它发送 catch-up `append_entries`，导致成员变更主流程直接失败。

### 4.2 本轮修复内容
针对上述问题及后续联动问题，本轮代码修复包括:
- 在 raft 成员变更 catch-up 阶段，先对新成员自动执行 `create_pg`，再发送第一批 `append_entries`。
- follower 收到配置变更 entry 后，立即更新本地成员视图，避免已被移出的旧成员继续对该 PG 参与选举。
- monitor 侧修复 `ProcessPgMemberChangeFinish()` 的锁释放问题，避免成员变更完成后卡住后续 `GetClusterMapRequest`。
- monitor 侧调整成员变更完成后的最终落盘逻辑，不再只依赖 `PgRemapped` 状态；只要请求成功且最终成员集合与当前 `pgmap` 不一致，就按最终成员集合更新 `pgmap`。
- OSD monitor client 增加 cluster-map 响应超时保护，避免 monitor 响应链路异常时长时间卡死。

## 5. 最新端到端验证

### 5.1 建立 4 OSD 开发集群
执行:
```bash
./scripts/create-rdma-rxe.sh -n rdmanic
./vstart.sh -m dev -c 4 -r 3 -C 1 -n rdmanic
```

结果:
- 集群启动成功
- pool `fb` 创建成功
- `status` 显示 `4 up, 4 in`
- `getpgmap` 显示 8 个 PG 均为 `active`

### 5.2 验证对象
选取 `PG 1.0` 作为目标 PG。

本轮最终验证中，`PG 1.0` 的初始成员为:
- `1`
- `4`
- `2`

目标成员集合为:
- `2`
- `3`
- `4`

说明:
- `pgmap` 中成员顺序可能与目标列表不同，但只要成员集合一致，即认为变更成功。

### 5.3 直接执行 `change_nodes`（无预创建 PG）
执行:
```bash
./build/src/test/raft_membership   --no-huge -s 1024   -m 0x10   -C .vstart/etc/fastblock/fastblock.json   -I 1   -o 10.211.55.29   -t 9818   -P 1   -G 0   -N change   -O '2:10.211.55.29:9087,3:10.211.55.29:9232,4:10.211.55.29:9965'
```

结果:
- 工具成功连接目标 OSD，并查询到 `PG 1.0` 的 leader。
- 变更请求返回:
  - `change membership in the pg 1.0 result: 0`

关键证据:
- monitor 日志出现:
  - `Received PgMemberChangeFinishRequest, pg  1 . 0`
- 变更后再次执行 `getpgmap`，`PG 1.0` 变为:
  - `[2,4,3]`

说明:
- `[2,4,3]` 与目标成员集合 `[2,3,4]` 等价，说明 monitor 侧 `pgmap` 已正确收敛到新成员集合。
- 本轮验证已证明“新成员未预创建 PG”的直接成员变更场景可以成功完成。

### 5.4 变更后集群可用性检查
变更完成后再次执行:
```bash
./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=status
./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=getpgmap
```

结果:
- `status` 仍显示:
  - `4 osds: 4 up, 4 in`
  - `8 active`
- `getpgmap` 仍可正常返回，未出现之前的 monitor 响应卡死问题。

说明:
- 成员变更完成后，集群基本状态保持正常。
- monitor 能持续处理后续 `GetClusterMapRequest`，不再出现之前那种因锁泄漏导致的响应卡住与误判 down 问题。

## 6. 测试结论

### 6.1 当前实现状态
当前仓库中的 raft 成员变更不是空壳，已经完成并验证了以下基础能力:
- `change_raft_membership`
- 新成员自动 `create_pg`
- catch-up + 配置切换
- monitor `pgmap` 最终成员落盘
- 变更完成后的基础集群可用性保持

### 6.2 本次专项测试结论
本次专项测试结论如下:

1. 直接执行 `change_nodes`（无预创建 PG）:
   - 成功
   - 不再出现 `RAFT_ERR_NOT_FOUND_PG`
   - 新成员自动建 PG 的联动链路已打通

2. monitor 侧 `pgmap` 更新:
   - 成功
   - `PG 1.0` 已从旧成员集合切换到目标成员集合

3. 变更后集群基本状态:
   - 正常
   - 仍保持 `4 up / 4 in / 8 active`
   - monitor 后续 `status/getpgmap` 仍可正常工作

### 6.3 是否可以说“raft 成员变更已完成”
更准确的说法应为:
- raft 成员变更的基础 `change_nodes` 端到端路径已验证通过
- 可以作为“基础能力已实现并通过专项验证”进行表述
- 但更复杂场景仍建议继续补测，例如:
  - `add_node`
  - `remove_node`
  - 成员变更过程中 leader 切换
  - 成员变更后的长时间稳定性
  - 与 snapshot/recovery 的联动场景

## 7. 最终判断
- 当前状态: `基础场景已验证`
- 能力结论:
  - “代码已实现”成立
  - “无预创建 PG 的基础成员变更路径已验证”成立
  - “复杂场景与完整签收”仍建议补充后续专项测试

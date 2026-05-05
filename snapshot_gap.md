# Snapshot Gap Tracker

日期：2026-05-05

## 1. 目的

本文档用于跟踪 fastblock `image snapshot / clone / rollback / flatten` 能力距离“可签收后端 MVP”还差什么。

本文档默认只讨论“用户可见的 image snapshot”：

- `create / list / get / delete`
- `protect / unprotect`
- `clone`
- `rollback`
- `flatten`
- 后续 `CSI snapshot / restore`

本文档**不**把 `raft recovery snapshot` 当作同一件事处理，避免追踪口径混乱。

## 2. 当前判断

截至 2026-05-05，snapshot 的整体状态可以概括为：

- 后端主链路已经基本具备，已经不是“只有设计、没有实现”的阶段。
- `monitor -> monclient -> client/bdev -> exporter` 的调用链已经打通。
- `create / protect / clone / rollback / flatten` 均已有代码路径。
- `nvmf` 侧已经有串联 smoke 脚本，可以跑一条基础流程。
- 但它还不能直接算“正式签收完成”。

当前最关键的差距不在“有没有 API”，而在：

- 持久化语义是否成立
- 删除与 GC 是否闭环
- 重启/故障后语义是否仍然成立
- 是否已经具备固定、可重复的端到端回归证据
- `nvmf / exporter` 侧是否已经稳定到足以承载完整功能联调

## 3. 已有能力概览

| 能力 | 当前状态 | 说明 |
| --- | --- | --- |
| image snapshot create | 已有实现 | monitor 元数据、client 路径、bdev RPC 已存在 |
| snapshot list / get | 已有实现 | exporter 和 bdev RPC 已支持 |
| snapshot delete | 已有实现 | 有删除保护约束，但完整 GC 未闭环 |
| protect / unprotect | 已有实现 | 已检查 child 依赖 |
| clone | 已有实现 | 仅允许从 protected snapshot 创建 |
| clone 读 fallback | 已有实现 | `libblk_client` 已有父链回源逻辑 |
| clone 首次写 copy-up | 已有实现 | partial write 首次写会走 copy-up |
| rollback | 已有实现 | 已有按快照版本回写当前 image 的路径 |
| flatten | 已有实现 | 有数据物化和 metadata finalize 路径 |
| exporter 北向接口 | 已有实现 | HTTP 已支持 snapshot/clone/rollback/flatten |
| nvmf smoke 串联 | 已有脚本 | 已有一条串行 smoke 流程 |
| CSI snapshot | 进行中 | 工作区已有代码，但未形成已提交基线 |

## 4. 关键缺口

### P0-1 持久 snapshot 生命周期未完全站稳

问题：

- `object_store::stop()` 当前仍会删除 snapshot blob。
- 这与“业务 snapshot 持久存在、重启后仍可见”的语义冲突。

影响：

- 只要这点不修，snapshot 的持久化语义就不可靠。
- 重启后的 snapshot/clone/rollback 行为无法被正式签收。

验收标准：

- `stop()` 不再删除业务 snapshot blob。
- 重启前创建的 snapshot，重启后仍能被列出、读取、克隆、回滚。
- clone 在重启后仍能继续读取 parent snapshot 数据。

最新进展：

- `object_store::stop()` 已开始修正为“关闭 business snapshot blob，而不是删除”。
- `recovery snapshot` 与 `business snapshot` 的 stop 路径已经开始分离。
- 但“重启前创建 snapshot，重启后仍可 list/read/clone/rollback”的固定验证还未完成。

状态：进行中

### P0-2 删除与 GC 闭环不完整

问题：

- 当前删除快照更接近“标记删除”而不是“完整清理”。
- snapshot / clone / flatten 之后的历史对象版本何时可回收，闭环仍不清晰。

影响：

- 生命周期不能正式闭环。
- 时间一长会积累悬挂对象、悬挂版本或保守残留数据。

验收标准：

- snapshot delete 后状态、索引、对象历史版本的处理规则清晰且一致。
- flatten 后 parent 依赖解除，并能安全推进后续删除。
- image delete、snapshot delete、clone delete 的引用关系和回收规则可证明正确。

状态：未完成

### P0-3 缺固定化的重启/故障回归

问题：

- 现有代码和 smoke 能说明“基础链路存在”，但不能代替正式签收回归。
- 当前最缺的是重启、异常中断、恢复后再读写的固定化测试。

影响：

- 很难判断 snapshot 目前是“功能真稳定”还是“路径刚好能跑通”。

验收标准：

- 形成固定脚本或固定用例，至少覆盖：
  - create snapshot
  - protect snapshot
  - create clone
  - clone read fallback
  - clone partial write copy-up
  - rollback
  - flatten
  - 重启后重复验证
- 输出一份独立测试记录或报告。

状态：未完成

### P0-4 nvmf / exporter 联调稳定性仍不足

问题：

- `nvmf-tgt` 与 exporter 的管理链路已经具备主要功能，但联调过程中仍暴露出 target/export 路径不够稳定的问题。
- 当前问题已经不只是“缺脚本”，而是 `nvmf` 侧本身还存在需要继续收敛的运行时问题。

影响：

- 即使后端 snapshot/clone/rollback 代码路径存在，也可能因为 `nvmf` 导出链路不稳而无法形成稳定联调结论。

验收标准：

- `register existing image`
- `create snapshot`
- `rollback`
- `clone`
- `flatten`

以上动作在 `nvmf` 侧可重复执行，不出现 target 进程被打挂、export 异常失效、重连后 namespace 不稳定等问题。

状态：未完成

### P1-1 端到端证据还不够系统

问题：

- 当前已有 `nvmf` smoke，但更偏联调链路，不等于完整签收矩阵。
- 还缺覆盖不同对象分布、clone-of-clone、flatten 后 parent 删除等场景的系统化证据。

验收标准：

- 至少补齐以下专项场景：
  - base image -> snapshot -> clone -> read/write
  - flatten 后删除 parent snapshot
  - clone-of-clone
  - rollback 后基础读写
  - 重复执行的幂等或失败保护行为

状态：未完成

### P1-2 错误语义、约束和可观测性还偏薄

问题：

- 目前主路径更偏“先打通能力”，对外错误语义、运维可观测性和问题定位材料还不够强。

验收标准：

- 关键失败路径有稳定错误返回。
- 日志能明确区分 create/protect/clone/rollback/flatten 的阶段。
- 至少有最小运维排查手册或测试说明。

状态：未完成

### P2-1 CSI 北向尚未进入稳定基线

问题：

- 工作区已经开始接 `CreateSnapshot / DeleteSnapshot / ListSnapshots / CreateVolumeFromSnapshot`。
- 但当前仓库已提交基线中，CSI snapshot 仍不应视为完成。

验收标准：

- CSI controller snapshot 生命周期走通。
- `CreateVolume from snapshot` 稳定可用。
- 与现有 volume publish / unpublish / delete 语义不冲突。
- 至少完成一轮 Kubernetes 侧联调验证。

状态：进行中

## 5. 建议执行顺序

建议按下面顺序推进，不要倒序：

1. 先完成 `P0-1`，修正业务 snapshot 的持久化生命周期。
2. 再完成 `P0-2`，把 delete / flatten / GC 约束补到可闭环。
3. 然后完成 `P0-3`，形成固定化重启与故障回归。
4. 并行收敛 `P0-4`，把 `nvmf / exporter` 联调链路稳定下来。
5. 再补 `P1-1` 和 `P1-2`，把证据链、错误语义和可观测性补强。
6. 最后推进 `P2-1`，把 CSI snapshot 正式接入并联调。

## 6. 追踪表

| 优先级 | 任务 | 当前状态 | 验收标准 | 负责人 | 开始时间 | 结束时间 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| P0 | 修正业务 snapshot 持久化生命周期 | 未完成 | `stop()` 不删除业务 snapshot；重启后 snapshot 仍可用 | 待定 | 待定 | 待定 | 首要 blocker |
| P0 | 补完 snapshot delete / flatten / GC 闭环 | 未完成 | 生命周期、引用关系、回收规则一致 | 待定 | 待定 | 待定 | 与持久化问题强相关 |
| P0 | 建立重启/故障回归脚本与结果记录 | 未完成 | 有固定脚本和固定结论 | 待定 | 待定 | 待定 | 用于签收 |
| P0 | 稳定 nvmf / exporter 联调链路 | 未完成 | target/export 可重复联调，不再随机失效 | 待定 | 待定 | 待定 | 当前真实 blocker |
| P1 | 补齐更完整 e2e 场景矩阵 | 未完成 | clone-of-clone、flatten 后删 parent 等通过 | 待定 | 待定 | 待定 | 补证据 |
| P1 | 补强错误语义与可观测性 | 未完成 | 失败场景可定位、可解释 | 待定 | 待定 | 待定 | 提升可维护性 |
| P2 | 接入并验证 CSI snapshot | 进行中 | CSI lifecycle + restore 联调通过 | 待定 | 待定 | 待定 | 放在后端站稳之后 |

## 7. 最近一轮建议

当前最值得先做的一件事：

- 直接修 `business snapshot` 的持久化生命周期问题。

完成这一步后，立即补一轮最小验证：

1. 创建 image
2. 创建 snapshot
3. protect snapshot
4. 基于 snapshot 创建 clone
5. 重启相关进程
6. 再次 list snapshot
7. 再次读取 clone
8. 执行 rollback 或 flatten

只有这条最小链路重启后仍成立，snapshot 才算真正迈过“能跑”和“可签收”之间的门槛。

## 8. 备注

后续每次推进 snapshot 相关工作时，建议直接更新本文档：

- 改状态
- 补日期
- 补验证方式
- 补失败点

不要只在 commit message 里记录，否则很快会失去整体视图。

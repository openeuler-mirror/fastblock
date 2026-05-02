# fastblock Image Snapshot/Clone 开发规划书

日期：2026-05-02  
分支：`addsnapshot`

## 1. 规划目标

本规划书用于把 fastblock 的 image snapshot/clone 能力拆成可执行任务，覆盖：

- 原生 image snapshot
- protect/unprotect
- clone
- flatten
- qemu/vhost 验证
- 后续 CSI 复用入口

总体原则：

- 先做 image 原生能力，再做 CSI 适配
- 先做离线 MVP，再做在线 crash-consistent
- 尽量复用现有 `libblk_client`、`monclient`、`object_store`
- 尽量避免第一阶段重写 OSD 常规读路径
- 所有实现必须对齐 fastblock 现有的多核 run-to-complete 框架

## 1.1 框架级硬约束

snapshot/clone 开发过程中，必须把以下约束当成硬约束，而不是优化建议：

- 不引入全局大锁来暂停整个 image 或整个 OSD
- 不在数据路径同步阻塞等待跨 core 结果
- 不在热路径引入高争用共享可变状态
- 不绕过 PG/raft 边界直接在 leader 本地做 snapshot 状态修改
- 不把大批量对象处理塞进单个长耗时 callback

推荐做法：

- 控制面编排与数据面执行分离
- 通过 image `snap_seq` 建立 snapshot create 与后续写入的顺序边界
- 通过普通复制写路径携带 `snap_context`，在写时按需触发 object COW
- 通过 shard 本地异步迭代处理对象集合
- clone fallback/copy-up 采用 continuation/阶段机推进
- 所有跨对象、跨 PG 的收敛通过 completion 聚合完成

这条约束会影响所有里程碑，尤其是：

- M2：object_store 的持久 snapshot 底座修正
- M3：快照边界与写时 lazy COW
- M4：clone fallback/copy-up
- M6：在线 freeze/drain/unfreeze

## 2. 里程碑总览

### M0 设计冻结

目标：

- 冻结首版能力边界
- 冻结 metadata 模型
- 冻结 MVP 先后顺序

输出：

- 设计草案评审通过
- 开发计划与分工确认

### M1 元数据与协议骨架

目标：

- 打通 image/snapshot/clone 的 metadata 模型
- 打通 monitor 北向协议

输出：

- monitor 支持 snapshot/clone metadata RPC
- monclient 支持新请求
- 基础 CLI/工具能查询和操作 metadata

### M2 持久 snapshot 底座修正

目标：

- 修正 object_store 的 snapshot 生命周期问题
- 增加持久 snapshot 读取能力

输出：

- `stop()` 不再删除业务 snapshot
- 可按 `snap_name` 读取 object snapshot
- 重启后 snapshot blob 仍可见

### M3 快照边界与写时 lazy COW

目标：

- 为 image 建立 `snap_seq` 边界
- 扩展普通写路径的 `snap_context`
- 支持对象在 snapshot 之后首次写入时按需 COW

输出：

- snapshot create 成为轻量 metadata 操作
- 后续首次写入可按需保留 object 历史版本

### M4 snapshot 版本读与 clone MVP

目标：

- 支持按 snapshot 版本读取 object
- 基于 protected snapshot 创建 clone image
- 读路径 parent fallback
- 写路径首次 copy-up

输出：

- clone 能作为普通块卷读写
- qemu/vhost 能消费 clone image

### M5 flatten 与删除约束

目标：

- flatten 打通
- protect/unprotect、child_count、delete guard 完整

输出：

- parent-child 生命周期可闭环

### M6 在线 crash-consistent snapshot

目标：

- 引入 image 级 freeze/drain/unfreeze 机制
- 支持在线 snapshot

输出：

- 单写 image 的在线 crash-consistent snapshot

### M7 CSI 适配

目标：

- 让 CSI 复用原生 snapshot/clone 能力

输出：

- CSI `CreateSnapshot / DeleteSnapshot / ListSnapshots`
- `CreateVolume from snapshot`

## 3. 任务拆分

## 3.1 M0 设计冻结

任务：

- 明确 MVP 只做离线 snapshot + clone + flatten
- 明确在线 snapshot 放到 M6
- 明确 snapshot create 采用 lazy COW 模型，不做全量 object 扫描
- 明确 clone 读写逻辑首版放在 `libblk_client`
- 明确后台维护层引入独立 `image admin orchestrator`
- 明确所有实现必须遵守多核 run-to-complete 约束

涉及模块：

- `docs/image_snapshot_clone_design.md`

验收：

- 设计评审通过
- 并发模型评审通过

## 3.2 M1 元数据与协议骨架

### 3.2.1 monitor config 与 key 前缀

任务：

- 新增 image snapshot/clone 相关 etcd key 前缀

涉及文件：

- `monitor/config/config.go`

### 3.2.2 monitor metadata 结构

任务：

- 定义 `ImageMetadataV2`
- 定义 `SnapshotMetadata`
- 定义 `ImageOperationRecord`
- 实现 etcd 存取逻辑

建议新增文件：

- `monitor/images/meta.go`
- `monitor/images/store.go`

或在现有 `monitor/osd/image.go` 邻近拆分。

### 3.2.3 monitor 北向协议

任务：

- 新增 snapshot/clone/flatten/protect/unprotect 相关 protobuf
- 新增 monitor request handler
- 新增错误码

涉及文件：

- `proto/messages.proto`
- `monitor/monitor.go`

### 3.2.4 monclient 扩展

任务：

- 新增 monitor client 请求封装
- 新增响应类型
- 新增状态码转换

涉及文件：

- `src/include/fastblock/monclient/client.h`
- `src/monclient/client.cc`

### 3.2.5 metadata CLI

任务：

- 选择是否继续扩展 Go 版 `monitor/fbclient.go`
- 或新增 C++ 工具只做后续后台维护层

建议：

- M1 先保留 monitor metadata 查询命令
- 真正的后台数据维护命令放到后续 C++ 工具

M1 验收：

- 能创建 snapshot metadata 记录
- 能列出 snapshots
- 能创建 clone metadata 记录但暂不执行业务数据逻辑

## 3.3 M2 持久 snapshot 底座修正

### 3.3.1 object_store 生命周期修正

任务：

- 改造 `object_store::stop()`，不再删除持久 snapshot
- 区分 `recover snapshot` 和 `business snapshot`
- 为 object 引入 snapset-like 元数据或等价版本链模型
- 定义 `object_version / object_snapset / object.birth_seq / object.last_cow_seq`
- 确认改造不引入跨 core 共享热锁或阻塞式 stop 路径

涉及文件：

- `src/localstore/object_store.cc`
- `src/localstore/object_store.h`

### 3.3.2 snapshot 读取能力

任务：

- 新增 `read_snapshot(object_name, snap_name, ...)`
- 允许从 `snap_list` 中按名查找 blob 并读取
- 后续可演进为按 `snap_id/snap_seq` 的统一版本读取接口
- 明确 `cover_until_seq` 的持久化方式与重启恢复方式

涉及文件：

- `src/localstore/object_store.cc`
- `src/localstore/object_store.h`

### 3.3.3 snapshot 装载与重启恢复验证

任务：

- 校验 `blob_manager` 加载 snapshot blob 后对象结构正确
- 增加专门测试，覆盖 stop/restart/reload

涉及文件：

- `src/localstore/blob_manager.cc`
- `src/localstore/demo/...`
- `src/test/...`

M2 验收：

- OSD 正常 stop/start 后，业务 snapshot 不消失
- object 可以维护业务 snapshot 历史版本结构
- 能通过对象名 + snapshot 版本读取数据

## 3.4 M3 快照边界与写时 lazy COW

### 3.4.1 image `snap_seq` 与 snapshot metadata

任务：

- 为 image 引入单调递增的 `snap_seq`
- `CreateImageSnapshot` 创建时只推进 snapshot 边界并写 metadata
- 不在 snapshot create 时扫描全量 object

涉及文件：

- `proto/messages.proto`
- `monitor/...`
- `src/include/fastblock/monclient/client.h`
- `src/monclient/client.cc`

### 3.4.2 扩展普通写路径的 `snap_context`

任务：

- 扩展前端写路径，使普通写请求带上当前 `snap_context`
- 扩展 OSD `write_request` / `write_cmd` / raft write meta
- 保证 follower 在 apply 时也能看到同样的 snapshot 边界信息

涉及文件：

- `proto/osd_msg.proto`
- `src/include/fastblock/client/fb_client.h`
- `src/client/libfblock.cc`
- `src/osd/osd_stm.cc`

### 3.4.3 写时 lazy COW

任务：

- 在普通写入 apply 过程中，判断该 object 是否跨过新的 snapshot 边界
- 若跨过且尚未 COW，则先保留旧版本，再写新 head
- 保证同一个 snapshot 边界下，一个 object 只做一次必要的 COW
- 明确新保留历史版本的 `cover_until_seq` 计算规则
- 明确 object 在老 snapshot 下“不存在/全零”的 `birth_seq` 判定规则

涉及文件：

- `src/osd/osd_stm.cc`
- `src/localstore/object_store.cc`
- `src/localstore/object_store.h`

### 3.4.4 snapshot create 路径校验

任务：

- 验证 snapshot create 是否保持为轻量 metadata 操作
- 验证 snapshot create 不引入全量 object 扫描
- 验证多次 snapshot 间 object 未改写时不会重复 materialize 历史版本

M3 验收：

- 创建 snapshot 时不扫描全量 object
- snapshot 之后第一次写 object 时触发一次正确的 COW
- 在下一个 snapshot 之前，对同一 object 的后续写入不重复 COW

## 3.5 M4 snapshot 版本读与 clone MVP

### 3.5.1 clone metadata

任务：

- `CloneImage` 北向接口
- clone image 创建时写入 `parent_snap_id`
- snapshot `child_count` 自增

涉及文件：

- `proto/messages.proto`
- `monitor/...`
- `src/monclient/...`

### 3.5.2 snapshot 版本读路径

任务：

- 支持按 `image@snap_id` 或等价版本上下文读取 object
- 约定 object 历史版本如何匹配 snapshot 边界
- 实现“找第一个 `cover_until_seq >= target_seq` 的历史版本，否则读 head”的选择逻辑

涉及文件：

- `src/localstore/object_store.cc`
- `src/client/libfblock.cc`

### 3.5.3 libblk_client lineage cache

任务：

- 为 image 增加 parent snapshot 链缓存
- 增加获取 lineage 的 monitor 请求

涉及文件：

- `src/include/fastblock/client/libfblock.h`
- `src/client/libfblock.cc`

### 3.5.4 clone 读 fallback

任务：

- 普通 object read miss 时，转到 parent snapshot object read
- 支持递归 parent 链
- 不存在时返回零块
- fallback 过程必须保持异步 continuation 风格

涉及文件：

- `src/client/libfblock.cc`

### 3.5.5 partial write copy-up

任务：

- 当 child object 不存在且写入不是整对象覆盖时：
  - 先读 parent snapshot 完整对象
  - patch 用户写入
  - 再写入 child object
- 整个 copy-up 过程必须拆成非阻塞阶段机

涉及文件：

- `src/client/libfblock.cc`

### 3.5.6 bdev/vhost 验证

任务：

- `fastblock-vhost` 打开 clone image
- QEMU 读写 clone image 正确

涉及文件：

- `src/bdev/bdev_fastblock.cc`
- `docs/qemu_vhost_test.md`

M4 验收：

- 能按 snapshot 版本正确读取 object
- clone image 可读到 parent snapshot 数据
- clone partial write 后，未覆盖区仍正确
- qemu 启动 clone 磁盘可正常工作

## 3.6 M5 flatten 与删除约束

### 3.6.1 flatten 流程

任务：

- 枚举 clone 缺失对象
- 读取 parent snapshot 并物化
- 更新 clone metadata，解除 parent 链
- 引入独立的后台维护组件承担 flatten 执行

涉及文件：

- `src/tools/image_admin/...`
- `monitor/...`
- `src/client/libfblock.cc`

### 3.6.2 删除约束

任务：

- snapshot delete 检查 protect/child_count
- image delete 检查 snapshots/children
- clone delete 后 child_count 正确回收

涉及文件：

- `monitor/...`
- `proto/messages.proto`

### 3.6.3 数据回收

任务：

- 设计并实现基于 snapshot/clone 引用关系的对象历史版本 GC 规则
- 明确 image 删除、snapshot 删除、flatten 后的对象清理责任
- 结合 `cover_until_seq`、snapshot 可见集和 clone 依赖做保守回收

注意：

- 当前 image 删除路径只删 monitor metadata 的迹象很强
- 这里必须补完，不然 snapshot/clone 会积累悬挂对象

M5 验收：

- flatten 后可安全删除 parent snapshot
- child_count/protect/delete guard 均正确
- image 删除不再留下不可控数据垃圾

## 3.7 M6 在线 crash-consistent snapshot

### 3.7.1 image 级使用状态

任务：

- 为 image 增加使用状态或 attachment/lease 语义
- 至少知道 image 当前是否被某个前端持有

涉及模块：

- monitor metadata
- bdev/vhost frontend

### 3.7.2 freeze/drain 协议

任务：

- 设计 image 级 freeze 请求
- 阻止新写入进入
- 等待 in-flight IO drain
- snapshot 完成后 unfreeze
- freeze 必须实现为 gate + drain，而不是全局大锁或阻塞 worker

建议首版范围：

- 仅支持单 writer image

### 3.7.3 qemu/vhost 联动

任务：

- 评估是否需要 QMP 层 guest fsfreeze 集成

建议：

- 首版只承诺 crash-consistent
- guest-aware 作为后续增强

M6 验收：

- 在线单写 image snapshot 可稳定成功
- snapshot 前后数据一致性满足 crash-consistent 预期

## 3.8 M7 CSI 适配

任务：

- 将现有 CSI snapshot 骨架接回原生后端
- 对接 external-snapshotter
- 打通 `CreateVolume from snapshot`

涉及文件：

- `csi/pkg/controller/...`
- `csi/pkg/monitorclient/...`

M7 验收：

- K8s 中可创建 VolumeSnapshot
- PVC 可从 snapshot 恢复

## 4. 推荐模块分工

### 4.1 monitor / metadata

职责：

- 元数据模型
- etcd key 设计
- 北向 snapshot/clone API
- 幂等和操作记录

### 4.2 localstore / OSD

职责：

- 持久 snapshot blob 语义
- object snapset/version chain
- 写时 lazy COW
- snapshot 版本读能力

### 4.3 image admin orchestrator

职责：

- flatten
- GC
- repair
- 后台数据维护任务编排

### 4.4 libblk_client / bdev

职责：

- clone lineage cache
- fallback read
- copy-up write
- qemu/vhost 验证

### 4.5 CSI

职责：

- 最后接入，不作为前置依赖

## 5. 测试规划

## 5.1 单元测试

覆盖：

- snapshot metadata store
- protect/unprotect 约束
- lineage 构建
- copy-up 合并逻辑

## 5.2 组件测试

覆盖：

- object snapshot 历史版本读
- 写时 lazy COW
- stop/restart 后 snapshot persistence
- clone fallback
- flatten

## 5.3 集成测试

覆盖：

- monitor + osd + vhost 全链路
- 创建 image -> snapshot -> clone -> qemu 挂载读写
- snapshot delete guard
- flatten 后 parent 删除

## 5.4 故障测试

覆盖：

- snapshot create 与后续第一次写入之间的恢复
- flatten/GC 执行中断后的重试
- OSD 重启后 snapshot 元数据与 object 历史版本一致性

## 5.5 性能测试

覆盖：

- clone cold read fallback 额外开销
- copy-up 首写延迟
- flatten 吞吐与时长

## 6. 风险与缓解

### 6.1 风险：MVP 范围失控

缓解：

- 明确 M4 前不做在线 snapshot
- 明确 M4 前不做 rollback

### 6.2 风险：语言边界导致控制面难产

缓解：

- snapshot create 首版保持为 metadata + 边界操作
- orchestrator 聚焦 flatten/GC/repair，而不是成为 snapshot create 的前置依赖

### 6.3 风险：GC 闭环不完整

缓解：

- 在 M5 前不宣称删除闭环已完成
- 把 object 回收列为显式任务，不作为隐含行为

### 6.4 风险：clone 写语义出错

缓解：

- 强制补 partial write copy-up 测试
- 做 clone-of-clone 测试

## 7. 粗略工期评估

基于当前代码现状，保守估计：

- M0-M1：1 到 2 周
- M2：1 到 2 周
- M3：2 周
- M4：2 到 3 周
- M5：1 到 2 周
- M6：2 周
- M7：1 周

总计：

- 原生离线 MVP：约 6 到 9 周
- 含在线 snapshot 与 CSI：约 9 到 12 周

前提：

- 有至少 1 名熟悉 OSD/raft/localstore 的开发
- 有至少 1 名能处理 monitor/metadata 和前端接入的开发

## 8. 推荐执行顺序

严格建议按以下顺序推进：

1. M0 设计冻结
2. M1 metadata 骨架
3. M2 持久 snapshot 底座修正
4. M3 快照边界与写时 lazy COW
5. M4 snapshot 版本读与 clone MVP
6. M5 flatten 与 GC
7. M6 在线 snapshot
8. M7 CSI

不要反过来先做 CSI。

原因很简单：

- `qemu/vhost` 是更核心的消费场景
- clone 数据路径主要在底层 image 语义，不在 CSI
- 先做 CSI 只会制造“接口看似有了，底层能力其实不存在”的假进度

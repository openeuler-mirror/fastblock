# fastblock Image Snapshot/Clone 设计草案

日期：2026-05-02  
分支：`addsnapshot`

## 1. 背景

fastblock 当前已经具备以下基础能力：

- monitor 侧 image 元数据管理：`create / get / resize / delete`
- OSD 侧对象读写与 raft 复制
- SPDK bdev 前端，已服务于 `qemu + vhost`、`nvmf` 等块设备消费路径
- object_store 底层具备 SPDK blob snapshot 原语

但当前系统还不具备“用户可见的卷快照/克隆能力”。

现状里的几个关键事实：

1. image 北向协议只有 create/get/resize/delete，没有 snapshot/clone
   - `proto/messages.proto`
   - `monitor/osd/image.go`

2. 现有 snapshot 主要是两类：
   - object 级 snapshot 原语：`src/localstore/object_store.h`
   - raft recovery snapshot：`docs/raft_recovery_and_snapshot.md`

3. 块读写路径是“image 按 object_size 切成多个对象，再按对象名直接访问”
   - 对象名前缀来自 `pool_id + image_name`
   - `src/client/libfblock.cc`

4. 当前 OSD 锁是按 object 加锁，不是按 image 加锁
   - `src/osd/osd_stm.h`

5. 当前 `object_store::stop()` 会删除 snapshot，这与持久 snapshot 语义冲突
   - `src/localstore/object_store.cc`

因此，如果目标是“像 RBD 一样的块存储快照/克隆能力”，这必须设计为 `image 原生能力`，并同时服务：

- `qemu + vhost`
- `nvmf`
- 后续 `CSI snapshot / clone`

而不能先做成某个前端的特化能力。

## 2. 目标

本设计的目标是定义一套 fastblock 原生 image snapshot/clone 能力，使其能够被所有块前端复用。

第一阶段目标：

- 支持 image snapshot `create / list / delete`
- 支持 `protect / unprotect`
- 支持基于 snapshot 的 `clone`
- 支持 clone 的读回源与首次写 copy-up
- 支持 `flatten`
- 支持重启后元数据和 snapshot blob 保持一致
- 能被 `qemu + vhost` 场景消费

第二阶段目标：

- 支持在线 crash-consistent snapshot
- 支持北向 CSI snapshot/restore/clone
- 支持更完整的 GC、可观测性和故障恢复

非目标：

- 第一阶段不做 guest-aware application-consistent snapshot
- 第一阶段不做 rename
- 第一阶段不做增量导出/差异同步
- 第一阶段不做多写共享卷语义

## 3. 目标语义

### 3.1 image

image 是对外暴露的块卷对象，具备：

- 唯一标识 `image_id`
- 名称 `pool_name + image_name`
- 容量 `size`
- 对象粒度 `object_size`
- 生命周期状态

### 3.2 snapshot

snapshot 是某个 image 在某一时刻的只读视图。

建议语义：

- 默认语义是 `crash-consistent`
- snapshot 隶属于一个 source image
- snapshot 可被 protect
- 有 clone child 的 snapshot 必须先 protect，不能直接删除

### 3.3 clone

clone 是基于某个 snapshot 派生的新 image。

建议语义：

- clone 必须来自一个 `protected snapshot`
- clone 初始不拷贝所有数据
- clone 读不到本地对象时，回退到 parent snapshot
- clone 首次写对象时发生 copy-up
- flatten 后 clone 与 parent snapshot 解耦

### 3.4 flatten

flatten 把 clone 对 parent snapshot 的逻辑依赖物化为自己的对象集。

建议语义：

- flatten 完成后，child image 读路径不再访问 parent snapshot
- flatten 完成后，可解除 parent-child 依赖

### 3.5 delete 约束

- snapshot 未 protect、且无 child clone 时才允许删除
- image 若仍有 snapshot 或 child 依赖，删除必须拒绝
- flatten 前，不允许删除被 clone 依赖的 parent snapshot

## 4. 当前实现评估

### 4.1 image 元数据层

当前 image 元数据模型非常薄，只包含：

- `imageID`
- `imagename`
- `poolname`
- `imagesize`
- `objectsize`

对应位置：

- `monitor/osd/image.go`
- `proto/messages.proto`

问题：

- 没有 snapshot 元数据
- 没有 parent-child 图
- 没有 state/protect/refcount
- 没有 image 使用状态或 snapshot 事务状态

### 4.2 数据路径

当前块 IO 路径如下：

1. 前端以 `(pool_id, image_name, offset, len)` 发起读写
2. `libblk_client` 将 image 拆成 object 序列
3. object name 使用固定前缀：
   - `pool_id + "__blk_data___" + image_name + seq`
4. OSD 按 object_name 写入或读取

对应位置：

- `src/client/libfblock.cc`
- `src/osd/osd_stm.cc`

这意味着：

- 当前系统没有显式的 image object manifest
- image 的对象集合只能通过“命名规则 + 现存对象”推导
- clone 读回源和 copy-up 需要额外的 lineage 语义

### 4.3 object snapshot 原语

当前 `object_store` 已支持：

- `snap_create`
- `snap_delete`

对应位置：

- `src/localstore/object_store.h`
- `src/localstore/object_store.cc`

这是非常重要的底座，但当前不足之处也很明显：

1. 只有“对单个 object 建 snapshot”的原语，还没有 image 级 `snap_seq/snapset` 语义
2. 只有 `recovery_read`，没有对业务 snapshot 的通用版本读路径
3. `object_store::stop()` 仍会删 snapshot，违反持久 snapshot 语义
4. 当前普通写路径还不具备“看到 snapshot 边界后按需 COW”的能力

### 4.4 OSD 协议层

当前 OSD RPC 只有：

- `process_write`
- `process_read`
- `process_delete`

对应位置：

- `proto/osd_msg.proto`

当前 raft apply 只识别：

- `RAFT_LOGTYPE_WRITE`
- `RAFT_LOGTYPE_DELETE`

对应位置：

- `src/osd/osd_stm.cc`
- `src/raft/raft.h`

这意味着：

- 当前写请求和写日志里没有 `snap_context / snap_seq` 这类版本信息
- 普通写入在 apply 时还无法确定“这次写是否需要先做 object COW”
- 还没有“按 snapshot 版本读取 object”的通用协议与执行路径

### 4.5 控制面语言边界

当前架构存在一个实际约束：

- monitor 控制面在 Go
- OSD/前端 RDMA 客户端在 C++

monitor 当前并不具备直接调用 OSD RDMA 管理 RPC 的能力。  
因此，如果后续需要后台数据维护能力，例如 flatten、GC、repair，“monitor 直接执行这些数据面编排”并不是低成本改动。

这个约束决定了：

- snapshot create 首版应尽量做成 metadata + 顺序边界操作
- 后台数据维护能力更适合由独立的 C++ 组件承担，而不是强行让 Go monitor 直接改造成 OSD 控制客户端。

## 5. 推荐总体架构

推荐将 image snapshot/clone 分成三层：

### 5.1 元数据层

职责：

- 管理 image / snapshot / clone / flatten 的元数据
- 维护 parent-child graph
- 维护保护状态、引用状态、流程状态
- 提供北向查询与幂等语义

实现位置：

- monitor
- etcd
- monitor client protocol

### 5.2 快照边界与上下文传播层

职责：

- 管理 image 的 `snap_seq / snap_id`
- 建立 snapshot create 与后续写入之间的顺序边界
- 将 `snap_context` 传递给普通写入路径
- 在在线场景下负责 gate / drain / unfreeze 的控制

推荐实现：

- monitor metadata 负责 snapshot 边界与元数据提交
- `libblk_client`、bdev、vhost 等前端负责刷新并携带 `snap_context`

原因：

- 卷级 snapshot 更像 image 级逻辑边界，而不是创建时全量 materialize 的对象操作
- 只要写路径带上正确的 `snap_context`，后续 object COW 可以按需发生

### 5.3 lazy COW 数据路径层

职责：

- 基于 `snap_context` 的对象级按需 COW
- snapshot 版本读
- clone 的 parent fallback
- clone 的 copy-up
- flatten 的数据物化

推荐拆分：

- 普通写路径在看到 snapshot 边界后，按需在 OSD/object_store 中触发 object COW
- object_store 维护 head + 历史版本 / snapset-like 语义
- clone fallback/copy-up 优先落在 `libblk_client`

这样做的原因：

- `qemu/vhost`、`nvmf`、SPDK bdev 等前端都经过 `libblk_client`
- 把 clone 逻辑先放在 `libblk_client`，可以尽量避免第一阶段重写 OSD 常规读路径
- snapshot create 本身不需要在创建时扫描所有 object，更符合多核 run-to-complete 模型

后台维护建议：

- 新增一个 C++ `image admin orchestrator`
- 第一阶段主要承担 flatten、GC、repair 等后台维护任务
- 不把它当成 snapshot create 的前置执行器

### 5.4 与多核 run-to-complete 框架对齐的实现约束

fastblock 当前不是传统的“共享内存 + 大锁 + 阻塞等待”程序模型。  
在 snapshot/clone 设计里，必须把下面这些现有框架特征当成硬约束：

- 多 core 并发执行
- core 之间存在受控 sharing
- 依赖现有 shard / PG / thread 归属来组织执行
- 主要遵循高级的 run-to-complete 风格

因此，这个项目不能按普通多线程存储程序的思路实现。

#### 5.4.1 明确禁止的实现方式

以下做法应明确禁止：

- 引入全局大锁来冻结整个 image 或整个 OSD 进程
- 在数据路径中同步阻塞等待跨 core 结果
- 在热路径引入高争用共享可变状态
- 用单线程中心化扫描直接跨 core 改所有对象
- 在单个 callback 或单次 `apply` 中塞入不可控的大批量长耗时工作

#### 5.4.2 snapshot create 与写时 COW 的实现约束

snapshot create 与后续写时 COW 必须满足：

- snapshot create 首版不做跨 PG 全量 object 扫描
- snapshot create 只建立 metadata 边界与 `snap_seq`
- 真正的 object snapshot/COW 在后续普通写入路径中按需触发
- 每个 shard 只处理自己负责的对象写入和对象版本变化
- 如果后续引入后台 flatten/GC，它们应拆成受控异步迭代，而不是单个长循环

这意味着：

- 不应把卷 snapshot 设计成“创建时对全 PG 对象直接 `snap_create` 一遍”
- 普通写入日志或写命令必须携带足够的 `snap_context`
- apply 过程必须能够在写入前确定是否要先做一次 object COW

#### 5.4.3 clone 读写路径的实现约束

clone 的 parent fallback 和 copy-up 放在 `libblk_client` 的前提下，也必须保持 run-to-complete 风格：

- 读 miss 后的 parent fallback 应写成明确的异步 continuation
- partial write copy-up 不能写成阻塞式“先同步读父，再同步写子”
- copy-up 应拆成阶段机：
  - 检查 child object 是否存在
  - 不存在则异步读取 parent snapshot object
  - 读取完成后 patch buffer
  - 再异步写入 child object
- 每个阶段都应在当前归属线程完成后，以回调推进下一阶段

#### 5.4.4 元数据与 cache 的实现约束

lineage、snapshot 状态、child 关系等数据可以缓存，但要满足：

- 热路径缓存尽量只读
- 可变状态的权威源仍在 monitor metadata
- 不允许多个 core 在数据路径直接争抢更新同一份共享热结构
- cache miss 的查询与刷新要有明确失效策略

第一阶段建议：

- 把 lineage cache 视为前端本地只读缓存
- 把冲突控制和最终一致性留在 monitor metadata 与 operation record

#### 5.4.5 在线 snapshot 的实现约束

如果后续进入在线 crash-consistent snapshot，freeze 也不能做成粗暴的大锁模型。

正确方向应是：

- 在 image 前端入口处 gate 新写入
- 等待 in-flight IO drain
- 以非阻塞方式收敛当前执行中的对象写入
- snapshot 完成后再解除 gate

不应做成：

- 卡死整个 worker thread
- 直接阻塞所有 PG 的执行
- 通过跨 core 同步等待来维持“全局暂停”

#### 5.4.6 评审要求

后续所有 snapshot/clone 相关设计和代码评审，都应显式回答下面几个问题：

1. 这段逻辑运行在哪个 core / thread / PG 归属上？
2. 是否引入了阻塞等待？
3. 是否新增了高争用共享可变状态？
4. 是否破坏了现有 run-to-complete 执行假设？
5. 是否能拆成更细粒度的异步阶段机？

如果这些问题答不清楚，该实现就不应进入主线。

## 6. 元数据模型设计

### 6.1 总体原则

- 保留当前 image create/get/resize/delete 兼容性
- 新能力尽量通过新 key 前缀承载
- 使用稳定 ID 建图，不依赖仅靠名称
- 支持幂等恢复

### 6.2 新增 etcd key 前缀

建议新增：

- `/config/image_meta/<image_id>`
- `/config/image_name/<pool_name>/<image_name>`
- `/config/image_snaps/<image_id>/<snap_id>`
- `/config/image_snap_name/<image_id>/<snap_name>`
- `/config/image_children/<snap_id>/<child_image_id>`
- `/config/image_ops/<op_id>`

说明：

- `image_meta` 存 image 主元数据
- `image_name` 做 name -> id 索引
- `image_snaps` 存 snapshot 主元数据
- `image_snap_name` 做 snapshot name -> id 索引
- `image_children` 可显式记录 child 关系，方便 delete/flatten 校验
- `image_ops` 用于幂等和故障恢复

### 6.3 image 元数据建议字段

建议定义新的 `ImageMetadataV2`：

- `image_id`
- `pool_id`
- `pool_name`
- `image_name`
- `size`
- `object_size`
- `features`
- `status`
  - `ready`
  - `creating`
  - `flattening`
  - `deleting`
- `parent_snap_id`
  - 普通 image 为空
  - clone image 指向其 parent snapshot
- `depth`
- `created_at`
- `updated_at`
- `generation`

### 6.4 snapshot 元数据建议字段

建议定义 `SnapshotMetadata`：

- `snap_id`
- `snap_name`
- `source_image_id`
- `source_pool_id`
- `source_pool_name`
- `source_image_name`
- `status`
  - `creating`
  - `ready`
  - `deleting`
- `protected`
- `object_prefix`
- `created_at`
- `op_id`
- `child_count`

### 6.5 operation 元数据

建议定义 `ImageOperationRecord`：

- `op_id`
- `op_type`
  - `create_snapshot`
  - `delete_snapshot`
  - `protect_snapshot`
  - `unprotect_snapshot`
  - `clone_image`
  - `flatten_image`
- `target_id`
- `status`
  - `pending`
  - `running`
  - `committing`
  - `done`
  - `failed`
- `error`
- `started_at`
- `updated_at`

用途：

- 幂等重试
- 故障恢复
- 审计

## 7. snapshot 设计

### 7.1 第一阶段一致性语义

建议把 snapshot 分成两个阶段交付：

#### 阶段 A：离线 snapshot

要求：

- image 不在使用中
- 或 operator 明确保证无写入

优点：

- 实现复杂度显著更低
- 便于快速验证 metadata + object snapshot 路径

#### 阶段 B：在线 crash-consistent snapshot

要求：

- 单写语义成立
- 前端支持 freeze / drain / unfreeze
- snapshot 期间写入有明确阻塞边界

在线 snapshot 不建议作为第一个里程碑。

### 7.2 snapshot create 流程

推荐流程：

1. 北向校验：
   - image 存在
   - snapshot name 未冲突
   - image 状态允许

2. 写入 operation record，状态 `pending`

3. 若为在线 snapshot，则先在前端入口处 gate 新写入，并等待 in-flight IO drain  
   若为离线 snapshot，则校验当前无写入者或由 operator 保证静默

4. 为 image 分配新的 `snap_id` 与单调递增的 `snap_seq`

5. 持久化 snapshot 元数据，并推进 image 当前 head 的 snapshot 边界

6. 清除 gate，允许后续写入继续进入  
   这些写入必须在普通写路径上携带新的 `snap_context`

7. 创建完成后，snapshot 可立即进入 `ready` 状态  
   此时并不会扫描现有 object，也不会立即为每个 object 建 snapshot 版本

8. operation record 标记 `done`

### 7.3 为什么不建议做成创建时全量 materialize

因为卷级 snapshot 的正确模型不是：

- 创建 snapshot 时立刻扫描所有 object
- 给每个 object 都建一个同名 snapshot blob

而是：

- image 创建一个新的 snapshot 边界
- 后续对象第一次被改写时，才按需保留旧版本

如果对象在多个 snapshot 之间从未被改写，则没有必要为每个 snapshot 单独立即 materialize 一份 object snapshot。  
这正是 lazy COW 模型能显著降低 snapshot create 成本的原因。

### 7.4 顺序边界与写时 COW

虽然 snapshot create 本身不需要全量扫 object，但仍然必须解决“snapshot 与后续写入之间的顺序边界”问题。

推荐方式：

- image 元数据维护单调递增的 `snap_seq`
- 普通写请求或写日志必须携带当前 `snap_context`
- `osd_stm::apply` 在执行普通写入时，根据对象本地版本状态与 `snap_context` 判断是否需要先做一次 COW

核心要求：

- 这次写如果跨过了某个新的 snapshot 边界，并且该 object 自那之后还没有做过 COW
- 那么在写入 head 之前，必须先把旧版本保留下来
- 如果一个 object 在多个 snapshot 之间始终没有被改写，则不应为每个 snapshot 单独生成一份历史版本
- 一份被保留下来的旧版本应能够通过 snapset-like 元数据服务多个尚未被该 object 改写穿越的 snapshot 边界

这样：

- snapshot create 仍然是轻量 metadata 操作
- object 历史版本的真正落盘由普通写入路径按需触发
- follower 与 leader 的行为仍然一致，因为决定 COW 的依据随写入路径一起复制

## 8. snapshot delete / protect 设计

### 8.1 protect / unprotect

建议语义：

- clone 只能从 `protected snapshot` 创建
- 存在 child 时，不允许 unprotect
- unprotect 后若无 child，可删除

### 8.2 delete snapshot

建议流程：

1. 校验 snapshot 存在
2. 若 `protected == true`，拒绝
3. 若 `child_count > 0`，拒绝
4. 置 snapshot 状态为 `deleting` 或 `deleted_pending_gc`
5. 将该 snapshot 从 image 的可见 snapshot 集中移除
6. 后台 GC 在确认没有 object 历史版本再被任何 snapshot/clone 引用后，回收对应版本数据
7. GC 完成后删除 snapshot 元数据与索引

## 9. clone 设计

## 9.1 为什么 clone 不建议先放进 OSD 读路径

当前 OSD 普通读路径只有：

- 按 object_name 读取 blob

OSD 不理解 image lineage，也没有 image 级上下文。  
如果把 clone fallback 直接塞进 OSD，会涉及：

- object_name 反解析
- image lineage 查询
- snapshot 链访问
- metadata 缓存一致性

第一阶段成本过高。

更务实的方案：

- OSD 继续只做 object 存储
- `libblk_client` 负责 clone lineage 感知

这样 `qemu/vhost`、`nvmf`、bdev 前端可直接受益。

### 9.2 clone 元数据

创建 clone image 时：

- 创建新 image 元数据
- `parent_snap_id = <source_snap_id>`
- `depth = parent.depth + 1`

clone image 自己的 object namespace 仍是自己的 `pool_id + image_name` 前缀。

第一阶段建议保留当前 object naming，不同时引入 object namespace 迁移。

代价：

- rename 仍然不能支持
- lineage 依赖 image_name/object_prefix

好处：

- 不影响现有 image 数据格式
- 可降低首版实现风险

### 9.3 clone 读路径

读路径建议放在 `libblk_client::read`：

1. 先读 child object
2. 若 object 存在，返回 child data
3. 若 object 不存在：
   - 查询 image lineage cache
   - 找到 parent snapshot
   - 读取 parent snapshot 对应 object
4. 若 parent snapshot 也没有对象，则继续向上递归
5. 最终都不存在时返回零块

需要新增：

- image lineage cache
- 通用 `read_snapshot_object` 路径

### 9.4 clone 写路径和 copy-up

写路径建议放在 `libblk_client::write`：

1. 计算目标 object
2. 若 child object 已存在，按现有路径写
3. 若 child object 不存在：
   - 如果是整对象覆盖写，可直接创建 child object
   - 如果是部分对象写，必须先从 parent snapshot 读出完整对象
   - 将写入区域 patch 到完整对象 buffer
   - 再把完整对象写入 child object

这是 clone 的核心复杂点之一。

原因：

- 当前不存在对象时，直接 partial write 会把未写部分当作零
- 对 clone 而言，未写部分应该继承 parent snapshot 数据

## 10. flatten 设计

flatten 建议由 orchestrator 驱动，读写数据仍经 `libblk_client`：

1. 枚举 image 空洞对象
2. 对每个缺失 object：
   - 从 parent snapshot 链读出完整对象
   - 物化到 child image
3. 全部完成后：
   - 更新 image 元数据，清空 `parent_snap_id`
   - 更新 child depth
   - 解除 snapshot child_count

flatten 第一阶段可以允许：

- 前台执行
- 单线程或低并发执行

后续再优化后台任务化与限速。

## 11. object_store 与 OSD 必须修改的点

### 11.1 object_store

必须修改：

1. `stop()` 不再删除持久 snapshot
2. 增加业务 snapshot 的通用版本读取能力
3. 为 object 增加 snapset-like 元数据或等价的版本链语义
4. 增加“写入前按需 COW”的辅助能力
5. 明确 `recover snapshot` 与 `persistent snapshot` 的存储隔离

建议做法：

- 保留 `recover` 字段仅服务 raft recovery
- `snap_list` 或其后继结构用于表达业务 snapshot 历史版本
- 为业务 snapshot 增加按 `snap_id/snap_seq` 读取的接口
- 记录 object 最近一次已处理的 snapshot 边界，避免重复 COW

#### 11.1.1 推荐逻辑模型

从卷级 snapshot/clone 语义出发，localstore 这一层不应被理解成：

- 一个 object 对应很多“彼此独立的完整 snapshot”

更准确的模型应是：

- 一个 object 有一个当前 `head`
- 再加 `0..N` 个历史版本
- 历史版本共同服务多个 snapshot 边界

也就是说，localstore 里维护的是 `object version chain / snapset`，而不是“snapshot 数量 = object 历史副本数量”。

推荐逻辑结构：

```cpp
struct object_version {
    fb_blob blob;
    uint64_t cover_until_seq;
};

struct object_snapset {
    uint64_t birth_seq;
    uint64_t last_cow_seq;
    std::list<object_version> versions; // 按 cover_until_seq 升序
};

struct object {
    fb_blob origin;   // 当前 head
    fb_blob recover;  // 仅服务 raft recovery
    object_snapset snapset;
};
```

字段含义建议如下：

- `origin`
  - 当前可写 head

- `birth_seq`
  - object 首次变为可见时对应的 image snapshot 边界
  - 用于判断某些更老 snapshot 下该 object 是否应视为“不存在/全零”

- `last_cow_seq`
  - 当前 head 已经处理到的最新 snapshot 边界
  - 用于避免同一 snapshot 边界下重复 COW

- `versions`
  - object 的历史版本链

- `cover_until_seq`
  - 该历史版本可服务到的“最新 snapshot seq”
  - 一个历史版本可以覆盖多个 snapshot 边界

#### 11.1.2 为什么不是“一个 snapshot 一份 object 副本”

假设：

- 当前 object head 是 `H0`
- 创建 snapshot `S10`
- 创建 snapshot `S20`
- 期间 object 一直没有被写
- 直到 `S20` 之后才第一次改写

这时正确行为不是：

- 为 `S10` 建一份历史 object
- 再为 `S20` 建一份历史 object

而是：

- 只在第一次改写前保留一份旧版本 `V20(H0)`

然后：

- `S10` 读 `V20`
- `S20` 读 `V20`

因此：

- 历史版本数量取决于“跨 snapshot 边界发生了几次改写”
- 而不是取决于 snapshot 总数

#### 11.1.3 历史版本元数据放哪里

这里必须分层放置，不能混。

##### A. 全局 image/snapshot 元数据

放在 `monitor + etcd`：

- image 元数据
- snapshot 元数据
- `snap_id`
- `snap_seq`
- protect/unprotect
- clone parent-child 关系
- flatten 状态

这是控制面元数据。

##### B. object 历史版本元数据

放在 `localstore/blobstore` 本地：

- 该历史版本属于哪个 object
- 对应哪个 `cover_until_seq`
- 该版本的 blob id
- 该 object 的 `birth_seq`
- 该 object 的 `last_cow_seq`

这是数据面元数据。

不建议把 per-object 历史版本链放到 etcd，原因很直接：

- 粒度太细
- 写入太热
- 每次普通写都可能碰到
- 会把控制面存储拖进快路径

#### 11.1.4 localstore 持久化方案建议

第一阶段建议优先走“历史版本 blob 自带元数据 + 启动时重建内存 snapset”的方式。

也就是：

- 历史版本 blob 继续用 blob xattr 表达
- 启动时由 `blob_manager` 扫描 blob 并重建 `object_snapset`

建议给历史版本 blob 增加或演进以下 xattr 语义：

- `type=object_snap`
- `shard`
- `pg`
- `object_name`
- `cover_until_seq`

同时，还需要为当前 object head 维护本地 snapset 元数据，至少包含：

- `birth_seq`
- `last_cow_seq`

首版有两个可选承载位置：

##### 方案 A：挂在 head blob xattr 上

优点：

- 改动较小
- 不需要额外 metadata blob

缺点：

- 后续字段继续膨胀时管理会变乱

##### 方案 B：单独的本地 snapset metadata blob / kv

优点：

- 模型更清晰
- 更利于后续 GC/repair

缺点：

- 首版实现更重

建议：

- 首版优先走 A
- 如果后续 GC、repair、debug 复杂度上来，再演进到 B

#### 11.1.5 snapshot 版本选择规则

按 snapshot 读取某个 object 时，建议采用如下规则：

1. 如果目标 `target_seq < birth_seq`
   - 该 object 在这个 snapshot 中应视为不存在
   - 对块卷语义返回零块

2. 否则，在 `versions` 中找到第一个满足：
   - `cover_until_seq >= target_seq`

3. 如果找到了，就读该历史版本 blob

4. 如果没找到，则读当前 `origin`

这要求：

- `versions` 按 `cover_until_seq` 升序维护
- 历史版本链在重启恢复后仍能稳定重建

#### 11.1.6 写时 COW 规则

普通写入到来时，建议按以下逻辑判定：

1. 取当前写请求携带的 `snap_context.current_seq`

2. 查看该 object 的 `last_cow_seq`

3. 如果：
   - `current_seq > last_cow_seq`
   - 并且这次写会修改当前 head

4. 则在改写 head 之前：
   - 先把旧 head 保留成一个历史版本
   - 该版本的 `cover_until_seq = current_seq`
   - 再把 `last_cow_seq = current_seq`

5. 然后执行对 head 的正常改写

关键约束：

- 同一个 snapshot 边界下，一个 object 只能做一次必要的 COW
- 下一个 snapshot 边界出现之前，对该 object 的后续写入不应重复生成历史版本

#### 11.1.7 clone 场景与本地 snapset 的关系

clone 读 fallback 在第一阶段仍建议放在 `libblk_client`。

但 localstore 的 snapset 仍然有明确职责：

- 服务“同一个 image 自己”的 snapshot 版本读
- 为 clone 的 parent snapshot 读取提供基础 object 版本选择能力

也就是说：

- clone 跨 image 的 parent-child 关系在控制面和前端
- object 的“某个 snapshot 下应该读哪个版本”在 localstore

#### 11.1.8 GC 视角下的约束

历史版本 GC 不应只看“这个 snapshot 是否被删了”，还必须同时看：

- 是否仍有 snapshot 需要这个版本
- 是否仍有 clone 通过 parent snapshot 间接依赖这个版本

因此，GC 的判断条件至少需要结合：

- 全局 snapshot 可见集
- clone parent-child 关系
- object 历史版本的 `cover_until_seq`

首版建议：

- 先做保守 GC
- 宁可晚删，不要错删

### 11.2 OSD 状态机

首版不应把卷 snapshot 设计成“必须新增 snapshot create/delete 独立 log type”的模型。

更核心的改造是：

- 扩展普通写请求 / 写命令 / 写日志，使其携带 `snap_context`
- `osd_stm::apply` 在处理普通写入时，基于 `snap_context` 与对象本地版本状态决定是否先做一次 COW
- 为 snapshot 版本读取增加通用执行路径

后续如果 flatten、GC、repair 等后台维护操作需要独立的复制语义，再评估是否新增专门的维护类 log type。

### 11.3 OSD 管理 RPC

snapshot create/delete 首版不要求新增专门的 PG 级管理 RPC。

更优先的工作是：

- 扩展普通写入协议中的 `snap_context`
- 扩展 snapshot 版本读路径

后台维护类能力，例如：

- flatten
- 历史版本 GC
- 数据修复

后续可由独立的 C++ `image admin orchestrator` 或维护 RPC 承担。

## 12. 北向 API 设计

### 12.1 monitor metadata API

需要在 `proto/messages.proto` 增加：

- `CreateImageSnapshotRequest/Response`
- `DeleteImageSnapshotRequest/Response`
- `ListImageSnapshotsRequest/Response`
- `ProtectImageSnapshotRequest/Response`
- `UnprotectImageSnapshotRequest/Response`
- `CloneImageRequest/Response`
- `FlattenImageRequest/Response`
- `GetImageLineageRequest/Response`

### 12.2 monitor client

需要在 C++ monitor client 增加上述请求封装与响应类型。

### 12.3 qemu/vhost / bdev / nvmf

第一阶段重点：

- bdev 能打开 clone image
- clone read/write 语义正确
- vhost 暴露 clone image 正常

可选增强：

- SPDK RPC 增加 snapshot/clone/flatten 管理命令

### 12.4 CSI

CSI 不应先行。

等原生能力稳定后再开放：

- `CreateSnapshot`
- `DeleteSnapshot`
- `ListSnapshots`
- `CreateVolume from snapshot`
- 后续 `CloneVolume`

## 13. 推荐实施策略

### 13.1 推荐 MVP

推荐把 MVP 定义为：

- 离线 snapshot
- protect/unprotect
- clone
- clone parent fallback
- partial write copy-up
- flatten
- qemu/vhost 验证通过

不把在线 snapshot 放进 MVP。

### 13.2 为什么这样拆

因为真正最难的不是“快照 blob 能不能建出来”，而是：

- 业务语义是否清晰
- `snap_seq` 与普通写入的顺序边界是否正确
- clone 读写是否正确
- 写时 lazy COW 是否稳定
- 重启和故障后 metadata 与对象历史版本是否仍一致

## 14. 主要风险

### 14.1 一致性风险

没有 image 级 fence 的情况下，在线 snapshot 很容易拿到不一致视图。

### 14.2 生命周期风险

当前 `object_store::stop()` 删除 snapshot，必须先修正。

### 14.3 删除与 GC 风险

当前 image 删除似乎只删 monitor 元数据，没有完整的数据对象回收闭环。  
引入 snapshot/clone 后，这个问题会进一步放大。

### 14.4 版本语义风险

如果 `snap_seq`、`snap_context`、object 历史版本之间的关系定义不清，snapshot/clone 很容易出现读错版本的问题。

这是这个项目比“补几个 API”难得多的根因之一。

### 14.5 语言边界风险

Go monitor 与 C++ OSD/RDMA 客户端之间的职责分层不清，会拖慢首版实现。  
建议把 orchestrator 收敛到 flatten/GC/repair 等后台维护任务，而不是让它成为 snapshot create 的前置依赖。

### 14.6 性能风险

clone fallback 放在 `libblk_client` 会带来：

- miss read 多一次父链读取
- partial write 首次 copy-up 变重

这是可接受的第一阶段代价，后续可以再逐步下沉优化。

## 15. 最终建议

建议按以下顺序推进：

1. 先修正 object_store 的持久 snapshot 语义
2. 再补 monitor 元数据与 admin 操作记录
3. 再补 image `snap_seq` 与普通写入的 `snap_context`
4. 再补写时 lazy COW
5. 再补 snapshot 版本读与 clone 的 `libblk_client` fallback/copy-up
6. 再补 flatten、删除约束和后台 GC
7. 先打通 `qemu/vhost`
8. 最后接 CSI

这个顺序最符合当前仓库结构，也最能避免“CSI 先做了，但底层能力仍不存在”的空转。

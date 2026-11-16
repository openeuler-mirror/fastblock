# fastblock Append-Only Block Store 设计草案

日期：2026-08-11

## 1. 背景

fastblock 当前本地对象存储依赖 SPDK blobstore。blobstore 帮我们解决了 blob 分配、元数据、snapshot、重启扫描等问题，但也带来了明显约束：

- raft recovery snapshot 需要对 PG 内每个 object 创建 blob snapshot，恢复启动成本与 object 数量相关。
- image/CSI snapshot 即使采用 `snap_seq` + lazy COW，跨 snapshot 的首次写仍依赖 `spdk_bs_create_snapshot`。
- snapshot 语义被底层 blob snapshot 牵引，PG/image 层的一致性视图需要用 per-object 物理 snapshot 拼出来。
- allocator、GC、version lifecycle、ZNS 支持等关键能力不在 fastblock 自己掌控中。

如果目标是长期支持高质量的 raft recovery、image snapshot/clone、ZNS 设备和可控 GC，需要把本地存储引擎升级为 fastblock 自己管理的 append-only block store。

本设计不是简单把所有写追加到文件末尾，而是定义一个：

```text
对上暴露随机读写对象语义
对内采用 append-only physical layout
通过 versioned extent map 支持 overwrite、snapshot、clone 和 GC
```

的本地存储引擎。

## 2. 目标

第一阶段目标：

- 替代 `object_store` 的核心对象读写能力。
- 原生支持多个 raft group / PG。
- snapshot 创建为轻量 metadata 操作，不扫描 PG 内全部 object。
- install snapshot 可以从稳定 read view streaming 读取。
- 支持 crash recovery，保证不会重复分配、读到脏数据或丢失已提交写。
- allocator 与 GC 从第一版设计进入模型。
- 为 ZNS 设备保留天然映射方式。

长期目标：

- 支持 image snapshot/clone 的 `snap_seq`、copy-up、flatten、GC。
- 支持普通 NVMe 和 ZNS 两种后端。
- 支持后台 scrub、fsck、空间泄漏检测和离线 debug 工具。
- object 数据面完全离开 blobstore，并最终迁出 log/kv，移除 blobstore 依赖。

非目标：

- 首版不追求极致性能。
- 首版不做复杂压缩、EC、跨节点数据编码。
- 首版不做跨 PG 原子事务。
- 首版不直接承诺完整 CSI snapshot/clone 功能，而是先提供底层能力。

## 3. 核心原则

### 3.1 snapshot 是一等公民

snapshot 不是为了 raft recovery 临时遍历 object 拼出来的副产品，而是 store 的原生 read view。

snapshot 创建应该只记录：

- `raft_snapshot_index` 或 `image_snap_seq`
- `snapshot_term`，如果服务 raft snapshot
- 对应 PG/image 的 metadata root 或 checkpoint generation
- 引用计数 / pin 信息

它不应该在创建时扫描全量 object，也不应该对每个 object 创建物理 snapshot。

### 3.2 append-only 是物理布局，不是对外语义

对上仍然暴露块存储需要的随机读写语义：

- 覆盖写
- partial write
- delete/discard
- snapshot read
- recovery streaming

对内每次写入追加新的 physical extent，然后通过 metadata 原子发布新映射。

### 3.3 PG 是一致性边界

fastblock 原生是多 raft group 系统，因此 store 不能按单 PG 产品化。正确模型是：

```text
OSDStore
  |
  +-- PGStore(pool=1, pg=0)
  +-- PGStore(pool=1, pg=1)
  +-- PGStore(pool=2, pg=7)
  |
  +-- SegmentAllocator
  +-- Manifest / Checkpoint
  +-- Device backend
```

逻辑上每个 PGStore 独立承载一个 raft group 的状态机数据。物理上多个 PGStore 可以共享同一个 allocator 和设备空间。

### 3.4 正确性优先于压榨性能

append-only store 的难点不在顺序写，而在：

- metadata 原子性
- allocator crash consistency
- snapshot pin
- GC 正确性
- 重启恢复
- 读放大和尾延迟控制

第一版必须先让这些可验证，再做性能优化。

## 4. 总体架构

```text
raft_server / osd_stm
  |
  | StoreEngine API
  v
OSDStore
  |
  +-- PGStore
  |     |
  |     +-- ObjectMap
  |     +-- VersionMap
  |     +-- SnapshotRegistry
  |
  +-- SegmentAllocator
  +-- SegmentCleaner
  +-- MetadataLog
  +-- CheckpointManager
  +-- DeviceBackend
```

### 4.1 Device layout 与共存策略

这是首版必须先钉死的边界：**append-only store 首版只替换 object 数据面，不同时迁移 raft log 和 kv_store**。

原因：

- `disk_log`、`kv_store`、`object_store` 当前都依赖 blobstore。
- 如果第一步同时接管 log、kv、object，设备格式、恢复路径和故障边界会一次性膨胀。
- 当前最痛的是 object snapshot/recovery 和 image snapshot 的物理版本管理，优先替换 object 数据面收益最大。

首版部署模型建议为：

```text
OSD
  |
  +-- raft log / kv metadata
  |     |
  |     +-- existing blobstore 或独立小 bdev/partition
  |
  +-- object data
        |
        +-- append-only raw bdev/partition
```

append-only object bdev 的物理布局：

```text
+-----------------------------+
| superblock A                |
+-----------------------------+
| superblock B                |
+-----------------------------+
| manifest / checkpoint area  |
+-----------------------------+
| segment table / allocator   |
+-----------------------------+
| data segment 0              |
| data segment 1              |
| ...                         |
+-----------------------------+
```

ZNS 后端可以映射为：

```text
conventional zone 或 reserved zones:
  superblock / manifest / checkpoint

sequential zones:
  data segments
```

首版格式要求：

- superblock 双写，带 `magic/version/generation/checksum`。
- manifest/checkpoint 区域可滚动写入，恢复时选择 generation 最大且 checksum 正确的一份。
- data segment record 必须自描述，checkpoint 只是加速恢复，不是唯一真相来源。
- 每个 record 必须携带 `pool_id/pg_id/store_epoch`，防止 PG 删除重建后旧数据串入新 PG。

中长期可以再评估把 raft log / kv 也迁入同一 StoreEngine，但不作为第一阶段目标。

### 4.2 StoreEngine API

建议先定义稳定接口，再提供 blobstore 和 append-only 两种实现。

```c++
struct store_object_id {
    std::string object_name;
};

struct pg_store_id {
    uint64_t pool_id;
    uint64_t pg_id;
    uint64_t store_epoch;
};

struct store_write_ctx {
    uint64_t raft_apply_index;
    uint64_t image_snap_seq;
};

struct store_read_ctx {
    uint64_t raft_read_index; // 0 means latest head
    uint64_t image_snap_seq;  // 0 means latest head
};

struct store_stat {
    uint64_t used_bytes;
    uint64_t live_bytes;
    uint64_t reclaimable_bytes;
    uint64_t pinned_bytes;
};

class pg_store {
public:
    virtual pg_store_id id() const = 0;

    virtual void write(store_object_id object, uint64_t offset, buffer data,
                       store_write_ctx ctx, store_complete cb) = 0;
    virtual void read(store_object_id object, uint64_t offset, uint64_t len,
                      store_read_ctx ctx, store_read_complete cb) = 0;
    virtual void remove(store_object_id object, store_write_ctx ctx,
                        store_complete cb) = 0;

    virtual void discard(store_object_id object, uint64_t offset, uint64_t len,
                         store_write_ctx ctx, store_complete cb) = 0;
    virtual void flush(store_complete cb) = 0;

    virtual void create_raft_snapshot(uint64_t raft_apply_index,
                                      uint64_t raft_term,
                                      snapshot_complete cb) = 0;
    virtual void create_image_snapshot(uint64_t image_snap_seq,
                                 snapshot_complete cb) = 0;
    virtual void release_raft_snapshot(uint64_t raft_apply_index,
                                       store_complete cb) = 0;
    virtual void release_image_snapshot(uint64_t image_snap_seq,
                                        store_complete cb) = 0;

    virtual void open_raft_snapshot_reader(uint64_t raft_apply_index,
                                      snapshot_reader_complete cb) = 0;
    virtual void iterate_objects(store_read_ctx ctx,
                                 object_iter_complete cb) = 0;
    virtual void stat(stat_complete cb) = 0;
    virtual void close(store_complete cb) = 0;
};

class osd_store {
public:
    virtual void create_pg(pg_store_id id, pg_store_complete cb) = 0;
    virtual void open_pg(pg_store_id id, pg_store_complete cb) = 0;
    virtual void delete_pg(pg_store_id id, store_complete cb) = 0;
};

class snapshot_reader {
public:
    virtual void next_chunk(uint32_t max_objects,
                            uint32_t max_bytes,
                            snapshot_chunk_complete cb) = 0;
    virtual void close(store_complete cb) = 0;
    virtual void cancel(store_complete cb) = 0;
};
```

`pg_store` 实例已经绑定 `pool_id/pg_id/store_epoch`，因此单 object 操作不再重复传 `pool_id/pg_id`。跨 PG 生命周期操作由 `osd_store` 管理。

`store_epoch` 由 OSD 本地 manifest 分配并持久化。`delete_pg()` 后如果同一个 `pool_id/pg_id` 被重建，必须分配更大的 `store_epoch`；replay 时 epoch 不匹配的旧 record 一律忽略，只能由 GC 回收。

`store_read_ctx` 必须满足：

- `raft_read_index == 0 && image_snap_seq == 0`：head read。
- `raft_read_index != 0 && image_snap_seq == 0`：raft snapshot read。
- `raft_read_index == 0 && image_snap_seq != 0`：image snapshot read。
- `raft_read_index != 0 && image_snap_seq != 0`：非法，直接返回 invalid argument。

`iterate_objects` 必须是异步 cursor/batch 模型，不能一次性同步灌完整个 PG。

### 4.3 PGStore

每个 PGStore 维护本 PG 的逻辑状态：

- object namespace
- object extent map
- object version chain
- PG snapshot registry
- PG local checkpoint generation
- raft install snapshot reader

PGStore 的状态只能由对应 PG 的 raft apply 路径推进。后台 GC 可以扫描物理 segment，但是否能回收某个 extent，必须回到 PGStore 的版本引用信息判断。

### 4.4 SegmentAllocator

SegmentAllocator 管理整块设备空间。建议首版采用固定大小 segment：

- 普通 NVMe：segment 是设备上的连续 LBA 区间。
- ZNS：segment 直接映射到 zone，写指针顺序推进，回收时 reset zone。

segment 状态：

- `free`
- `open`
- `sealed`
- `gc_candidate`
- `cleaning`
- `bad`

首版可以限制每个 shard/core 只持有少量 open segment，降低并发 allocator 复杂度。

## 5. 版本时间线

设计里必须区分两条时间线，不能都叫 `seq`。

### 5.1 raft_apply_index

`raft_apply_index` 是 PG 内 raft 状态机的提交/应用时间线。

用途：

- raft install snapshot
- PG recovery read view
- raft log trim 边界
- PG 级 crash recovery 校验

性质：

- 只在单个 PG 内单调。
- 不同 PG 之间不可比较。
- 对应 raft log index。
- 首版 PG snapshot 只使用这条时间线。

### 5.2 image_snap_seq

`image_snap_seq` 是 image/volume snapshot 的控制面时间线。

用途：

- CSI/image snapshot
- clone parent fallback
- image snapshot version read
- flatten/GC 对 clone 依赖的判断

性质：

- 属于 image metadata。
- 一个 image 可能跨多个 PG。
- 需要由前端/控制面把当前 `image_snap_seq` 带入写请求。
- 与 raft index 没有天然顺序关系。

写入语义：

```text
create_image_snapshot(S):
  只 pin S，不复制数据

write(ctx.image_snap_seq = current):
  该写对 image snapshot <= current - 1 不可见
  该写对 head 可见
  如果未来创建 image snapshot >= current，则按后续覆盖关系判断可见性
```

同一个 `image_snap_seq` 内可以有多次写入，`raft_apply_index` 更大的写覆盖同一 logical range 上较早的写。`image_snap_seq = 0` 表示禁用 image snapshot 维度，只写 head；这类写不能被 image snapshot reader 按 `image_snap_seq` 读取，只能参与 head read 或 raft snapshot read。

如果 image 跨多个 PG，store 不做跨 PG 边界校验。控制面和前端必须保证同一 image snapshot boundary 被一致传播到相关 PG；PGStore 只信任写请求携带的 `image_snap_seq`。

### 5.3 record 中的版本字段

data record 首版应同时带：

```text
raft_apply_index
image_snap_seq
store_epoch
```

版本选择规则必须按读上下文区分：

- raft recovery snapshot read：使用 `raft_apply_index`。
- image snapshot read：使用 `image_snap_seq`。
- head read：读取最新 head mapping。

不要用一个通用 `read_seq` 同时表达两类 snapshot。

## 6. 物理布局

### 6.1 Segment

每个 segment 由连续 records 组成：

```text
+----------------+
| segment header |
+----------------+
| record 0       |
| record 1       |
| ...            |
+----------------+
| footer/tail    |
+----------------+
```

record 类型：

- data record
- delete tombstone
- gc rewrite record
- metadata delta
- object-name record
- checkpoint marker
- segment seal marker

### 6.2 Data Record

建议 data record header 包含：

```text
magic
format_version
record_type
header_len
payload_len
pool_id
pg_id
store_epoch
object_id
object_offset
raft_apply_index
image_snap_seq
header_checksum
payload_checksum
flags
```

payload 是实际数据。小写入可以先直接存 payload；后续再考虑聚合、压缩或对齐优化。

首版不建议每条 data record 都写完整 `object_name`。更合理的方式是：

- object 首次出现时追加 object-name record，分配定长 `object_id`。
- 后续 data record 只携带 `object_id`。
- checkpoint 中保存 `object_id -> object_name` 二级索引。
- replay 时如果发现 data record 引用未知 `object_id`，该 segment tail 视为不完整或损坏。

这样能降低小随机写的 record metadata 放大。

### 6.3 Metadata

核心内存结构：

```text
ObjectMap:
  object_id ->
    object_name
    raft_birth_index
    image_birth_seq
    raft_delete_index
    image_delete_seq
    head_extent_tree
    versioned_extent_tree

Extent:
  physical_segment
  physical_offset
  logical_offset
  length
  raft_create_index
  image_create_seq
  checksum

Tombstone:
  logical_offset
  length
  raft_delete_index
  image_delete_seq
```

对同一个 object 的 logical range，head extent map 表示最新版本；historical versions 用来服务 snapshot read。

`versioned_extent_tree` 首版建议采用 interval tree 或 ordered interval map，节点保存同一 logical range 的版本链。不要为每个 snapshot 全量拷贝 object map。

raft 读与 image 读共用同一组 physical extent 节点，但使用不同字段做可见性判断：

```text
raft_visible(extent e, raft_index r):
  e.raft_create_index <= r
  AND no visible tombstone covers e.range with raft_delete_index <= r
  AND no newer extent e2 overlaps e.range with
        e2.raft_create_index <= r
        AND e2.raft_create_index > e.raft_create_index

image_visible(extent e, image_seq s):
  e.image_create_seq != 0
  AND e.image_create_seq <= s
  AND no visible tombstone covers e.range with image_delete_seq <= s
  AND no newer extent e2 overlaps e.range with
        e2.image_create_seq != 0
        AND e2.image_create_seq <= s
        AND (
          e2.image_create_seq > e.image_create_seq
          OR (e2.image_create_seq == e.image_create_seq
              AND e2.raft_create_index > e.raft_create_index)
        )
```

range tombstone 必须和 data extent 一样同时带 `raft_delete_index` 与 `image_delete_seq`。`delete/discard` 不能只记录一个模糊的 delete seq。

GC rewrite record 只搬迁物理位置，不产生新的用户可见版本。它必须保留原 extent 的 `raft_create_index/image_create_seq`，并通过 `record_type` 或 flags 与用户 data record 区分。replay 时，GC rewrite 只能更新 physical pointer，不能被当成新的 raft apply。

首版需要做一个量级预算：

```text
假设单 object 发生 10,000 次 4K overwrite，期间创建 3 个 image snapshot。
如果没有 snapshot pin，旧 extent 可很快被 GC。
如果 3 个 snapshot 都 pin，最坏需要保留 10,000 个历史 extent 记录。
每条 extent metadata 若约 64B，单 object 约 640KB metadata。
```

这个估算说明：

- 频繁小覆盖写必须依赖 extent coalescing/compaction。
- snapshot 长时间存在会放大 metadata 和空间。
- M1/M2 原型必须暴露 per-PG metadata bytes、extent count、average extents per read。

## 7. 写路径

### 7.1 普通写

写入流程：

1. raft entry 已提交，`osd_stm::apply` 调用 PGStore。
2. PGStore 为写入分配 append 空间。
3. 写入 data record 到当前 open segment。
4. fsync/flush 策略按 durability 配置执行。
5. flush 成功后发布内存 ObjectMap。
6. 写入 metadata delta 或等待 checkpoint 聚合，用于加速恢复。
7. 回调 raft apply 完成。

metadata delta 是恢复加速优化，M1 可以先不实现；只要 record 自描述和 checkpoint/replay 正确，功能正确性不依赖 metadata delta。

覆盖写不是原地覆盖，而是：

```text
append new extent
publish new logical mapping
old extent becomes obsolete or snapshot-pinned
```

### 7.2 partial write

partial write 不要求读旧数据后重写完整 object。extent map 可以表达稀疏 range：

```text
object A:
  [0, 4K)      -> extent x
  [4K, 8K)     -> extent y
  [8K, 64K)    -> extent z
```

读时按 logical range 合并多个 extent。不存在的 range 返回零。

后续为了控制碎片，可以引入 extent coalescing 或后台 compaction。

初始 compaction 触发阈值可以保守设定为：

- 单 object extent 数超过 256。
- 单次 read 平均跨 extent 数超过 8。
- segment live ratio 长期高但前台读放大明显升高。

### 7.3 delete/discard

delete/discard 追加 tombstone 或 range tombstone：

- 删除整个 object：记录 object tombstone，包含 `raft_delete_index/image_delete_seq`。
- 删除 range：记录 range tombstone，包含 `raft_delete_index/image_delete_seq`，读时覆盖旧 extent。

GC 只有在确认旧 extent 不再被 head、snapshot、clone 或 recovery reader 引用时才能回收。

### 7.4 IO 对齐、对象大小与稀疏语义

M1 原型先采用保守约束：

- 对外 `offset/len` 要求 4K 对齐；不满足时返回 invalid argument，不做 RMW。
- object 最大尺寸固定为 4 MiB，与当前块对象模型保持一致。
- 未写入 range 读返回零。
- discard 后的 range 读返回零。

后续如果需要兼容 512B 或非对齐写，再单独引入 RMW 阶段机。

### 7.5 空间压力与 ENOSPC

空间压力必须有确定行为：

- free segment 低于高水位阈值时，后台 GC 提前启动。
- free segment 低于低水位阈值时，前台写入限速或排队。
- 没有可用 segment 且 GC 无法及时释放空间时，store write 返回 ENOSPC。
- ENOSPC 必须向 raft apply 和客户端显式返回，不能静默丢写。
- 当旧 snapshot/clone pin 导致空间无法回收时，可以拒绝创建新的 image snapshot 或 recovery snapshot。

首版可以保守失败，但不能出现“callback 成功而数据不可恢复”的情况。

## 8. Durability contract with raft

raft 和 store 的耐久性契约必须硬定义：

```text
store write callback success
  => 对应 data/tombstone record 已完整持久化
  => crash recovery 后该写必须可见
  => raft 才能把对应 apply 结果视为持久完成
```

首版建议把 data/tombstone record 作为正确性来源：

- data record 完整落盘且 checksum 正确，replay 后必须可见。
- metadata delta 和 checkpoint 只加速恢复，不是唯一真相。
- data record 已写但 checksum 不完整，replay 必须丢弃该 record 及之后 tail。
- checkpoint 写一半，必须回退到上一份完整 checkpoint。

可以允许 batch flush，但语义必须清楚：

- 多个 raft apply 可以合并一次设备 flush。
- flush 完成前，相关 store callback 不能成功。
- 如果 store callback 已成功，重启后不能丢这批写。

这意味着 `last_applied` 的持久推进必须在 store callback 成功之后，不能早于 store 的 durability 点。

还有一个必须处理的 crash 窗口：

```text
data record 已完整落盘
进程在 store callback 返回前 crash
```

这种情况下，恢复后可能看到该 record，但 raft 侧可能还会重新 apply 同一个 index。PGStore 必须按 `raft_apply_index` 做幂等处理：

- 同一个 object/range 上相同 `raft_apply_index` 的重复 record 必须合并为一次可见写。
- 如果发现同一个 `raft_apply_index` 对应不同 payload/checksum，应视为严重一致性错误。
- 重放时以 `raft_apply_index` 顺序发布 PG 状态，避免乱序 record 破坏 raft apply 顺序。

## 9. 读路径

### 9.1 head read

`raft_read_index = 0` 且 `image_snap_seq = 0` 表示读最新 head。PGStore 查 head extent map，按 logical range 拼接数据。

### 9.2 raft snapshot read

raft snapshot read 使用 `raft_read_index = raft_snapshot_index`。

版本选择规则：

1. 如果 `raft_read_index < object.raft_birth_index`，该 object 在此 snapshot 不存在，返回零。
2. 如果 object 在 `raft_read_index` 前已 delete，返回零。
3. 对每个 logical range，选择 `raft_create_index <= raft_read_index` 且未被更早可见 tombstone 覆盖的最新 extent。
4. 如果找不到 extent，返回零。

这等价于 MVCC read view。

### 9.3 image snapshot read

image snapshot read 使用 `image_snap_seq = target_image_snap_seq`。

版本选择规则类似 raft snapshot read，但比较字段换成 `image_birth_seq/image_delete_seq/image_create_seq`。一个请求不应同时指定 raft snapshot read 和 image snapshot read；如果后续确实需要组合语义，必须先定义清楚优先级和合法场景。

## 10. Snapshot 设计

### 10.1 PG snapshot

PG snapshot 服务 raft recovery。创建时只做：

```text
raft_snapshot_index = last_applied_index
raft_snapshot_term = term(raft_snapshot_index)
pin raft_snapshot_index
persist snapshot metadata
```

不遍历 object，不创建 per-object snapshot。

首版策略：

- 每个 PG 最多保留 1 个 raft recovery snapshot pin。
- 创建新的 raft snapshot 前，必须释放或替换旧 pin。
- `open_raft_snapshot_reader()` 成功后增加 reader pin。
- `snapshot_reader::close()` 或 `cancel()` 后释放 reader pin。
- snapshot 发送完成、失败、leader stepdown 或目标 follower 退出 recovery 时，都必须走释放路径。

### 10.2 image snapshot

image snapshot 使用 image 级 `snap_seq`。写请求携带 `snap_context.current_seq`，PGStore 按 seq 写入版本。

如果 image 跨多个 PG，image snapshot 的控制面需要为所有相关 PG 建立同一个 image snap boundary。首版可以先支持离线 snapshot，在线 snapshot 后续通过前端 gate/drain 实现。

image snapshot pin 由控制面生命周期驱动：

- `create_image_snapshot(seq)` 创建 image read view pin。
- clone 或 snapshot reader 可以增加引用。
- `release_image_snapshot(seq)` 只在 snapshot 删除、clone flatten 或引用清零后执行。
- M1 可以不实现 image snapshot pin，但接口和 record 字段必须保留。

### 10.3 install snapshot

raft install snapshot 不再依赖 blob snapshot。leader 直接打开：

```text
PGSnapshotReader(raft_snapshot_index)
```

然后 streaming：

- object metadata
- object data extents
- tombstone/range delete 信息

follower 接收后以批量 append 方式写入自己的 PGStore，并在完成后设置 raft snapshot index。

关键要求：

- snapshot reader 持有 pin，GC 不能回收其引用 extent。
- install snapshot 完成后，follower 可以继续通过 append entries 接收后续日志。
- snapshot reader 必须可取消，取消后释放 pin。

### 10.4 InstallSnapshot follower ingest 状态机

follower 侧不能把 install snapshot 简化为普通覆盖写。需要显式状态机：

```text
empty/current
  -> ingesting(snapshot_index, generation)
  -> sealed
  -> catchup
  -> active
```

建议流程：

1. 收到首个 snapshot chunk，创建临时 ingest generation。
2. 新 snapshot 数据写入 staging PGStore namespace，不能立即覆盖当前 active view。
3. chunk 以 object 或 extent 为单位携带 checksum，follower 做端到端校验。
4. ingest 期间如果 crash，重启后发现未完成 generation，直接丢弃 staging 数据并重新请求 snapshot。
5. 收到 done chunk 后 seal staging generation。
6. 原子切换 active PGStore root 到 snapshot generation。
7. 设置 raft `last_applied/commit/current` 到 snapshot index。
8. 进入 catchup，继续接受 snapshot index 之后的 append entries。

关键约束：

- install snapshot 不应要求原 PG 立即 wipe，否则 crash 中途会失去回退路径。
- 需要 backpressure，避免 staging generation 与旧 active generation 同时存在时把空间打满。
- leader 端 snapshot reader 的 pin 在 install 完成、失败或取消时都必须释放。
- 协议可以先兼容现有按 object chunk 的 `installsnapshot_request`，但长期应演进为 extent/chunk streaming 协议。

manifest 中需要显式保存 active/staging generation：

```text
manifest.pg[pool_id, pg_id].active_generation = G
manifest.pg[pool_id, pg_id].staging_generation = G + 1 | none
manifest.pg[pool_id, pg_id].active_store_epoch = E
manifest.pg[pool_id, pg_id].staging_store_epoch = E + 1 | none
```

首版建议 staging 使用新的 `store_epoch/generation`，并尽量写入 staging 专属 segment 集合。这样失败回收可以按 generation 批量丢弃，避免逐 object 清理。

切换规则：

1. `staging_generation` 写入 manifest 后，ingest record 才允许落盘。
2. `sealed` 状态持久化后，active root 指针原子切到 staging generation。
3. active 切换成功后，旧 generation 进入 `pending_gc`。
4. crash recovery 看到未 sealed 的 staging generation，直接丢弃对应 generation 的 segment 引用。
5. crash recovery 看到 sealed 但未 active 的 staging generation，按 manifest generation/checksum 判断是继续切换还是丢弃。

## 11. Allocator 与 GC

### 11.1 allocator 元数据

allocator 必须持久化：

- segment id
- segment state
- write pointer
- generation
- owner shard/core
- live bytes estimate
- reset count
- checksum

不能只依赖内存 free list。

### 11.2 GC waterline

每个 PG 至少维护：

- `head_raft_index`
- `oldest_raft_snapshot_index`
- `oldest_recovery_reader_index`
- `oldest_image_snapshot_seq`
- `oldest_clone_dependency_seq`

GC 判断需要分别按 raft 时间线和 image 时间线处理引用，不能把两者混成一个 `min(seq)`。

```text
gc_safe_raft_index(pg) = min(
  oldest_raft_snapshot_index,
  oldest_recovery_reader_index
)

gc_safe_image_seq(pg) = min(
  oldest_image_snapshot_seq,
  oldest_clone_dependency_seq
)
```

如果某类引用不存在，它不参与 `min()`；如果 PG 没有任何 snapshot、recovery reader 或 clone dependency，则 GC 可以只按 head extent map 判断 live 数据。

物理 extent 是否 live，不能只看最新 head。必须判断是否被任意 snapshot/read view/clone 依赖。

### 11.3 多 PG segment 归属

多 PG 共享 allocator 时，不能让 sealed segment 中无限混杂所有 PG 的 record。否则 GC live 判断会退化成大量跨 core 查询。

首版约束：

- open segment 按 shard/core 隔离。
- 一个 open segment 优先服务同一 core 上的一组 PG。
- 如果实现成本可接受，优先采用 per-core segment，而不是全局 open segment。
- segment summary 记录 `pg_store_id -> record_count/live_bytes_estimate`。
- GC live 判断按 PG 批量查询：一次把某 segment 内属于同一 PG 的 extent 列表发给 PGStore。
- 禁止 GC 对每个 extent 单独跨 core RPC。

segment summary 是选 victim 的必要输入。`live_bytes_estimate` 可以不完全精确，但必须能通过后台 scrub/rebuild 修正。

### 11.4 segment cleaning

GC 流程：

1. 选择 live ratio 低的 sealed segment。
2. 扫描 segment records。
3. 按 `pg_store_id` 聚合该 segment 内的 extent 列表。
4. 批量发送给对应 PGStore 做 live 判断。
5. live extent 以 GC rewrite record 搬迁到新 segment。
6. 原子发布新的 physical pointer mapping。
7. seal 新 segment。
8. old segment 标记 free/reset。

GC 必须限速，并与前台 IO 分离预算，避免尾延迟失控。

## 12. Crash Recovery

### 12.1 checkpoint + replay

checkpoint 粒度：

- allocator / manifest 使用全局 checkpoint。
- ObjectMap 使用 per-PG checkpoint，或在全局 checkpoint 中保存 per-PG section。
- checkpoint 记录每个 PG 的 `active_generation/staging_generation/store_epoch`。
- replay 扫描 checkpoint 之后的 sealed/open segments，按 `pool_id/pg_id/store_epoch` 路由给对应 PGStore。
- torn tail 只影响所在 segment，不能影响已经 sealed 且 checksum 正确的其他 segment。

恢复路径：

1. 读取最新完整 checkpoint。
2. 重建 ObjectMap、SnapshotRegistry、SegmentAllocator 状态。
3. 从 checkpoint 后的 segment offset 开始 replay records。
4. 校验 record checksum 和 sequence。
5. 遇到 torn write 停止当前 segment replay，并截断/封闭 tail。
6. 丢弃未 sealed 的 install snapshot staging generation。
7. 按 `raft_apply_index` 对用户写 record 幂等排序并发布 PG 状态。
8. 对 GC rewrite record 只更新 physical pointer，不推进用户版本。
9. 重建 live bytes estimate。

M1/M2 需要记录重启恢复耗时；恢复时间如果随全盘 segment 数线性增长到不可接受，应提前增加 checkpoint 频率或 per-PG replay cursor。

### 12.2 原子性规则

一次写入的可见性以完整 record 和 checksum 为准。metadata delta/checkpoint 只加速恢复，不能成为唯一正确性来源。需要定义明确状态：

- data record 完整且 checksum 正确：replay 后可见，重复 apply 由 `raft_apply_index` 幂等处理。
- data record 不完整或 checksum 错误：丢弃该 record 及之后 tail。
- metadata delta 指向 data record 但 data checksum 无效：恢复必须忽略该 metadata delta。
- checkpoint 写一半：必须能识别并退回上一个 checkpoint。

首版建议让 data record 自描述，metadata 可由 replay 重建，降低双写一致性复杂度。

## 13. Run-To-Complete 约束

实现必须遵守现有多核模型：

- 不在 reactor 上做长时间同步扫描。
- 不引入全局大锁冻结 OSD。
- PGStore 状态由归属 core 修改。
- 跨 core allocator 请求使用消息或 lock-free/batched 方式。
- GC、checkpoint、scrub 都拆成小步异步任务。
- install snapshot streaming 按 chunk 推进，支持暂停和取消。

每个设计和代码评审都必须回答：

- 这段逻辑在哪个 core/thread 执行？
- 是否阻塞当前 reactor？
- 是否跨 PG 修改状态？
- 是否会和 GC/checkpoint 争用热路径？
- crash 发生在任意一步时如何恢复？

## 14. ZNS 支持

如果后端是 ZNS：

- segment 对应 zone。
- open segment 对应 open zone。
- append 必须遵守 zone write pointer。
- GC 回收完成后 reset zone。
- allocator 需要控制 open zone 数量。
- checkpoint/manifest 建议放在 conventional zone 或预留 metadata zone。

普通 NVMe 后端也使用相同 segment abstraction，只是不受 zone 顺序写硬约束。

## 15. 为什么不是继续修 blobstore

需要明确：本文方案不是修 install snapshot 的最小代价方案，而是面向下一代 localstore 的长期路线。

可选路径：

```text
A. 保留 blobstore，只优化 raft recovery snapshot
B. 保留 blobstore allocator，自研 object 版本 map
C. 自研 append-only block store
```

A 可以短期降低 install snapshot 创建成本，但仍然无法解决：

- image snapshot/clone 的长期版本生命周期
- ZNS 顺序写约束
- allocator 和 GC 不可控
- per-object blob snapshot 语义继续污染上层 snapshot

B 可以缓解部分版本管理问题，但底层空间、snapshot、GC 仍被 blobstore 约束。它适合作为过渡实验，不适合作为最终架构。

C 工程量最大，但能统一解决：

- lightweight snapshot read view
- versioned extent map
- snapshot-aware GC
- ZNS-ready segment layout
- raft recovery 与 image snapshot 共用底层版本能力

因此，短期仍可以在旧路径上修 blobstore 保障现网稳定；**目标架构是完全离开 blobstore 做 object 数据面**，并在后续阶段再迁出 log/kv，最终去掉 blobstore 依赖。如果目标包含 ZNS、可控 GC 和原生 snapshot/clone，终态应走 C，而不是长期双引擎。

## 16. 与现有 blobstore 路径的关系

### 16.1 战略：终态放弃 blobstore，过程单主路径

组织决策如下：

1. **AppendOnlyStore 是 object 数据面的唯一目标实现。**  
   不把 “blobstore / append-only 双生产引擎长期可选” 当作产品架构。
2. **BlobstoreObjectStore 只允许作为限时遗留/对照实现**，必须有明确删除里程碑，不能无限期 fallback。
3. **不允许**配置项长期 `store_engine=blobstore|appendonly` 双生产开关常开。  
   回滚手段是版本回滚或盘未迁移，而不是进程内长期双轨。
4. **log/kv 暂留 blobstore 不算 “object 两套并存”。**  
   那是第二阶段工作：object 单主路径站稳后，再迁 log/kv，最后从仓库移除 blobstore 依赖。

```text
终态：完全放弃 blobstore（至少 object；理想是整个 localstore）
过程：单主路径演进，而不是两套对等实现长期共存
节奏：先 object → 删旧 object 路径 → 再 log/kv → 最后去掉 blobstore
```

### 16.2 迁移期：限时 shadow，不是产品特性

不要 big-bang 一天删光 blobstore，也不要永久双实现。迁移窗口内可以这样做：

```text
开发/预发限时 shadow（例如 M4–M5）：
  - raft apply 主写 AppendOnlyStore
  - 可选旁路写/读 BlobstoreObjectStore 做对照
  - 对比 head 数据、install snapshot 输出、重启后 hash
  - divergence 必须可判定、可失败，不能静默双写
```

约束：

- shadow mode **有时限**，通过验收后关闭，不进入长期运维手册。
- 新集群 / 新 object 盘：mkfs 直接使用 append-only 分区，默认唯一路径。
- 旧集群：明确迁移窗口；窗口结束后 object 侧不再提供 blobstore 实现。

### 16.3 分阶段放弃 blobstore

| 阶段 | 内容 | blobstore 是否还在 |
|------|------|-------------------|
| A. Object 换主 | object 只走 AppendOnlyStore；删除 object 的 blob 路径 | 仅服务 disk_log + kv |
| B. log/kv 迁出 | raft log、kv 改为独立 raw append 区或并入 StoreEngine | 依赖可移除 |
| C. 仓库级删除 | 无 blobstore 初始化、无 blob snapshot/xattr | 完全离开 |

阶段 A 的完成标志：

```text
代码中不再存在 object_store 的 blob 生产路径
install snapshot 只走 PGSnapshotReader
无长期 store_engine 双生产配置
```

## 17. 与 CSI / image snapshot 分支的关系

### 17.1 两层能力，不是两套产品

卷 snapshot/clone 在仓库里已有独立演进（例如 `origin/csi` 分支，以及 `docs/image_snapshot_clone_design.md` 所描述的能力）。它与本文的关系是**上下层**，不是平行的两套卷 snapshot：

```text
CSI / mon / libfblock / bdev
  │  snap_ctx.current_seq、snapshot/clone 元数据、protect、lineage
  ▼
osd_stm apply
  │
  ▼
localstore 数据面
  │
  ├─ csi 分支现状：blobstore + per-object spdk snapshot / COW
  └─ 本文目标：AppendOnlyStore + image_snap_seq 版本视图
```

- **控制面与北向**（image/snapshot 元数据、`current_snap_seq`、CSI create/delete snapshot、clone 协议）定义“卷”语义。
- **本文**定义 localstore 如何用 `image_snap_seq` 提供只读视图、pin 与 GC，而不再依赖 blob snapshot。

因此：`image_snap_seq`、`create_image_snapshot`、`image_visible(...)` 是在给 CSI/image 产品线**换底**，而不是另起一套 snapshot 语义。

### 17.2 后续合并策略：分层迁，禁止整支硬合

`csi` 分支相对 master 体量大，且数据面仍绑 blobstore。**不要整支 cherry-pick 进 append-only 主路径。** 应按层处理：

| 层 | 来源 | 策略 |
|----|------|------|
| CSI 北向（controller snapshot/clone RPC） | `csi/` | 可独立合入；不依赖 localstore 实现 |
| mon / 协议（`ImageMetadataV2`、`SnapshotMetadataV2`、`snap_seq`、parent/clone） | monitor + proto | 优先对齐；seq 语义与本文 §5.2 一致 |
| 客户端 `snap_ctx` 传递 | libfblock / bdev | 可迁；写路径必须带 `current_snap_seq` |
| osd_stm 写/读 snap 字段 | osd | 改接到 StoreEngine image API |
| object_store blob COW / `spdk_bs_create_snapshot` | localstore | **禁止原样 cherry-pick**；按本文重写 |
| clone 回源、copy-up、flatten | osd + store | 语义测试可复用；实现绑 AppendOnlyStore |
| 证明测试（clone/flatten/rollback proof） | `src/test/*_proof.cc` | 保留断言；底层改为新 store |

适合优先迁入的：

1. 协议与 mon 元数据（snap_seq、protect、parent-child）
2. CSI controller 北向（若产品需要）
3. 客户端 / bdev 携带 `snap_ctx`
4. 证明测试的**语义期望**（作为验收，而非 blob 实现细节）

明确不要原样迁入的：

1. `object_store` 基于 blob snapshot 的 `snap_create` / COW
2. 任何“首次写触发 `spdk_bs_create_snapshot`”路径
3. 与 blob xattr / snap blob 生命周期绑定的恢复逻辑

在 AppendOnlyStore 上的对应关系：

```text
create image snap S     -> create_image_snapshot(S)   // O(1) pin
write after snap        -> write(..., image_snap_seq = current)
read snapshot S         -> read(..., image_snap_seq = S)
clone parent fallback   -> image/clone pin + 读父视图
copy-up / flatten       -> 追加新 extent + 更新映射，再 release 依赖
delete snapshot         -> release_image_snapshot(S)（无 clone 引用后）
```

### 17.3 必须对齐的三个契约

合并控制面与数据面前，下列语义必须与本文 §5.2 / §10.2 / §11.2 一致：

1. **`current_snap_seq` 约定**  
   `create_image_snapshot(S)` 之后，后续写应携带 `image_snap_seq >= S+1`；创建前的写 `<= S`。  
   控制面（mon/image 元数据）是 seq 的权威来源；store 只实现可见性，不重新发明一套编号。

2. **跨 PG boundary**  
   image 可能跨多个 PG。snapshot 一致性 boundary 由 mon/前端 gate 保证；PGStore 只信任请求里的 `image_snap_seq`，不做跨 PG 原子事务。

3. **clone 依赖与 GC**  
   被 clone 引用的 snapshot 必须 pin；对应 `oldest_clone_dependency_seq`。  
   flatten 完成、引用清零后才能 `release_image_snapshot`，否则 GC 不得回收父视图 extent。

### 17.4 与里程碑的衔接

```text
M0–M3
  append-only 聚焦 raft snapshot / head 读写；image 字段与 API 预留
  csi 分支可继续演进控制面，但须认知：blob 数据面是临时实现

M4–M5
  object 主路径已是 AppendOnlyStore
  迁入 mon/协议/snap_ctx；osd_stm 接到 create_image_snapshot / image 读
  禁止再合入新的 blob COW 逻辑

M6+
  GC pin 稳定后做 clone 回源、copy-up、flatten
  再挂 CSI 北向与 proof 测试做产品验收
```

不推荐顺序：先把整支 `csi`（含 blob 数据面）合进 master，再换 append-only。  
那会导致 blob COW 与 versioned extent **双倍返工**。

### 17.5 文档交叉引用

- 卷/image 产品语义：`docs/image_snapshot_clone_design.md`（csi/snapshot 相关分支）
- raft recovery snapshot：`docs/raft_recovery_and_snapshot.md`
- 本文：localstore 物理布局、双时间线、pin/GC、与 blobstore 退出策略

产品讨论以 image 设计为准；落盘与 recovery 以本文为准。两者冲突时：

```text
控制面生命周期、命名、CSI 行为  -> image/CSI 设计
record 可见性、durability、GC     -> 本文
snap_seq 数值约定                 -> 两边必须写成同一套，以 §5.2 为准落地
```

## 18. 实施计划

### M0 设计冻结

输出：

- 本文档评审通过。
- StoreEngine API 冻结。
- raft/image 两条时间线、durability contract、device layout、install snapshot ingest 状态机冻结。
- record/checkpoint 格式带 `format_version` 和 reserved 字段，M1/M2 故障注入后再正式冻结。

### M1 单 PG 原型

目标：

- 单 PGStore 支持 append write、read、overwrite、delete。
- 支持 checkpoint + replay。
- 不接 raft。

验收：

- 随机写读一致。
- crash 注入后 replay 正确。
- allocator 不重复分配。

### M2 多 PG + 共享 allocator

目标：

- 多 PGStore 共用一个 SegmentAllocator。
- 每个 PG namespace 隔离。
- GC live 判断能回到 PGStore。

验收：

- 多 PG 并发读写不串数据。
- PG 删除/重建不会污染其他 PG。

### M3 snapshot read view

目标：

- PG snapshot O(1) 创建。
- snapshot read 正确。
- snapshot pin 阻止 GC 回收。

验收：

- snapshot 后覆盖写，旧 snapshot 读到旧数据。
- 多 snapshot 之间未修改 range 不重复复制。
- 删除 snapshot 后 GC 可回收。

### M4 raft 接入 + 限时 shadow

目标：

- raft apply **主路径**写入 AppendOnlyStore。
- 可选、限时 shadow 旁路对照 blobstore object 路径（仅开发/预发）。
- 自动对比 head 读结果与关键元数据；divergence 必须失败可见。
- raft install snapshot reader 以 AppendOnlyStore 为准做对照/接入准备。

验收：

- 现有 raft 回归全部通过（主路径为 append-only）。
- 限时 shadow 窗口内无未解释 divergence。
- 验收通过后关闭 shadow，不保留双写配置。

### M5 install snapshot 正式接入

目标：

- leader 从 PG snapshot reader streaming。
- follower 以 append-only ingest 安装 snapshot。
- 完成后继续 append entries。
- object 生产路径不再依赖 blob snapshot。

验收：

- 大幅落后 follower 必然触发 install snapshot。
- install snapshot 期间正常读写可继续。
- snapshot 完成后 append entries 正常推进。

### M6 GC 与空间回收

目标：

- segment cleaner 可回收无效 extent。
- GC 限速。
- 支持空间压力下前台写入。

验收：

- 长时间 overwrite workload 空间稳定。
- snapshot pin 下不误删。
- snapshot 删除后空间可回收。

### M7 object 单主路径落地并删除 blob object 路径

目标：

- AppendOnlyStore 成为 **唯一** object 生产路径（不是 “默认 + fallback”）。
- 删除 `BlobstoreObjectStore` / 旧 `object_store` blob 生产代码与双引擎配置。
- disk_log / kv_store 仍可暂留 blobstore（阶段 B 另排期）。
- 具备 fsck/dump 与现场诊断工具。

验收：

- 全量单元、组件、集群、故障、性能测试通过。
- 代码与部署文档中不再出现 object 侧 blobstore 选项。
- 新 mkfs 仅创建 append-only object 布局。

### M8（后续）迁出 log/kv 并移除 blobstore 依赖

目标：

- raft log、kv 迁离 blobstore，或并入统一设备布局。
- OSD 进程不再依赖 spdk blobstore 初始化。

验收：

- 无 blobstore 链接/初始化路径仍可通过全量回归。
- 部署与运维文档完成切换。

image/CSI 控制面与 clone/flatten 数据面按 §17 分层迁入，不占用 M1–M3 主路径；最早在 M4 后接 mon/snap_ctx，M6 后接 clone/flatten 与 CSI 验收。

## 19. 测试计划

### 19.1 单元测试

- record encode/decode/checksum
- extent map range overwrite
- tombstone 覆盖规则
- snapshot version selection
- allocator state transition
- checkpoint 选择和回退

### 19.2 故障注入

在每个关键步骤 crash：

- data record 写一半
- data record 完整但 metadata delta 未写
- metadata delta 写一半
- checkpoint 写一半
- segment seal 写一半
- GC 搬迁一半
- snapshot pin 创建一半
- install snapshot ingest 一半

### 19.3 集群测试

- 多 PG 多副本随机写读。
- follower 大幅落后后通过 install snapshot 恢复。
- snapshot 期间并发读写。
- GC 与 recovery 同时发生。
- OSD 反复重启。
- leader 切换期间写入和 snapshot reader 正确。

### 19.4 性能测试

- 顺序写吞吐。
- 小随机覆盖写 IOPS。
- snapshot 后首次覆盖写延迟。
- GC 前后台干扰。
- 高空间利用率下尾延迟。
- ZNS 后端 open zone 数量和 reset 频率。

## 20. 主要风险

### 20.1 GC 复杂度

append-only 的核心代价是 GC。GC 做不好，会出现空间放大、读放大和尾延迟抖动。

缓解：

- 首版保守 GC。
- 明确 live 判断。
- GC 限速。
- 加空间泄漏检测。

### 20.2 metadata 一致性

allocator、object map、snapshot pin 三者必须 crash-safe。

缓解：

- record 自描述。
- checkpoint + replay。
- 对每种 torn write 写故障测试。

### 20.3 多 PG 共享物理空间

共享 allocator 容易出现跨 PG 状态污染。

缓解：

- metadata 中强制携带 `pool_id/pg_id`。
- PGStore 只修改本 PG 逻辑状态。
- allocator 只管理物理空间，不理解 raft 语义。

### 20.4 读放大

频繁 partial overwrite 会让一个读跨多个 extent。

缓解：

- extent 合并。
- 后台 object/range compaction。
- 对热点 object 做 rewrite。

### 20.5 迁移与双路径风险

两个相反的风险都要避免：

- **big-bang 一天删光 blobstore**（object+log+kv 同时重写）失败面过大。
- **长期双生产引擎**导致行为漂移、测试翻倍、问题归因困难。

缓解：

- 终态明确：object 单主路径，最终放弃 blobstore。
- 过程单主路径演进；shadow 仅限时、可关闭。
- 先删 object 的 blob 路径（M7），再迁 log/kv（M8）。
- 回滚靠版本/盘迁移状态，不靠长期双引擎开关。

### 20.6 双时间线语义风险

raft recovery snapshot 和 image snapshot 使用不同时间线。实现时如果混用 `raft_apply_index` 与 `image_snap_seq`，会出现读错版本、误 GC 或 clone 读回源错误。

缓解：

- API 层禁止同时指定 raft snapshot read 和 image snapshot read。
- extent/tombstone 同时保存 raft/image 两套字段。
- 单元测试分别覆盖 raft 可见性、image 可见性和 head 可见性。
- GC live 判断分别走 raft waterline 和 image waterline。

### 20.7 CSI / image 分支合并风险

整支合入 `csi`（含 blob COW 数据面）再换 append-only，会导致控制面与错误数据面绑定、返工翻倍。

缓解：

- 按 §17 分层迁入：控制面/协议可先，blob 数据面禁止原样 cherry-pick。
- snap_seq 契约与本文 §5.2 先对齐再合 osd 写路径。
- proof 测试保留语义、改绑 StoreEngine。

## 21. 结论

沿盘古类似路线做 append-only block store 是 fastblock 下一代 localstore 的合理方向，但它必须被设计成完整存储引擎：

- append-only physical layout
- versioned extent map
- snapshot/read-view first
- PG-native raft boundary
- shared segment allocator
- checkpoint/replay
- snapshot-aware GC
- ZNS-ready backend

最重要的设计判断是：

```text
不要把 append-only 当成一个写入优化。
要把它当成支撑 snapshot、recovery、allocator、GC 和 ZNS 的底层一致性模型。
```

实现上必须分阶段推进：先原型验证，再以 AppendOnlyStore 为唯一 object 主路径接入 raft，限时 shadow 对照后删除 blob object 路径；log/kv 另阶段迁出，最终完全离开 blobstore。不要把 “两套引擎长期并存” 当成架构。

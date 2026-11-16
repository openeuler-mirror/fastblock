# append_only_store_design.md 设计评审意见

日期：2026-08-11

评审对象：`docs/append_only_store_design.md`

本文是对 append-only block store 设计草案的评审记录，覆盖总体判断、优点、关键缺口、中等风险、与现有代码的贴合度，以及建议的文档修订优先级。

---

## 1. 总评

方向是对的，而且抓住了当前架构的真正瓶颈。

现在的痛点很清晰：`object_store` 是 **1 object = 1 blob**，raft recovery snapshot 又落成 **per-object `spdk_bs_create_snapshot`**。一致性视图被物理 blob snapshot 牵着走，代价和 object 数量线性相关，ZNS / 自研 GC / 可控 lifecycle 也几乎走不通。

把 store 定义成：

- 对上：随机读写对象语义
- 对内：append-only layout + versioned extent map
- 一致性边界：PG
- snapshot：metadata read view，不是物理拷贝

这是合理且足够“大胆”的下一层。里程碑（原型 → 多 PG → snapshot → shadow mode → install snapshot → GC → 切换）和风险意识也都不错。

**真正需要补强的，不是愿景，而是若干会直接决定能不能落地的语义与边界。** 下面按严重程度展开。

---

## 2. 做得好的地方

### 2.1 问题定义准

不是“写慢了所以 append”，而是 snapshot / recovery / allocator / GC / ZNS 被 blobstore 绑死。这个判断对。

### 2.2 snapshot 一等公民

创建只 pin `snapshot_seq` + metadata root，不扫 object，直接对准了现有 `_recovery_by_snapshot` / `object_recovery` 的痛。

### 2.3 PG 作为一致性边界

和 fastblock 多 raft group 模型一致；物理共享 allocator、逻辑隔离 PGStore，分层正确。

### 2.4 record 自描述 + checkpoint/replay

“data 可重建 metadata，降低双写原子性复杂度”是对的，应写成**硬规则**，不要只当建议。

### 2.5 迁移策略务实

`StoreEngine` 双实现 + shadow mode + 长周期对照，比“一次切生产”现实得多。

### 2.6 RTC 约束写进设计

对 SPDK reactor 模型很关键，后面每个 PR 都应用设计文档第 11 节那 5 个问题当 checklist：

- 这段逻辑在哪个 core/thread 执行？
- 是否阻塞当前 reactor？
- 是否跨 PG 修改状态？
- 是否会和 GC/checkpoint 争用热路径？
- crash 发生在任意一步时如何恢复？

---

## 3. 关键缺口（建议冻结设计前补齐）

### 3.1 设备所有权与 blobstore 共存未定义（最高优先级）

当前 localstore 是整盘 blobstore：`disk_log` / `kv_store` / `object_store` 都在上面。

新引擎要自己管 segment / allocator，那就必须回答：

| 问题 | 若不定义的后果 |
|------|----------------|
| 新 store 吃整盘 raw bdev，还是分区？ | 无法启动开发 |
| `disk_log` / `kv` 是否继续 blobstore？ | hybrid 布局、容量规划全乱 |
| 现有部署如何 mkfs / 迁移？ | M7 不可达 |
| superblock / manifest 放哪？ | recovery 无根 |

建议在设计文档加一节 **Device layout**：

```text
[ superblock ]
[ manifest / checkpoint zones ]
[ data segments ... ]
# 或：预留 blobstore 区域给 log/kv（过渡期）
```

首版可以明确：**只替换 object 数据面；log/kv 暂留 blobstore（甚至独立小盘/分区）**。但要写死，不能悬空。

### 3.2 `apply_seq` / `snap_seq` / `read_seq` 命名空间混在一起

文档里三者都叫 `seq`，但至少有两套：

- **raft apply index**（PG 内单调，服务 recovery snapshot）
- **image `snap_seq`**（可能跨 PG，服务 CSI/image snapshot）

它们不是同一条时间线。若写路径只塞一个 `snap_seq`，image snapshot 和 raft snapshot 的版本选择会缠在一起。

建议显式拆开：

```text
raft_apply_index   // PG 内，install snapshot / recovery pin
image_snap_seq     // image 级，可选；首版可只支持 raft 维度
generation / epoch // PG 重建、delete+recreate 防串数据
```

`create_snapshot` 也应区分 `raft_snapshot` vs `image_snapshot`，或至少说明首版只做前者。

### 3.3 元数据模型写得太“概念”，会在实现时爆炸

`ObjectMap: head_extent_map + historical_versions` 还不够落地，至少要定：

1. **historical 的结构**  
   全量 map 拷贝？interval tree + version chain？delta 链？  
   频繁 partial overwrite 下，结构选择直接决定内存和 snapshot 读复杂度。

2. **内存上限**  
   单 PG 几十万～百万 object 时，全量常驻 ObjectMap + 历史 extent 是否可接受？  
   checkpoint 是全量 dump 还是分层（L0 delta + L1 object table）？

3. **object 标识**  
   每条 data record 带完整 `object_name` 字符串，小 IO 元数据开销很大。  
   更常见：`object_id`（hash/分配 id）+ 名称表；record 里只放定长 id。

4. **object 大小模型**  
   固定 max object size（类 Ceph 4MB）还是任意大稀疏对象？  
   影响 extent 数量、recovery 分片、读合并策略。

建议 M0 补一张 **“单 object 在 1 万次 4K overwrite + 3 个 snapshot 后”** 的内存/磁盘元数据量级估算。

### 3.4 写完成语义 / 耐久性契约要钉死

设计文档 6.1 的步骤顺序现在有歧义：

```text
写 data → fsync? → 更新内存 map → metadata delta → 回调 apply
```

若坚持 “record 自描述，metadata 可由 replay 重建”，正确契约更接近：

```text
data record 落盘且 checksum 完整
  => 该写在 recovery 后可见
  => 此时才允许 apply 完成回调
内存 ObjectMap 只是缓存
metadata delta/checkpoint 只加速恢复，不参与正确性（或仅作加速）
```

需要明确：

- apply complete 的 durability 点在哪？
- 是否允许 batch fsync（多个 apply 合并 flush）？
- torn write 时“丢弃未完整 record”是否与 raft 的 “已 commit 必不丢” 冲突？

答案通常是：store 回调成功前 raft 不能推进 `last_applied` 的持久化；回调成功后必须可 recovery 出该写。

这和现有 raft apply / disk_log trim 边界强相关，建议单独一小节 **Durability contract with raft**。

### 3.5 多 PG 共享 segment 的 GC 跨核代价被低估

设计写：scan segment → 对每个 extent 问 PGStore 是否 live。

问题：

- sealed segment 里可能交织很多 PG 的 record
- PGStore 在 home core 上，GC 若在别的 core，live 查询是大量跨核消息
- 没有 **segment summary / reverse index / per-PG live-bytes**，`live_bytes estimate` 会漂，选 victim 会不准

建议首版简化策略写进文档（可接受的工程折中）：

1. **open segment 按 core 隔离**（已有）
2. **sealed segment 尽量 per-PG 或 per-core**（降低交织），或
3. 每个 segment 维护 `pg_id → live_bytes/record 摘要`，GC 先按摘要过滤，再细查
4. live 判断批量化：一次把 segment 内属于某 PG 的 extent 列表发给该 PGStore

否则 M6 很容易变成“能跑 demo、扛不住多 PG 压力”。

### 3.6 install snapshot 的 ingest 语义不完整

现在有 reader 侧 pin/streaming，缺 follower 侧：

- 已有脏数据怎么办：整 PG wipe 再 ingest，还是按 object 覆盖？
- ingest 到一半 crash：如何识别 partial install 并回滚？
- 空间：全量 snapshot 临时接近 2× 占用时如何 backpressure？
- streaming 单位：按 object、按 extent、按固定 chunk？checksum 如何端到端校验？
- 与现有协议（`installsnapshot_request` 按 object 批量）是兼容演进还是新协议？

建议 M5 前先定 **InstallSnapshot ingest state machine**（`empty → ingesting → sealed → catchup`）。

### 3.7 StoreEngine API 对 raft/OSD 还不够用

现状接口覆盖了 read/write/remove/snapshot，但生产路径通常还要：

| 能力 | 原因 |
|------|------|
| `iterate_objects` / recovery cursor | install snapshot、scrub、fsck |
| `stat` / 空间用量 | mon、ENOSPC、GC 触发 |
| `discard(offset,len)` | UNMAP/trim |
| `flush` / 持久化 barrier | 与 raft 耐久点对齐 |
| batch apply（同 `apply_seq` 多 object） | 合并 entry 常见 |
| `delete_pg` / `create_pg` | PG 生命周期 |
| cancelable `snapshot_reader` | 已有文字，缺 API |

另外 `store_object_id` 里带 `pool_id/pg_id`，而类又叫 `pg_store`——id 与 store 实例的归属关系建议统一（要么 id 不含 pg，要么全局 `OSDStore::write(id, …)`）。

### 3.8 和 “只改 snapshot、不换引擎” 的对比写得不够

大胆重做之前，最好用一小节回答：为什么不走更小切口？

例如：

- **A.** 保留 blobstore，raft snapshot 改为逻辑 pin + 读 head（不做 per-object blob snap）
- **B.** blob + 自研 object 级 COW map，仍用 blob 当 allocator
- **C.** 本文 append-only 全自研

A/B 解决不了 ZNS、统一 GC、allocator 可控的话，直接说清楚 **为什么必须到 C**。否则评审时容易被挑战“是不是 overkill”。

个人判断：若中期真要 ZNS + 可控 GC + 轻量 snapshot，C 合理；但短期若只救 install snapshot 启动成本，A 可能是更便宜的过渡。文档里应用目标反推选 C。

---

## 4. 中等风险 / 实现时必踩的坑

1. **读放大**  
   文档有 coalescing，建议定阈值：extent 数 > N 或 read 跨 extent > M 时触发异步 rewrite。

2. **校验和与 torn write**  
   header 在 payload 前时，单独 header CRC 不够；需要 payload CRC，且长度字段策略能识别半截写。

3. **零填充与稀疏读**  
   “不存在 range 返回零”要和块设备语义、是否暴露 unwritten 一致。

4. **对齐**  
   对外是否强制 512/4K？不对齐是 RMW 还是禁止？

5. **GC 与前台写抢 open segment**  
   需要独立 GC 写入通道和 IO 预算，否则尾延迟必抖。

6. **raft 通常只保留一个 recovery snapshot**  
   与 image 多 snapshot 的 pin 模型不同；`gc_safe_seq` 公式对两类引用应分开讲。

7. **格式冻结（M0）过早**  
   record/checkpoint 建议从第一天带 `version` + reserved，M1 原型后再 freeze，而不是评审日 freeze。

8. **Shadow mode 的真相对照**  
   两边独立 allocator/时间线时，byte 级一致容易；但 snapshot index、删除时序、crash 后 recovery 边界要对齐注入点，否则 false divergence 会淹掉真 bug。

---

## 5. 和现有代码的贴合度

| 现状 | 设计对应 | 评价 |
|------|----------|------|
| `object_store` per PG，table 在内存 | PGStore + ObjectMap | 自然演进 |
| `snap_create` → `spdk_bs_create_snapshot` | O(1) metadata snapshot | 核心收益 |
| `object_recovery` 扫 object 建 recovery snap | `PGSnapshotReader` | 方向正确，协议层要一起改 |
| `disk_log` / `kv_store` 仍 blobstore | 文档几乎未谈 | **必须补** |
| multi-core + home shard | 设计文档 §11 RTC | 对齐得好 |
| install snapshot 按 object 批量 | streaming extent | 需协议演进计划 |

相关参考：

- `src/localstore/object_store.h` / `object_store.cc`
- `src/localstore/README.md`
- `docs/raft_recovery_and_snapshot.md`

---

## 6. 建议的文档修订优先级

### P0（不补不宜开写代码）

1. Device layout + 与 blobstore/log/kv 共存策略
2. seq 语义拆分（raft index vs image snap_seq）+ durability 契约
3. ObjectMap/historical extent 的具体结构和内存预算
4. install snapshot follower state machine

### P1（M1–M2 前）

5. StoreEngine API 补齐 iterate/stat/pg lifecycle/flush
6. 多 PG GC 的 segment 归属与 live 判断批量化
7. record 用 object_id，名字二级索引
8. 为何选全自研 append-only 而非增量改造

### P2

9. 量级估算、IO 模型、对齐/稀疏语义
10. 格式 versioning，避免 M0 过早冻死

---

## 7. 结论

这是一份**方向正确、分层清楚、迁移意识成熟**的草案，配得上“大胆重构”：把 append-only 当成一致性模型，而不是写路径小优化——这点判断很准。

当前最大风险不是“这个方向错了”，而是：

1. **设备与现有 blobstore 世界的边界没画清**
2. **版本/seq 与 durability 语义还不够硬**
3. **元数据与 GC 的复杂度写得偏概念，工程量可能被低估**

建议：把上面 P0 四块补进设计文档并再开一次窄评审，然后用 **M1 单 PG 原型**验证 checkpoint/replay/故障注入，再冻结 format。不要在 M0 就冻死 record layout。

---

## 8. 可选后续工作

- 按本评审直接修订 `docs/append_only_store_design.md`（补 P0 章节 + 修订 API/seq/durability）
- 或先出一页「与现有 `object_store` / install snapshot 的对照变更清单」

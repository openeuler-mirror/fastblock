# fastblock Append-Only Store（AOS）统一 Localstore 设计

日期：2026-08-12（v2：AOS 管一切）  
状态：**object + log + kv 引擎已落地；启动路径 AOS-only；P4 生产路径清理完成**（blob 实现仍留 demo）  


实现目录：`src/localstore/aos/`  
历史评审：`docs/append_only_store_design_comments*.md`（针对 v1 草案，仅作档案）

---

## 0. 文档定位与决策

### 0.1 一句话

```text
AOS 不是 “object 写入优化层”，
而是 fastblock OSD 本地存储的唯一引擎平台：
  object 数据面 + raft log + PG/元数据 KV
  共享 device_backend / segment / record / recover
  最终进程内无 spdk blobstore。
```

### 0.2 已钉死的产品决策

| 决策 | 内容 |
|------|------|
| 无兼容包袱 | **未真正上线**；不迁旧 blob object 数据，不双轨，不 shadow 产品化 |
| Object 唯一路径 | 生产 object 读写/install **只走 AOS**；无 `object_engine=blob` |
| 终态无 blobstore | log、kv 亦迁出后删除 `blobstore_init` 与 blob 依赖 |
| 回滚 | 版本回滚 / 重建集群，**不是**进程内 fallback |
| 实现顺序 | Object 已切（knife-1）→ Log → KV → 删 blobstore 启动路径 |

### 0.3 与 v1 草案的关系

v1（2026-08-11）把 AOS 定义为 **object 数据面替换**，log/kv 明确「另阶段」。  
v2 在工程判断上升级为：

```text
v1：AOS 替换 object_store，blobstore 暂服 log/kv
v2：AOS 是 StoreEngine 平台；object / log / kv 都是平台上的逻辑引擎
     共享物理层，禁止用 4K object 语义硬模拟 log/kv
```

object 子系统设计（双时间线、extent、snapshot pin、install ingest、GC）大体沿用 v1；  
**布局、进程模型、里程碑、与 blobstore 关系**按 v2 重写。

---

## 1. 背景

fastblock 本地存储曾全部依赖 SPDK blobstore：

| 组件 | 旧实现 | 痛点 |
|------|--------|------|
| object | `object_store` + blob + `spdk_bs_create_snapshot` | recovery snapshot 成本 ∝ object 数；image snap 被 blob 语义牵引 |
| raft log | `disk_log` + `rolling_blob` | 与 blob 生命周期/xattr 耦合 |
| kv | `kvstore` + rolling_blob + blob checkpoint | 同上；每 PG 仅数个小 key，却绑整套 blob 栈 |

目标能力（高质量 raft recovery、image snapshot/clone、可控 GC、ZNS）要求 **自管 append-only 物理布局与版本生命周期**。  
v2 进一步要求：**一条盘栈服务全部本地持久状态**，而不是「object 自研、log/kv 永远 blob」。

---

## 2. 目标

### 2.1 已完成（knife-1 / 阶段 A）

- [x] Object 随机读写、overwrite、delete/discard、幂等 apply  
- [x] 多 PG + 共享 `segment_allocator` + superblock/manifest  
- [x] Raft / image 双时间线 + O(1) snapshot pin + 历史读  
- [x] Install：leader pin + list/read；follower AOS apply（ingest SM 库就绪）  
- [x] GC skeleton（free dead sealed + rewrite + 限速参数）  
- [x] OSD 进程强制 AOS 对象盘；**无 blob object 生产路径**  

### 2.2 阶段 B：log + kv 迁入平台（基本完成）

- [x] `log_store`：对齐 `disk_log` API，底层 AOS record/segment  
- [x] `kv_store` / `kv_engine`：put/get/commit + WAL + checkpoint  
- [x] 启动路径：**跳过** `blobstore_init/load`；`aos_format` / `aos_recover` + `attach_aos_kv`  
- [x] PG load 从 AOS `list_pg_ids()` 发现，不再扫 blob xattr  
- [ ] 单盘布局/部署文档完全收敛（可仍挂 legacy bdev 名但不 init bs）  

### 2.3 终态（阶段 C / P4）

- [x] 生产进程无 `blobstore_init/load`；无 object/log/kv 的 blob **生产入口**  
- [x] load PG API 去掉 blob/`object_store` 参数；`osd_load` 仅扫 `list_pg_ids`  
- [x] demo-only 标注：`make_disk_log` / `make_kvstore` / `blobstore_init`  
- [ ] 从仓库物理删除 `object_store`/`rolling_blob`（仍被 demo 链接）  
- [ ] 部署文档与 mkfs 仅 AOS 布局  
- [x] dump / light fsck 库：`aos_dump`（`scan_image` + `format_report`）；CLI/scrub 后续  


### 2.4 非目标（仍适用）

- 不追求首版极致性能  
- 不做复杂压缩、EC、跨节点编码  
- 不做跨 PG 原子事务  
- CSI 完整产品验收可后置；底层 `image_snap_seq` 能力先具备  

---

## 3. 核心原则

### 3.1 平台与逻辑引擎分离

```text
物理平台（共享）
  device_backend / spdk_bdev_backend
  segment_allocator（fixed-size segment）
  record encode/decode/CRC
  superblock A/B + manifest generation
  recover: 找合法 checkpoint/generation + 扫 tail

逻辑引擎（各用各的语义，共用物理平台）
  object_engine  → pg_store / osd_store（versioned extent）
  log_engine     → log_store（有序 entry 流 + index map + trim）
  kv_engine      → kv_store（内存表 + WAL + checkpoint）
```

**禁止：**

- 用 4MiB/4K object 接口模拟 raft log  
- 每个 KV key 一个 object  
- 三套互不相干的盘格式  

### 3.2 snapshot 是 object 引擎一等公民

object 的 raft/image snapshot 仍是 **O(1) pin + 版本可见性**，不扫全量 object、不建 per-object 物理 snapshot。  
log 的「截断」是 **trim**，不是 object snapshot。  
kv 的「快照」是 **checkpoint generation**，不是 raft snapshot。

### 3.3 append-only 是物理布局，不是对外语义

| 引擎 | 对外 | 对内 |
|------|------|------|
| object | 随机读写 / overwrite / discard | 追加 extent + 发布 map |
| log | append / range read / trim | 追加 log record + index |
| kv | put/get（内存）+ 后台 durable | 追加 WAL record + 偶发 checkpoint |

### 3.4 PG 是 raft 一致性边界

- object 与 log：**按 PG 隔离**（`pool_id/pg_id/store_epoch`）  
- kv：当前是 **shard 级一张表、key 带 `pool.pg.` 前缀**；迁入后可保持该模型，或 per-PG 小表，但 **不得跨 PG 共享可变 object 状态**  

### 3.5 正确性优先

难点仍是：metadata 原子性、allocator crash consistency、pin/GC、恢复、尾延迟。  
先可验证，再压榨性能。

### 3.6 Run-To-Complete

所有盘 IO 与状态推进在 SPDK reactor 上异步完成；禁止 reactor 上阻塞等盘。  
GC / checkpoint / scrub 拆小步。

---

## 4. 总体架构

### 4.1 进程内结构（目标）

```text
osd / partition_manager / raft_server
  │
  ▼
aos_runtime (per shard region)
  │
  ▼
osd_store（设备门面）
  │
  ├── pg_store × N          # object_engine
  ├── log_store × N         # log_engine（per PG）
  ├── kv_store × 1          # kv_engine（per shard，key 含 pool.pg）
  │
  ├── SegmentAllocator      # 可按 owner 分 segment 池（object / log / meta）
  ├── Manifest / Superblock
  └── DeviceBackend
```

### 4.2 部署布局（目标）

**推荐单盘统一布局**（新集群默认）：

```text
+-----------------------------+ 0
| superblock A/B              |
+-----------------------------+
| manifest slots              |
+-----------------------------+
| kv checkpoint slots         |  小、双 generation
+-----------------------------+
| kv WAL / meta segments      |  可选独立段池
+-----------------------------+
| data segments               |  object + raft log 记录混部或分池
|   (object / log records)    |
+-----------------------------+
```

**过渡（knife-1 现状）**：

```text
主盘 bdev  → blobstore（仅 disk_log + kv）   ← 待删除
AOS 盘     → object only（已强制）
```

过渡只为减少同时改动面；**不是终态**。B 阶段结束后主盘 blobstore 消失。

### 4.3 Segment 归属（多引擎共盘）

首版约束：

- segment 可标记 `owner_class ∈ {object, log, kv_meta}`  
- 或使用 **独立 open segment** 给 log / object，避免无限混杂导致 GC 扫描跨引擎  
- GC live 判断：  
  - object：extent + pin waterline  
  - log：index map 仍引用的物理范围  
  - kv：WAL 在 checkpoint 之前的 generation 可裁  

### 4.4 启动顺序（目标）

```text
1. open bdev + aos_runtime.open(region)
2. recover superblock/manifest
3. recover kv（checkpoint + WAL）
4. recover log streams（或按 PG lazy）
5. recover object PGs（或 ensure_pg on create）
6. partition_manager 挂 raft（log + stm + kv 指针均来自 AOS）
```

knife-1 现状：blobstore 先起 → storage(kv) → AOS recover object → load PG。  
B 完成后变为上表。

---

## 5. Object 引擎（沿用并固化）

### 5.1 API 面

`osd_store` / `pg_store` 提供：

- `write/read/remove/discard/flush`  
- `create/release_raft_snapshot`、`create/release_image_snapshot`  
- `open_raft_snapshot_reader` / stream  
- install：`snapshot_ingest`（staging epoch）  
- `checkpoint` + `recover`  

STM 路径：`osd_stm::attach_aos` 后 apply/read/delete/install **只走 AOS**。

### 5.2 双时间线

| 字段 | 用途 |
|------|------|
| `raft_apply_index` | PG 内 raft apply 序；recovery / raft snap |
| `image_snap_seq` | image/CSI 控制面；与 raft index 无天然顺序 |

读上下文禁止同时非零指定两种 snapshot read（非法参数）。  
详见实现：`pg_store::view_at` / version log。

### 5.3 Record（object 相关）

已实现类型包括：`data`、`delete_tombstone`、`range_tombstone`、`gc_rewrite`、`object_name`、`segment_seal`、`checkpoint_marker`、`raft_snapshot_pin/unpin`、`image_snapshot_pin/unpin`、`install_seal/activate`。  

`format_version`、CRC、自描述 record 规则保持。

### 5.4 Durability（与 raft 合同）

object 用户写 complete 仅在 **record 落盘 + flush** 之后（§ 与 raft apply 完成对齐）。  
log/kv 各自定义 complete 语义（见下），但 **不得弱于「调用方认为 durable」的契约**。

### 5.5 Install snapshot

- Leader：`pin_recovery_view` + list live objects + `read_recovery_object`  
- Follower：`write_install_object` → AOS apply；完整 staging 状态机（`snapshot_ingest`）为强化路径  
- **禁止** `spdk_bs_create_snapshot` 服务 object recovery  

---

## 6. Log 引擎（新建）

### 6.1 职责

替换 `disk_log` + `rolling_blob`，对接现有 `raft_log`。

最小 API（对齐现状）：

```text
append(entries) -> cb
read(start_index, end_index) -> entries, cb
trim / 前缀回收（配合 raft base_index）
stop / remove_pg_log
```

### 6.2 物理模型

```text
log_record:
  magic, format_version, record_type=raft_log_entry
  pool_id, pg_id, store_epoch
  raft_index, raft_term
  payload_len, header_crc, payload_crc
  payload = 现有 EncodeLogHeader 布局或等价序列化
```

内存：

```text
index_map: raft_index -> { phys_offset, length, term }
base_index, last_index
```

### 6.3 为什么不用 object API

- entry 大小可变，不是 4MiB 固定 object  
- 需要高效 **前缀 trim**，不是版本 pin  
- 读路径是 **按 index 范围连续读**，不是 sparse random  

### 6.4 与 raft 的边界

- `raft_log` 缓存层可保留  
- 仅替换 `disk_log` 实现类 / 工厂 `make_disk_log`  
- 每 PG 一条 log 流（与现 per-PG blob 对应）  

### 6.5 恢复

- 从 log segment 扫描合法 record 重建 `index_map`  
- torn tail 丢弃（CRC 失败停扫）  
- 不依赖 blob xattr  

### 6.6 Trim / GC

- `trim_before(index)`：删除 map 前缀；物理段无任何 live index 后标 free  
- 可与 object GC 共用 cleaner，但 live 判断分离  

---

## 7. KV 引擎（新建）

### 7.1 真实工作负载（决定方案）

当前 `raft_server` 仅使用：

| key 模式 | 含义 |
|----------|------|
| `{pool}.{pg}.vote_for` | 投票 |
| `{pool}.{pg}.term` | 当前 term |
| `{pool}.{pg}.node_cfg` | 成员配置 |
| `{pool}.{pg}.lapply_idx` | last applied |

→ **小表、低频写、读几乎全在内存**。  
现实现：`put` 进 op_log → poller 批量 `commit` append WAL → 周期性 checkpoint。

### 7.2 目标语义（保持上层不变）

```text
put/remove：改内存意图（可同步返回）
get：只读内存表
commit / poller：异步 durable WAL
checkpoint：整表快照，加速恢复、允许裁 WAL
```

**不在 v2 改变** raft 对 kv 的调用形状（同步 put/get）。

### 7.3 推荐实现：K1 Meta 区（首选）

```text
kv checkpoint slots（双 generation，类似 manifest）
  + kv WAL records（append-only，可放专用 meta segments）
```

- 与 object/log **物理隔离**，避免 GC 纠缠  
- 复用 generation + checksum 选择最新合法视图  
- recover：`load best checkpoint → replay WAL`  

### 7.4 备选：K2 系统 record 流

- `record_type::kv_put / kv_delete / kv_checkpoint`  
- 系统 namespace或独立 segment pool  
- 可行，但 live 规则必须与 object 分开实现  

### 7.5 明确禁止：K3 object 伪装

每个 key 一个 object：对齐放大、异步语义错位、过重。**不做。**

### 7.6 与现 `kv_checkpoint` 的关系

逻辑可平移：整表序列化 → 写新 generation → 切换 active → 允许裁旧 WAL。  
载体从 blob 换成 **meta slot / meta segment record**。

---

## 8. 统一 Record 类型空间

| type | 引擎 | 说明 |
|------|------|------|
| data / tombstone / range_tombstone / gc_rewrite | object | 已实现 |
| object_name / checkpoint_marker | object | 已实现 |
| raft/image pin-unpin / install_* | object | 已实现 |
| segment_seal | 共享 | 已实现 |
| **raft_log_entry** | log | **待实现** |
| **kv_put / kv_delete** | kv | **待实现** |
| **kv_checkpoint** | kv | **待实现** |
| metadata_delta | object 优化 | 可选，非阻塞 |

所有类型共用 magic/version/CRC 框架；未知 type 在 recover 时按策略 skip 或 fail-fast（首版 fail-fast 更安全）。

---

## 9. Device layout 细节（目标格式）

```text
offset 0
  superblock A (4KiB)
  superblock B (4KiB)
  manifest slots (2 × 128KiB)     # PG epoch 表、open cursor、可选 log/kv 指针
  kv_checkpoint slots (2 × N)     # 新；N 足够装整表序列化
  [reserved]
data_region_base
  segments[0..S)
    owner: object | log | kv_wal
```

`superblock` 增加：

- `layout_version`（v2 起）  
- `flags`（是否含 kv_meta 区）  
- `kv_checkpoint_generation` 提示（可选，真相仍以 slot checksum 为准）  

`manifest` 增加（可选渐进）：

- per-PG：`log_base_index` / `log_high_water` 提示  
- `kv_wal_high_water`  

Checkpoint **不是唯一真相**；record 自描述 + 扫盘可重建。

---

## 10. 与 blobstore 的关系（v2）

### 10.1 战略

```text
终态：仓库级无 blobstore 生产路径
过程：单主路径硬切（无上线兼容）
禁止：长期 object/log/kv 双实现开关
```

### 10.2 阶段

| 阶段 | 内容 | blobstore |
|------|------|-----------|
| **A（已完成）** | object 仅 AOS；删 object 生产 blob 路径 | 仅 log+kv |
| **B（进行中）** | log_store + kv_store 上 AOS 平台 | 可移除 |
| **C** | 无 blobstore_init；链接与文档清理 | 无 |

### 10.3 现状代码映射

| 模块 | 状态 |
|------|------|
| `osd_stm` object IO | AOS only |
| `aos_runtime` / 强制 `aos_disk` | 已落地 |
| `disk_log` | 仍 rolling_blob |
| `kvstore` | 仍 rolling_blob |
| `blobstore_init` | 仍服务 log/kv 与遗留扫描 |

---

## 11. CSI / image

上下层关系不变：

- 控制面：mon / CSI / `snap_seq`  
- 数据面：AOS `image_snap_seq` + pin/GC  

禁止整支合入含 blob COW 的 csi 数据面；控制面可分层迁。  
契约以本文 object 可见性规则 + image 产品文档对齐。

---

## 12. 实施计划（v2 里程碑）

### 已完成（归档，不再排期）

| 代号 | 内容 |
|------|------|
| M0–M3 | 设计 + 单/多 PG + snapshot view |
| M4† | 进程 attach + **直接 AOS-only**（跳过长期 shadow） |
| M5† | install 写/读走 AOS；ingest 库就绪 |
| M6† | GC skeleton + rewrite + rate limit 参数 |
| **A / knife-1** | object 唯一路径 + 强制 AOS 盘 |

† 相对 v1 的 M4–M7 有所压缩：无兼容 → 不保留 blob object 与 shadow 产品路径。

### P0 — 设计冻结（本文 v2）

- [x] 平台三分引擎 + 禁止 object 模拟 log/kv  
- [ ] layout_version 与 record type 枚举冻结（实现时写入 `types.h` / `layout.h`）  

### P1 — log_store ✅

目标：

- `log_store` 实现 append/read/trim/recover  
- 替换 `make_disk_log` / PG create-load 绑 log  
- 单测：append 顺序、读范围、trim、crash tail  

验收：

- 无 blob 的 disk_log 路径下 raft 基本回归通过（可与 object AOS 同盘或同 runtime）  

### P2 — kv_store on meta ✅

目标：

- meta slots + WAL  
- 保持 put/get/commit 语义  
- 替换 `make_kvstore`  

验收：

- term/vote/cfg/lapply 持久化与重启一致  
- 故障注入：WAL 半写、checkpoint 半写  

### P3 — 统一 mkfs / 启动 ✅（代码路径）

目标：

- 单盘或双区 AOS-only mkfs  
- 删除 blobstore_init 对 log/kv 的依赖  
- 删除 object 相关 blob 扫描残留  

验收：

- 全新集群：无 blobstore 进程可起、可写、可恢复  
- 实现：`block_started` 跳过 bs；`storage_*` defer kv；`list_pg_ids` load；create/load PG 仅 AOS

### P4 — 清理与工具（生产入口 ✅；物理删库 / 工具 ⏳）

- [x] 删除 production 对 `rolling_blob` / blob object 的调用与 load API 参数  
- [x] `storage_manager` 永不建 blob kv；demo 仍可直调 `make_kvstore`  
- [x] 文档：`aos/README`、`localstore/README`、设计 v2 状态  
- [ ] 物理删除 `object_store.cc` / `rolling_blob` / blob_manager 生产依赖（需迁移 demo）  
- [x] dump/fsck 库 `aos_dump.{h,cc}` + 单测；独立 CLI 后续  
- [ ] 部署脚本仅 AOS 盘  

### P5+ — image/CSI、ZNS、性能

- mon snap_ctx、clone/flatten  
- ZNS segment=zone  
- GC 策略与压测  

---

## 13. 测试计划（增量）

### Object（已有 + 加强）

- record/extent/snapshot/GC/install 单元测试  
- 故障注入 torn write  

### Log（新增）

- index 连续与空洞拒绝策略  
- trim 后读失败边界  
- 恢复后 last_index 正确  

### KV（新增）

- 批量 put 后 kill，WAL replay  
- checkpoint 后裁 WAL，重启数据完整  
- 并发 put 与 poller commit  

### 系统

- 无 blobstore 启动  
- 多 PG 写 object + 打 log + 改 term  
- install snapshot + restart  

---

## 14. 风险

| 风险 | 缓解 |
|------|------|
| 三引擎共盘 GC 互伤 | segment owner_class；live 判断分引擎 |
| 一次改 log+kv+启动面过大 | P1→P2→P3 严格串行验收 |
| layout 变更 | `layout_version`；新集群 only |
| log 性能 | 先正确；批量 append 对齐现 disk_log |
| kv 误做成大引擎 | 锁死工作负载：每 PG 少量 key |

---

## 15. 结论

```text
AOS = fastblock 本地存储平台
  object_engine：已上线路径（knife-1）
  log_engine：下一切，替换 disk_log
  kv_engine：再下一切，meta WAL+checkpoint，替换 kvstore
  终态：无 blobstore

不要：
  - 双生产引擎长期并存
  - 用 object 接口硬套 log/kv
  - 为未上线系统背兼容包袱
```

实现顺序：**冻结本文 → P1 log_store → P2 kv meta → P3 去 blobstore 启动 → P4 清理。**

---

## 附录 A. 代码地图（实现时）

| 区域 | 路径 |
|------|------|
| 平台 | `src/localstore/aos/`（device, segment, record, superblock, osd_store） |
| object | `pg_store.*`, `snapshot_*. *`, `osd_stm` attach |
| log（目标） | `src/localstore/aos/log_store.*` 或 `src/localstore/log_store.*` 接 AOS backend |
| kv（目标） | `kv_store` 后端换 meta；或 `aos/kv_engine.*` |
| 进程 | `aos_runtime`, `partition_manager`, `osd.cc` |
| 旧 blob | `blob_manager`, `rolling_blob`, `object_store` — 阶段 C 删除生产路径 |

## 附录 B. v1 → v2 变更摘要

| 主题 | v1 | v2 |
|------|----|----|
| AOS 范围 | 仅 object | object + log + kv 平台 |
| blobstore | object 先切，log/kv 长期可留 | 全部迁出，尽快删 |
| 兼容 / shadow | 限时 shadow | **不做**（无上线） |
| 部署 | object 独立盘 + blob 主盘 | 目标单盘统一；过渡双盘可接受 |
| 里程碑 | M0–M8 | A 完成 + P1–P4 |

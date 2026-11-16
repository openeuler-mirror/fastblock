# append_only_store_design.md 第二轮设计评审意见

日期：2026-08-11

评审对象：`docs/append_only_store_design.md`（更新后版本）

上一轮评审：`docs/append_only_store_design_comments.md`

本文是对更新后设计草案的第二轮评审。总体结论：**上一轮 P0 基本补齐，文档已达到可按 M1 开工的标准；剩余问题主要是语义边角与 API/落盘细节。**

---

## 1. 总评

上一轮 P0 基本都补上了，文档从“方向正确的概念稿”变成了“能按里程碑开工的引擎草案”。

**大方向维持认可；当前主要问题从“缺关键章节”变成了“若干语义边角仍会在实现时咬人”。**

- **可以开 M1（单 PG 原型）**，不必再等大改架构。
- **不建议现在就宣布 M0 完全冻结**，尤其 API 细节、image 版本语义、snapshot reader 句柄生命周期。
- 剩余问题大多是：双时间线如何写入/选择、staging 与 epoch 如何落盘、运维/空间压力语义，属于“定清楚就能写”的级别。

---

## 2. 本轮明显变好的地方

| 上一轮缺口 | 本版对应 | 评价 |
|-----------|----------|------|
| Device layout / 与 blobstore 共存 | §4.1 | 钉死了“只替换 object 数据面”，布局也清楚 |
| seq 命名空间混用 | §5 + API 拆字段 | 两条时间线拆开，这是质变 |
| Durability 契约 | §8 | 含 batch flush、`last_applied` 顺序、重 apply 幂等 |
| 元数据过虚 | §6.3 + 量级估算 | interval tree / 不按 snapshot 全量拷贝 / 640KB 估算都到位 |
| install ingest | §10.4 | staging generation + seal + 原子切换，路径正确 |
| API 过窄 | §4.2 | discard/flush/stat/iterate/pg lifecycle/双 snapshot API 都有 |
| 多 PG GC | §11.2–11.3 | 双 waterline + segment summary + 批量 live 判断 |
| 为何不是修 blobstore | §15 | A/B/C 对比清楚，长期选 C 有依据 |
| 过早冻 format | §6.2 / M0 | `format_version` + reserved，M1/M2 后再冻 |

§8 把 “data record 是真相、metadata 只是加速、callback 成功前不能推进 applied” 写硬，是这版最重要的正确性补强。

---

## 3. 仍建议在开写前收紧的点

### 3.1 image 时间线的写入语义还不够硬（高）

现在 record/extent 同时带：

- `raft_apply_index`
- `image_snap_seq`

读路径也分了 raft / image 两套规则，但**写时 `image_snap_seq` 到底表示什么**仍偏含糊。

需要钉死一种模型，例如：

```text
create_image_snapshot(S):
  只 pin S，不拷贝数据

write(ctx.image_snap_seq = current):
  表示“该写对 image snap <= current-1 不可见，对 head / snap>=current 可见”
  或等价地：extent.image_create_seq = current
```

并明确这些边界：

1. 连续两次写、`image_snap_seq` 相同、`raft_apply_index` 不同：image snap 读应看到哪次？  
   （通常是同一 snap epoch 内后者覆盖前者。）
2. `image_snap_seq = 0` 的 write/read 语义（禁用 image 版本？只写 head？）
3. `store_read_ctx` 若 `raft_read_index` 与 `image_snap_seq` 都非 0：直接报错（正文已倾向禁止，API 层应写成硬约束）。
4. image 跨 PG 时，boundary 由控制面保证；store 只信任本 PG 收到的 seq，不做跨 PG 校验——建议写死。

不先定这个，§9.3 的 version selection 实现时会对不齐。

### 3.2 `versioned_extent_tree` 的“一树双键”要给实现处方（高）

§6.3 建议 interval tree + 版本链，很好；但每个 extent 同时有 raft/image 两个 create 戳时，要回答：

- 树按 logical range 索引，节点上挂 **一条链还是两条视图**？
- raft 读与 image 读是否共用物理 extent 节点、仅比较字段不同？
- tombstone 是否也双时间线（`raft_delete_index` / `image_delete_seq`）？  
  §7.3 仍写 “delete seq”，与 §6.3 字段不完全对齐。

建议补一小段伪规则（实现者可直接照着写）：

```text
raft_visible(e, r):
  e.raft_create_index <= r
  AND (delete 不存在 OR raft_delete_index > r)
  AND 同 range 内无更大 create_index 仍 <= r 的 extent

image_visible(e, s): 同上，换 image_* 字段
```

range tombstone 的覆盖规则也要同样拆成两套。

### 3.3 InstallSnapshot staging 的物理落点与回收（高）

§10.4 状态机方向对，但还缺“落在磁盘上长什么样”：

| 问题 | 为何重要 |
|------|----------|
| staging 用新 `store_epoch` / generation，还是同 epoch 下 shadow root？ | 决定 record 过滤与切换原子性 |
| staging 数据进共享 segment 还是独立 segment 集合？ | 决定失败丢弃成本与 GC |
| active root 指针写在 manifest 哪一项？ | 决定 crash 后看到哪一代 |
| seal 成功后旧 generation 何时可删？ | 空间与回退窗口 |
| 空间不够时 reject ingest 还是暂停前台写？ | 生产行为 |

建议补一张极简图：

```text
manifest.pg[pool,pg].active_generation = G
manifest.pg[pool,pg].staging_generation = G+1 | none
```

并把 “丢弃未完成 staging” 写进 §12 recovery 步骤。

### 3.4 Snapshot reader / pin 生命周期 API 仍缺（中高）

正文要求 pin、可取消、install 结束释放，但 §4.2 只有：

- `create_raft_snapshot`
- `open_raft_snapshot_reader`

缺少至少：

```text
release_raft_snapshot(index)
snapshot_reader::next_chunk / close / cancel
```

以及策略：**每个 PG 是否最多保留 1 个 raft recovery snapshot pin**（强烈建议首版是，避免 GC waterline 被历史 pin 拖死）。

image snapshot 的 delete/unpin API 也还没有；长期 clone/GC 会用到，至少先列 “M3 补”。

### 3.5 §11.4 与 §11.3 表述不一致（中，好改）

§11.3 已禁止 per-extent 跨核 RPC，并要求按 PG 批量判断；  
§11.4 第 3 步仍写 “对每个 extent 查询 PGStore 是否仍 live”。

建议改成与 11.3 一致的批量流程，避免实现时走回慢路径。

### 3.6 空间压力与 ENOSPC 契约（中）

目前 backpressure 主要出现在 install staging。前台写路径还需要：

- free segment 低于阈值：触发 GC / 限速 / 对 apply 返回 ENOSPC
- ENOSPC 时 raft apply 失败如何向上暴露（不可静默丢）
- GC 赶不上 overwrite 时的保护（拒绝新 snapshot pin？强制 compact？）

首版可以很保守，但要有明确行为，否则故障注入和集群测试无法判定 “正确失败”。

### 3.7 对齐、object 大小、稀疏语义仍空（中，M1 前够用即可）

M1 原型前建议最少写三行：

```text
对外 offset/len：4K 对齐（或 512，二选一）
object 最大尺寸：固定上限（例如 4MiB）或稀疏无上限
未写入 range：读返回零；discard 后同样返回零
```

否则 extent map、recovery chunk、性能测试基线都不好定。

### 3.8 Checkpoint 粒度与恢复时间（中）

架构里 CheckpointManager 挂在 OSDStore 下，但未说明：

- checkpoint 是 **per-PG** 还是 **全局**？
- 多 PG 时 replay 是否按 segment 顺序扫全盘再分发给各 PGStore？
- open segment 的 “torn tail 截断” 是否 per-segment、是否影响其他 PG 的已 seal 数据？

首版可定：

```text
allocator/manifest：全局 checkpoint
ObjectMap：per-PG checkpoint 或全局 checkpoint 内嵌 per-PG section
replay：扫 checkpoint 之后的 sealed+open segments，按 pool/pg/epoch 路由
```

并给一个粗恢复目标（例如单盘 N 分钟内可服务），避免 M2 才发现 replay 过慢。

### 3.9 幂等规则还可再收紧一点（中）

§8 已很好。建议再补两条边界：

1. **同一 `raft_apply_index` 下多 object / 多 range**：按 `(object_id, logical_range, raft_apply_index)` 幂等，而不是 “整个 index 只能有一条 record”。
2. **replay 乱序**：segment 物理序不一定等于 raft index 序（多 open segment / GC rewrite）。  
   文档写了 “以 raft_apply_index 顺序发布”，需明确：  
   - 是 recovery 时先收集再排序发布，还是  
   - 强制同 PG 的用户写只追加到单个 open segment 保证近似有序？

GC 搬迁会产生新 physical record 但逻辑 index 旧，**绝对不能**再当新 apply。GC rewrite record 需要单独 `record_type` 或 flag，避免和用户写混淆。

### 3.10 Shadow mode 的失败语义（低，M4 前）

双写时：

- blobstore 成功、append-only 失败 → 以谁为准？shadow 是否降级？
- 只对比 head 数据，还是也对比 snapshot reader 输出？
- divergence 是 assert 杀进程还是打标禁 GC？

M4 前写清即可，不挡 M1。

---

## 4. 小问题 / nits

1. **§7.3** “delete seq” 应改成 raft/image 双字段，与 §6.3 对齐。
2. **`pg_store_id.store_epoch`**：谁分配（mon / OSD 本地）？`delete_pg` 后 epoch 如何递增？建议一句。
3. **`iterate_objects` 回调模型**：同步灌完 vs cursor；RTC 下必须是 async cursor + 限量 batch。
4. **metadata delta** 仍出现在写路径，但正确性不依赖它——可标明 “可选优化，M1 可先不做”。
5. **量级估算** 很好；建议加触发阈值草案：`extents_per_object > 256` 或 `avg extents/read > 8` 启动 compact。
6. **风险章** 可补一条 “双时间线实现复杂度 / 语义 bug”，现在风险仍偏 GC/替换。
7. `append_only_store_design_comments.md` 里上一轮 P0 可视为 **大部分 resolved**。

---

## 5. 对照上一轮优先级：当前状态

| 项 | 状态 |
|----|------|
| P0 Device layout | **已解决** |
| P0 seq 拆分 + durability | **已解决**（image 写入语义还差最后一钉） |
| P0 ObjectMap 结构 + 预算 | **基本解决**（双键选择规则需伪代码） |
| P0 install follower 状态机 | **基本解决**（staging 落盘/回收需补） |
| P1 API 补齐 | **大部分解决**（reader cancel/release、unpin 仍缺） |
| P1 多 PG GC | **已解决**（11.4 措辞对齐即可） |
| P1 object_id | **已解决** |
| P1 路线对比 | **已解决** |
| P2 量级/versioning | **已推进** |
| P2 对齐/ENOSPC/checkpoint 粒度 | **仍待补** |

---

## 6. 建议在 M1 编码启动前完成的 delta 修订

不必再扩写成长文，只补以下 7 点：

1. **image_snap_seq 的写/读可见性规则**（含 `0` 的含义）
2. **version/tombstone 双时间线选择伪代码**
3. **staging generation 在 manifest 中的表示与 recovery 丢弃规则**
4. **snapshot pin/release/reader cancel API** + 每 PG 最多 1 个 raft snapshot
5. **对齐 / object size / ENOSPC 三行政策**
6. **GC rewrite record 与用户 record 的区分**
7. **§11.4 与 §11.3 措辞对齐**

补完这 7 点后，**M0 可以宣布“语义冻结、format 仍 versioned 可演进”**，然后专心做 M1：单 PG 写读删除 + checkpoint/replay + crash 注入。

---

## 7. 结论

这版更新质量很高：**P0 骨架已经立住**，尤其是 device 边界、双时间线、durability+幂等、ingest 状态机、GC 批量与 segment summary。

作为设计草案，已经达到 **“团队可以按 M1 开工”** 的标准。

当前最大剩余风险不再是架构方向，而是：

1. **双时间线（raft / image）在 extent 与 tombstone 上的精确可见性规则**
2. **install snapshot staging generation 的落盘与 crash 回收**
3. **pin/release、ENOSPC、checkpoint 粒度等实现契约**

建议：短 delta 修订上述 7 点 → 宣布语义冻结 → 进入 M1 原型，用故障注入验证 §8 与 §12，而不是继续在文档里扩 scope。

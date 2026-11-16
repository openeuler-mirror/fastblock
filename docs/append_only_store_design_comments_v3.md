# append_only_store_design.md 第三轮设计评审意见

日期：2026-08-11

评审对象：`docs/append_only_store_design.md`（第三版更新）

上一轮评审：

- `docs/append_only_store_design_comments.md`（第一轮）
- `docs/append_only_store_design_comments_v2.md`（第二轮）

本文是对再次更新后设计草案的第三轮评审。总体结论：**v2 提出的 7 点 delta 已基本落地，文档达到可宣布语义冻结并开 M1 的状态。**

---

## 1. 总评

这版是认真按第二轮意见改的。正确性骨架已经完整：

- device 边界
- 双时间线
- durability
- 幂等
- install ingest
- GC rewrite
- ENOSPC
- 对齐 / 对象大小

首版约束够保守（4K、4MiB、单 raft snapshot pin），适合先做可验证原型。

**可以进入 M0 语义冻结 + M1 编码。**

剩余问题多为实现细则 / 边界 case，不必再大改章节结构。

---

## 2. 对照 v2 七项：完成度

| v2 要求 | 本版位置 | 状态 |
|--------|----------|------|
| image 写/读可见性 + `0` 含义 | §5.2 | **已解决** |
| 双时间线可见性伪代码 + tombstone 双字段 | §6.3、§7.3 | **已解决** |
| staging 在 manifest 中的表示 + recovery 丢弃 | §10.4、§12.1 | **已解决** |
| pin/release、reader cancel、每 PG 最多 1 个 raft snap | §4.2、§10.1 | **已解决** |
| 4K 对齐 / 4MiB object / ENOSPC | §7.4、§7.5 | **已解决** |
| GC rewrite 与用户写区分 | §6.1、§6.3、§11.4、§12.1 | **已解决** |
| §11.4 与批量 live 判断对齐 | §11.4 | **已解决** |

额外加分项：

- `store_epoch` 本地分配与重建递增（§4.2）
- `iterate_objects` 强制 async cursor（§4.2）
- metadata delta 标明 M1 可不做（§7.1）
- compaction 阈值 256 / 8（§7.2）
- checkpoint 粒度 + staging 丢弃进 recovery 路径（§12.1）
- 风险章补双时间线（§19.6）

上一轮的“挡开工”问题，这版基本清完了。

---

## 3. image 可见性公式核对

对 §6.3 的公式做了对照，与 §5.2 写入语义一致：

| 场景 | 预期 | 公式结果 |
|------|------|----------|
| `image_snap_seq=0` 的写 | 不进 image snap 视图 | `create_seq != 0` 过滤掉 |
| 同 `image_snap_seq` 两次覆盖写 | 较大 `raft_apply_index` 胜出 | image 分支的 tie-break 正确 |
| snap `S` 读 | 见 `create_seq <= S` 的最新写 | 与 “对 ≤ current-1 不可见” 一致 |

raft 侧伪代码与 §9.2 文字规则也一致。这块可以认为**可编码**。

---

## 4. 仍建议收紧的边角（不挡 M1，但最好写死）

### 4.1 `create_image_snapshot(S)` 与写路径 `current` 的约定（中）

§5.2 已写：

```text
write(current): 对 snap <= current-1 不可见
image_visible: create_seq <= s 且 create_seq != 0
```

自洽的前提是控制面约定清晰，建议在 §5.2 再加一句硬约定，例如：

```text
create_image_snapshot(S) 创建后，后续写必须携带 image_snap_seq >= S+1
创建前的写应携带 image_snap_seq <= S
store 不负责跨请求校验单调性，但单 PG 内若发现 image_snap_seq 回退，记错误日志/计数
```

否则实现和前端会对 “snapshot 那一刻 current 是 S 还是 S+1” 产生分歧。

### 4.2 staging `sealed 但未 active` 的 recovery 规则仍偏软（中）

§10.4 提到 sealed 但未 active 时，按 generation/checksum 判断是继续切换还是丢弃。

实现需要**确定性**，建议改成二选一，例如：

```text
若 staging 已 sealed 且 manifest 中 seal checksum 完整：
  完成 active 切换，旧 generation 进 pending_gc
否则：
  丢弃 staging
```

不要留 “判断” 空间，故障注入才好写期望结果。

### 4.3 幂等粒度可再钉一句（低）

§8 已说同 object/range + 同 `raft_apply_index` 合并。建议显式写：

```text
幂等键 = (store_epoch, object_id, logical_range, raft_apply_index)
同一 index 下多 object / 多 range 合法
GC rewrite 不参与该幂等键，只修 physical pointer
```

与 §12.1 的 “按 raft_apply_index 排序发布” 一起，足够 M1 实现。

### 4.4 同 PG 多 open segment 时的 replay 序（低）

若同 PG 用户写允许跨多个 open segment，物理序 ≠ raft 序，必须 **先收集再按 index 排序发布**（§12.1 已倾向如此）。

可补一句工程约束（二选一即可）：

- M1：每 PG 仅 1 个 open data segment（实现简单），或
- 允许多 segment，但 recovery 必须全局排序后再 publish。

### 4.5 Shadow mode 失败语义（低，M4 前）

仍未写：blobstore 成功 / append-only 失败时以谁为准、divergence 是 assert 还是降级。

**不挡 M1**，M4 开工前补半页即可。

### 4.6 `pending_gc` 旧 generation 的释放条件（低）

active 切换后旧 generation 何时真正 free：

- 无 snapshot reader
- 无未完成 apply
- manifest 已持久化新 active

建议一句，避免 install 成功后空间一直挂着。

### 4.7 API 小缺口（nit）

- `open_image_snapshot_reader` 未提供（M1 可不做，但和 raft 不对称；可标 M3）
- `snapshot_reader` 如何从 `open_raft_snapshot_reader` 交到调用方（callback 带句柄即可，实现期再定）
- `create_pg` 的 `store_epoch`：正文说 OSD 分配，则 API 可能应是 `create_pg(pool, pg)` 返回完整 `pg_store_id`

---

## 5. M0 / M1 建议

### 5.1 M0 冻结范围

```text
语义冻结：
  device layout、双时间线、durability、幂等、
  snapshot pin 模型、install ingest、GC rewrite、ENOSPC、对齐/对象大小

仍 versioned、可演进：
  record 二进制 layout 细节、checkpoint 编码、
  shadow mode 失败策略、image reader API
```

### 5.2 M1 范围建议

保持文档现状，并显式砍掉：

- 不做 image snapshot pin（接口保留）
- 不做 metadata delta
- 可不做多 PG / GC cleaner（GC rewrite 类型先定义）
- 单 PG、单 open data segment 可进一步降低复杂度

### 5.3 M1 必测

1. 随机 4K 写读 + overwrite + discard
2. data 写一半 / 写完未 callback / checkpoint 写一半
3. 同 `raft_apply_index` 重复 apply 幂等
4. replay 后与 crash 前 head 一致
5. 统计 extent 数 / metadata bytes（为后续 compaction 留基线）

---

## 6. 三轮演进对照

| 轮次 | 文档状态 |
|------|----------|
| 初稿 | 方向对，P0 缺口多 |
| 第二版 | P0 骨架立住，7 点 delta 待补 |
| **本版** | **delta 落地，可语义冻结并开 M1** |

---

## 7. 结论

这已经是一份**可执行的存储引擎设计**，而不是概念稿。

剩余最大风险不再是架构方向，而是实现纪律：

1. 严格按 §8 durability 与 §12 recovery 实现
2. 严格按双时间线伪代码做可见性判断
3. 用故障注入锁住 crash 窗口行为

建议：

- 可选：把 §4.1–4.4 的几句硬约定补进设计正文
- 然后宣布 **语义冻结、format versioned 可演进**
- 进入 M1：单 PG 写读删除 + checkpoint/replay + crash 注入

不建议在文档阶段继续扩 scope（压缩、跨 PG 事务、完整 CSI、ZNS 生产路径等都应留在后续里程碑）。

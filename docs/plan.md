# fastblock 后续开发计划

日期：2026-08-21  
状态：**执行中（从 Phase 0 立刻开工）**  
代号：`engine_signoff_then_product`  
读者：实现者。本文是排期与验收，不是再开一轮存储/网络设计。

对照文档（本文不重开它们的设计）：

| 文档 | 角色 |
|------|------|
| `docs/append_only_store_design.md` | AOS 平台。P1–P4、P6 已完成。P5 控制面是本文 Phase 2。 |
| `docs/aos_unified_log_design.md` | log=STM。已合入，默认开启。 |
| `docs/rdma_rpc_fail_soft.md` | RDMA 调用方合同。已落地。 |
| `docs/msg_sock_transport_design.md` | sock 后端。PR1–PR5 已合入。剩余是签收，不是 PR6 uring。 |
| `docs/complete_rtc_design.md` / `docs/rtc_implementation_status.md` | RTC 主路径已完成。可选 PR-7/8/9 不进本计划。 |
| `docs/raft_recovery_and_snapshot.md` | install snapshot 机制。回归脚本已有，交叉故障未签收。 |
| `docs/kfastblock_raw_tcp_design_20260423.md` / `kfastblock/TASKS.md` | 内核客户端。Phase 4 才碰。 |
| `docs/system_self_test_report_20260415.md` | 已知失败：选主中 snapshot、成员变更联动。Phase 1 必须关掉。 |

---

## 0. 当前树的事实（不要靠记忆）

已经落地、本计划不当成缺口：

- AOS 是唯一 localstore（object / log / kv）；生产不编 blobstore。
- 用户写只落一条 data/tombstone（log=STM）；apply 只 `publish_commit`。
- Data/Raft RPC 有两套后端：`msg_transport=rdma|sock`。无 RNIC 时 vstart 写 sock。
- RTC 主路径：sticky home shard、local-first process+raft、get_leader home-safe、COW shard table。
- OSD 数据面已有 `process_image_snap`（CREATE/CLONE/RELEASE/FLATTEN）和 `pg_store` pin/clone/flatten。
- 用户态接入可用：vhost、nvmf、`libfblock`、`block_bench`。
- 已有回归脚本：`scripts/run-dev-write-read-verify.sh`、`run-aos-restart-verify.sh`、`run-raft-dev-regression.sh`、`run-raft-install-snapshot-regression.sh`。

还没做、本计划要做：

- sock 集群在断连 / 选主 / 恢复 suffix 后仍有未合入的修复（工作区 `msg-sock`）。
- Monitor **没有任何** snap/clone API。`ImageInfo` 只有 `poolname/imagename/size/object_size`。`fastblock-client` 只有 create/remove/resize/get image。
- CSI、NBD、kfastblock 都不能当产品入口。kfastblock attach 返回 `monitor bootstrap protocol is not implemented yet`。
- 对象锁是整对象 type 锁（`op_type_excl_lock.h` `TODO: range lock`）。
- `rpc_controller::StartCancel` / `NotifyOnCancel` 是空实现。
- `raft_index_t` / `raft_term_t` 仍是 `long int`。

---

## 1. Locked Product Calls

下列决定已拍板，执行中不再重开：

| # | 决定 |
|---|------|
| L1 | **不再开存储引擎或运输层重写。** AOS、log=STM、RDMA fail-soft、`msg::sock` 是终态。修 bug，不换架构。 |
| L2 | **立刻从 Phase 0 开工。** 先合入并签收工作区里的 sock/raft 修复，再做产品面。 |
| L3 | **开发机默认 sock，有 mlx5 默认 rdma。** 禁止混运输集群。不把 sock 宣传成生产快路径。 |
| L4 | **Phase 1 签收之后冻结合同。** `layout_version`、sock 帧、RDMA RPC `error_class`、image_snap 语义不再「开发阶段随便破坏」。需要破坏时单独写兼容说明。 |
| L5 | **卷快照控制面先于 CSI。** Monitor 编排 `image_snap_seq`，OSD 走现有 `process_image_snap`。CLI 可测后，CSI 只消费同一套 API。 |
| L6 | **计算侧产品入口只做 CSI。** CSI 走现有用户态：monitor + `libfblock` + nvmf（或 vhost）。不为此新开数据协议。 |
| L7 | **kfastblock 后置，且不与 CSI 并行。** 它需要 raw TCP，与 `msg::sock` 无关，禁止拿 sock 去「顺便」做内核。 |
| L8 | **不增长 `raw_tcp_server` / `fbraw_hdr` 给 Raft 或用户态客户端。** |
| L9 | **write ring、sock uring、ZNS、加密、QoS、多 attach、DPU 卸载不进本计划。** 需要时另立文档。 |
| L10 | **在线 PG 跨核迁移、动态改 `OSDCoreNum` 不做。** RTC 非目标保持。 |

---

## 2. 工作方式

1. **一阶段一门。** Phase N 的门没过，不开 Phase N+1 的功能 PR。修 Phase N 里发现的引擎 bug 可以插队。
2. **每个 PR 可独立 revert。** 不把「修 sock + 加 snap CLI + 改 raft 类型」捆一条。
3. **默认在 sock 集群上验收。** 有 mlx5 时 Phase 1 末再跑一遍 rdma，证明运输分叉没有把 raft/STM 语义带偏。
4. **验收必须含数据校验。** 只看 pg active / 日志关键字不算过。
5. **发现要破坏 L4 冻结项：先改本文，再改代码。**

默认开发拓扑（与 vstart 一致）：

```text
1 mon + 3 OSD + 1 nvmf（或 block_bench）
msg_transport=sock
replica=3，PG 数 ≥ 3
FASTBLOCK_VSTART_SPDK_ARGS=--no-huge   # 本环境无 hugepage 时
```

---

## 3. 阶段总览

```text
Phase 0  sock/raft 未合入修复     ← 现在就开始
Phase 1  引擎签收（故障交叉 + 回归）
Phase 2  卷快照控制面（mon + CLI）
Phase 3  CSI（用户态，消费 Phase 2）
Phase 4  kfastblock 内核入口（独立，不挡 2/3）
Phase 5  引擎打磨（range lock / RPC cancel / index 类型）
Phase 6  以后再说（本文只登记，不排期）
```

Phase 4 可以在 Phase 3 之后启动，**不可以**和 Phase 0–2 抢人。Phase 5 的单项若在 Phase 1 成为 blockers（例如 cancel 导致 nvmf abort 打挂），允许提前抽一条 PR，但范围只限那个洞。

---

## Phase 0 — 合入 sock/raft 活性修复（立刻做）

**目标：** 把 `msg-sock` 工作区里已经写出的修复变成可回归的提交。这不是新功能。

当前未提交（以工作区为准，合入前按逻辑拆 PR）：

### PR 0.1 — sock：连接表回收 + 请求克隆

- **文件：** `src/msg/{client,server,connection,cm_pool}.cc`，`src/include/fastblock/msg/sock/options.h`
- **内容：**
  - client/server poll 与 accept 前 `erase_if` 已 `is_terminated()` 的连接。`max_conns` 按活连接计。
  - `CallMethod` 先 `CopyFrom` 再 `peel_rpc_payload`。`fb_client` 在 `NOT_LEADER` 后重试同一 `write_request` 时，禁止把调用方 bytes 剥空。
  - posix `zcopy` 默认 false（loopback 上 zcopy 是坑，不是优化）。
  - `rpc_task` 持 `connection` 的 `shared_ptr`，方法返回后连接不得先于 reply 析构。
- **不改：** Raft 协议、OSD STM。
- **验收：**
  - `sock_frame_test` 仍过。
  - ping_pong / 4MiB / 未知 correlation / timeout 恰好一次。
  - 人为 RST 一条连接后再建连，server `max_conns` 不因为僵尸连接而 `accept_drop`。
  - 复现：同一 `write_request` 剥一次再 `CallMethod` 第二次，OOB 非空。

### PR 0.2 — raft：死 stub 重连 + 恢复后 election NOOP 能 flush

- **文件：** `src/raft/raft_client_protocol.{h,cc}`，`src/raft/raft.h`，`src/raft/raft_log.h`，`src/raft/raft_server.cc`
- **内容：**
  - `_get_stub` 发现 cache 连接已 terminated 则丢掉 stub。
  - `send_*` 拿不到 stub 时 `maybe_reconnect`（单 node 飞行中只一次）。
  - `raft_task_func` 只在「已 flush 批次等 commit」（`current > commit && last_cache == current && !has_queued_entries()`）时跳过。恢复 suffix 后 queue 里的 election NOOP 必须能第二次 flush。
- **验收：**
  - 3 OSD sock 集群：杀一个 follower OSD → 重启 → PG active，对该 PG 写读校验通过。
  - 杀 leader → 新主选出 → 写读校验通过。
  - 日志可见 `raft reconnect to node`，且重连后 AE/heartbeat 恢复，不是永远 `RAFT_ERR_NO_CONNECTED`。

### Phase 0 门

```bash
# 构建（无 mlx5 的箱子）
cmake -DFASTBLOCK_WITH_RDMA=OFF && ninja fastblock-osd sock_frame_test
# 有 mlx5 则 FASTBLOCK_WITH_RDMA=ON，默认运输仍随 RNIC

./vstart.sh -m dev -c 3 -r 3
./scripts/run-dev-write-read-verify.sh
./scripts/run-aos-restart-verify.sh
```

杀 OSD / 选主场景在 PR 0.2 的验收里跑。过门之前不开 Phase 1 的新脚本需求以外的功能。

---

## Phase 1 — 引擎签收

**目标：** 在 sock 上证明「写、重启、落后 follower、选主、成员变更」可以组合发生，数据仍对。这是 L4 冻结的前提。

### PR 1.1 — 回归入口收口

- **文件：** `scripts/` 下新增一个总入口，例如 `scripts/run-engine-signoff.sh`；必要时薄封装现有脚本。
- **内容：** 固定顺序调用已有脚本，失败即停，打印拓扑与 `msg_transport`。
  1. vstart 3 OSD sock
  2. `run-dev-write-read-verify.sh`
  3. `run-aos-restart-verify.sh`
  4. `run-raft-dev-regression.sh`
  5. `run-raft-install-snapshot-regression.sh`
- **不改：** 产品代码，除非脚本暴露出 Phase 0 没盖住的 bug（bug 修在本 PR 或插队小 PR）。
- **验收：** 一条命令从干净 vstart 跑完，exit 0；过程无 core。

### PR 1.2 — 选主 × snapshot / 选主 × 成员变更

对照 `docs/system_self_test_report_20260415.md` §8 P0。

- **文件：** 新脚本 `scripts/run-leader-change-snapshot-regression.sh`、`scripts/run-leader-change-membership-regression.sh`；相关 `src/raft/**`、`src/osd/**` 只在测试红时改。
- **场景 A（必须绿）：** install snapshot 进行中重启当时的 leader。新主继续或中止后重来，follower 追平，对象 read-verify 通过。不允许无限 `_recovery_by_snapshot`。
- **场景 B（必须绿）：** `change_nodes` 进行中原 leader 退出。`pgmap` 收敛到目标成员，变更后再写读通过。Monitor 不得因锁/完成通知卡住 `GetClusterMap`。
- **场景 C（必须绿）：** 先 snapshot recovery，再成员变更（或反过来），最终数据对。
- **验收：** 三个脚本稳定绿（连续两轮）。失败时保留 OSD log 与 `status/getpgmap`。

### PR 1.3 — nvmf 路径在 sock 上的故障回归

- **文件：** `scripts/` + 现有 nvmf/vhost 启动方式。不改协议。
- **场景：** nvmf 写过程中杀一个 replica OSD；杀 leader；OSD 重启后继续 fio/block_bench 写读校验。
- **验收：** 无 D-state、无 reactor 永久卡住；I/O 以可重试错误恢复，最终数据对。

### PR 1.4 —（有 mlx5 时）rdma 对照

- 同一组脚本把 `msg_transport=rdma` 再跑一遍。
- **验收：** 不要求性能数字；要求语义与 sock 一致（选主、recovery、写读校验）。无 RNIC 的箱子跳过，在脚本里显式打印 skipped。

### Phase 1 门

- PR 1.1–1.3 绿。
- 已知报告里的「leader 切换中 snapshot 失败 / 成员变更失败」关闭或降为有工单的非门禁项（必须写明复现与理由，默认不允许降级）。
- 此后适用 L4：布局与 RPC 合同冻结。

---

## Phase 2 — 卷快照控制面（AOS P5）

**目标：** 用户能对一个 image 做 snapshot / clone / release / flatten。数据面已经会了；缺的是 monitor 分配 `image_snap_seq` 并指挥每一个 PG。

### 2.1 语义（实现时按此写，不另开设计长文除非打脸）

```text
image  (pool, name, size, object_size)
  └── snaps[]  { snap_seq, name, parent_seq, created_at, state }

CREATE  mon 分配 snap_seq = max(existing)+1
        对 pool 内每个 PG 的 leader 调 process_image_snap(CREATE, snap_seq)
        全部成功才在 etcd 提交 ImageInfo.snaps
        部分失败：重试未成功 PG；仍失败则标记 snap state=failed，不把 seq 暴露给新写

写路径  libfblock / bdev 对「当前 image 头」带 image_snap_seq=head
        对「只读 snap」的读走 view_at(snap_seq)；禁止对 snap 写

CLONE   新 image 记录 parent=(pool, name, snap_seq)
        对每个 PG process_image_snap(CLONE, snap_seq)
        新 image 的后续写使用更大的 image_snap_seq（mon 分配）

RELEASE 无 clone / 无头依赖才允许
        每个 PG process_image_snap(RELEASE, snap_seq)
        成功后从 ImageInfo.snaps 删除；GC 之后才能回收 extent

FLATTEN 对 clone 的每个 PG process_image_snap(FLATTEN, parent, new_seq)
        成功后 clone 与 parent 解耦
```

约束：

- `image_snap_seq` 是 **image 级** 单调值，不是 raft index，不是 PG 本地值。所有 PG 必须用 mon 发下来的同一个 seq。
- OSD 已有 RPC，**禁止**再发明第二套 pin 协议。
- 不在 Phase 2 做跨 pool clone、snap 组、crash-consistent 多盘组。
- flatten 的 `raft_write_index`：由各 PG leader 在本 PG 上取当前可提交 index 填入，mon 不编造一个全局 raft index。

### PR 2.1 — proto + etcd 元数据

- **文件：** `proto/messages.proto`，`monitor/osd/image.go`，etcd key 前缀（`monitor/config`）。
- **内容：**
  - `ImageInfo` 增加 `int64 image_id`、`uint64 head_snap_seq`、`repeated ImageSnap snaps`。
  - `ImageSnap`：`uint64 snap_seq`、`string snap_name`、`uint64 parent_seq`、`enum state`。
  - 新请求：`CreateImageSnap` / `ListImageSnap` / `RemoveImageSnap` / `CloneImage` / `FlattenImage`。
  - etcd 与现有 image key 同前缀演进；开发阶段无旧集群义务，但仍写 load 时缺字段 = 无 snap 的兼容。
- **验收：** monitor 单测：创建 image 后 head_snap_seq=0；序列化往返。

### PR 2.2 — monitor 编排 OSD RPC

- **文件：** `monitor/`（leader 侧），必要时 OSD 侧只加「按 pool 列 PG / 找 leader」已有能力的调用点。
- **内容：**
  - monitor **不**直接链 `msg::sock`。沿现有 monitor↔OSD 控制面（已有 boot/heartbeat/pg 通道）下发；若现有通道塞不进 `process_image_snap`，则 **monitor 让 `fastblock-client` 或一个小的用户态 helper 走数据 RPC**——选一条，写进本 PR 的 README，不要两条并存。
  - 推荐（实现时按此，打脸再改）：**`fastblock-client` 作为编排器**：mon 只做元数据事务；client 从 clustermap 拿每个 PG leader，用现有 `msg` factory 调 `process_image_snap`。理由：mon 是 Go+TCP 控制面，数据 RPC 已在 C++ 客户端。
- **验收：** 单 pool、3 PG：CREATE 后每个 PG 的 pin 计数可查（OSD log 或 dump）；故意停一个 leader 时 CREATE 返回失败且 etcd 无半成品 snap 名。

### PR 2.3 — CLI

- **文件：** `monitor/fbclient.go`
- **内容：**
  ```text
  -op=createsnap  -poolname= -imagename= -snapname=
  -op=listsnap    -poolname= -imagename=
  -op=rmsnap      -poolname= -imagename= -snapname=
  -op=cloneimage  -poolname= -imagename= -snapname= -newimagename=
  -op=flatten     -poolname= -imagename=
  ```
- **验收：** 脚本化：写数据 → createsnap → 再写 → 从 snap clone → 读 clone 得到 snap 时点数据，读原 image 得到新数据。

### PR 2.4 — 只读 snap 读路径

- **文件：** `src/include/fastblock/client/fb_client.h`、`libfblock`、bdev 打开参数。
- **内容：** 打开 image 时可指定 `snap_name` / `snap_seq`。读带 `image_snap_seq`；写返回 `-EROFS`。
- **验收：** bdev 或 `block_bench` 打开 snap 只读，verify 与创建 snap 前写入一致。

### Phase 2 门

- CLI 四操作 + clone 读校验绿。
- 与 Phase 1 的 restart / 杀 OSD 组合：snap 元数据在 mon 重启后仍在；PG 恢复后 pin 仍在，clone 读仍对。
- GC：release 之后，后续 `gc_pass` 可回收不再 live 的 extent（允许用 `fastblock-aos-dump` 证明，不必一上来压测吞吐量）。

---

## Phase 3 — CSI

**目标：** Kubernetes 能创建 PVC、挂到 Pod、读写 fastblock image。数据面不新做。

依赖 Phase 2（VolumeSnapshot 用同一套 snap API）。没有 Phase 2 只做「动态建 image + nvmf attach」也可以开工 **Controller 建卷**，但 Snapshotter 必须等 2.3。

### PR 3.1 — 仓库与 CSI Identity/Controller 骨架

- **位置：** 新建 `csi/`（Go）。不要把 CSI 塞进 `monitor/`。
- **内容：** Identity、Controller `CreateVolume` / `DeleteVolume` / `ControllerPublishVolume`。CreateVolume = 现有 `createimage`。Publish = 在节点上保证 nvmf subsystem/target 可连（复用 `fastblock-nvmf-tgt` + `bdev_fastblock_create`）。
- **不做：** 内核模块、第二套 RPC。
- **验收：** 单节点 kind/kubelet 或 csi-sanity 的 CreateVolume/DeleteVolume 过。

### PR 3.2 — Node 挂载

- **内容：** `NodeStageVolume` / `NodePublishVolume`：nvme connect → 块设备 → bind mount 到 target path。Disconnect 成对。
- **验收：** Pod 里 `mkfs` + 写文件 + 删 pod 再挂，文件还在（同一 PVC）。

### PR 3.3 — VolumeSnapshot

- **内容：** CSI Snapshotter 调 Phase 2 CLI/API。Restore = cloneimage + 新 PVC。
- **验收：** 写 → VolumeSnapshot → 新 PVC 读到旧数据；原 PVC 继续写不影响快照。

### Phase 3 门

- 文档：最小 YAML（StorageClass、PVC、Pod、VolumeSnapshot）。
- README 里「客户端有 CSI」从宣传变成可跟做步骤。本阶段结束后，**NBD 仍不做**（SPDK nbd 导出如有需要用现成 bdev nbd，不写新客户端）。

---

## Phase 4 — kfastblock（独立轨道）

**目标：** 内核块设备 attach image，不经用户态 libfblock。按 `docs/kfastblock_raw_tcp_design_20260423.md` 与 `kfastblock/TASKS.md`。

前置：Phase 1 已冻结用户态合同，避免内核对着还在改的 OSD 协议打靶。

### 顺序（不要一次合入）

1. **Monitor 聚合接口：** `GetClusterMap` / `GetImageInfo` 对内核够用（pool_id、object_size、osd 地址与 raw_port、pg→osd）。修用户态 `object_size` 与 4MiB 切分不一致。
2. **OSD raw 数据面：** 现有 `raw_tcp_server` 的 4096 上限、thread-per-conn、只服务实验协议——按设计扩到 object 级 read/write/delete + get_leader。**仍然不是** Raft-over-raw-TCP。
3. **内核 meta bootstrap：** `meta.c` 真正拉 map；attach 不再 `-EOPNOTSUPP`。
4. **请求切分与完成：** `request.c` 按 object_size 切 bio，聚合失败/超时。
5. **故障：** leader 变化、pg remap、monitor 不可达时 freeze/fail。
6. **测试：** 单测切分 + 一台机器 fio 冒烟。

kfastblock 第一版明确不做：snapshot、多 attach、RDMA、QoS。只读 snap 用 Phase 2 的用户态路径。

---

## Phase 5 — 引擎打磨（不挡产品面）

每条独立，按痛感排序，不打包。

### PR 5.1 — RPC 取消

- **文件：** `src/include/fastblock/msg/rpc_controller.h`，rdma/sock `complete_rpc` 路径，nvmf/bdev abort。
- **内容：** `StartCancel` 把 in-flight RPC 标 `completed` 一次，`error_class=timeout`（或显式 `canceled`，若加枚举须同时改调用方）。连接不拆。
- **验收：** server 故意不回；client 取消后 `Run()` 恰好一次；连接上后续 RPC 仍成功。nvmf reset 不再靠「等 1s 时钟」才能收回。

### PR 5.2 — 对象 range lock

- **文件：** `src/osd/op_type_excl_lock.h` 及 `lock_manager`。
- **内容：** 同对象不重叠的 offset/len 允许并发（含读写不同区间）；重叠区间保持现有 type 互斥。先正确，再谈加速。
- **验收：** 单测覆盖重叠/相邻/包含；现有写读回归不回退。

### PR 5.3 — `raft_index_t` / `raft_term_t` → `uint64_t`

- **文件：** `src/raft/raft_types.h` 及所有强制转换（`raft_log.h` 的 TODO）。
- **内容：** 纯类型清理。禁止夹带行为变化。
- **验收：** 现有 raft 单测 + Phase 1 脚本。

### PR 5.4 — GC 压测与默认参数

- 长时间写 + 覆盖 + release snap，观察 segment 回收、写放大、是否打满盘。
- 产出：生产默认 rewrite 上限 / poller 周期，写进 `aos/README`。

---

## Phase 6 — 登记但不排期

需要时另立设计，不要在本计划的 PR 里「顺便」做：

| 项 | 为什么现在不做 |
|----|----------------|
| 卷 QoS | 产品策略未定，且会改 IO 入口 |
| 卷加密 | 新密钥生命周期，与 STM 无关 |
| 多 attach / 卷共享 | 需要锁与 cache 一致性 |
| ZNS | 只有 `choose_segment_size_for_device` |
| sock `io_uring` | `msg_sock` PR6，不挡签收 |
| write ring 复活 | RTC 已延后，不是主路径 |
| DPU 卸载 vhost | 硬件与发布模型 |
| 独立 kv_checkpoint slots（K1） | K2 够用 |
| 在线 PG 迁核 | RTC 非目标 |
| 混运输集群 / RDMA↔sock 网关 | P4 禁止 |
| 内网离线 RPM 依赖 | `depend.md`，与引擎正交 |
| 旧 blob 盘升级 | 无上线数据，不做迁移 |

升级/回滚：L4 冻结之后，**下一份**文档写「layout N → N+1」。本计划不发明双轨。

---

## 4. PR 总表（执行顺序）

| PR | 标题 | 依赖 | 阶段门 |
|----|------|------|--------|
| 0.1 | sock：回收连接、peel 克隆、关 zcopy | 无 | |
| 0.2 | raft：死连接重连、恢复后 NOOP flush | 0.1 建议先合 | **Phase 0** |
| 1.1 | `run-engine-signoff.sh` 串现有脚本 | 0.2 | |
| 1.2 | 选主 × snapshot / 成员变更回归 + 修红 | 1.1 | |
| 1.3 | nvmf + 杀 OSD 回归 | 1.1 | |
| 1.4 | rdma 对照（有 NIC） | 1.2, 1.3 | **Phase 1 / L4 冻结** |
| 2.1 | ImageInfo snaps proto + etcd | 1.x | |
| 2.2 | client 编排 `process_image_snap` | 2.1 | |
| 2.3 | `fastblock-client` snap/clone/flatten | 2.2 | |
| 2.4 | 只读 snap 打开 | 2.3 | **Phase 2** |
| 3.1 | CSI Controller 建删卷 | 2.x 可部分重叠 3.1 与 2.4 | |
| 3.2 | CSI Node stage/publish | 3.1 | |
| 3.3 | CSI VolumeSnapshot | 2.3, 3.2 | **Phase 3** |
| 4.* | kfastblock 按 TASKS.md | Phase 1 门 | 不挡 CSI |
| 5.1–5.4 | cancel / range lock / index 类型 / GC | 按痛感 | 不挡 2/3 |

---

## 5. 明确不在本计划出现的工作

- 新的 localstore。
- 第三套 on-wire（除 kfastblock 自己的 raw TCP 以外）。
- 把 `monitor::client` 抽成通用 RPC。
- 把 `src/rpc/server.h` 当 OSD 主路径改。
- 为无 RNIC 默认 `rdma_rxe`。
- 完整 mon RO snapshot / RTC strict / heartbeat mutex（RTC 可选，不 gate）。
- README future works 里尚未列入上表的项目。

---

## 6. 现在立刻做什么

工作区已经停在 Phase 0。下一步：

1. 把 PR 0.1 / 0.2 从当前 diff 整理成两个可审提交（先 sock，再 raft）。
2. 在 sock 的 3 OSD vstart 上跑写读、重启、杀 follower、杀 leader。
3. 过 Phase 0 门之后开 `scripts/run-engine-signoff.sh`。

不要在 0.1/0.2 里夹带 snap proto 或 CSI 骨架。

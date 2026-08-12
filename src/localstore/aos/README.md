# Append-Only Store (AOS)

设计：`docs/append_only_store_design.md`

## 铁律：SPDK 是整体框架

AOS 不是普通用户态文件库。它运行在 **fastblock / SPDK reactor** 里：

| 必须 | 禁止 |
|------|------|
| `spdk_bdev_*` 异步 IO | `pwrite` / `write` / `fsync` 当生产写路径 |
| completion callback | 在 reactor 上阻塞等 IO |
| DMA 缓冲（`spdk_malloc` / pool） | `new[]` / stack buffer 直接下盘 |
| write pointer 尾部推进 | 随机 offset 覆盖写 segment |
| `flush` = durability 屏障（§8） | 写完同步返回当 durable |

**POSIX 文件 IO 不是“先实现再移植”的合法起点。**  
唯一 IO 边界是 `device_backend`；生产实现是 `spdk_bdev_backend`。

## 分层

```text
osd_store                # 多 PG 生命周期，共享 allocator（M2 骨架）
    │
pg_store                 # object 语义、幂等、extent 发布、checkpoint
    │
segment                  # record 编码 + 对齐 + reserve/write_at + segment_seal
    │
segment_allocator        # 固定大小 free/open/sealed（可共享）
    │
device_backend           # 异步 append / write_at / read / flush
    │
spdk_bdev_backend        # spdk_bdev_write/read/flush_blocks
```

纯逻辑（可单测、无 bdev）：

- `record` encode/decode/checksum
- `extent_map` head 覆盖写

## 当前进度

| 模块 | 状态 |
|------|------|
| record / extent_map | 有 |
| `device_backend` 接口 | 有 |
| `spdk_bdev_backend` | 有（生产 IO） |
| `mem_device_backend` | 有（单测 double，同 async 形状） |
| `segment` | 有 |
| `pg_store` | 有：async write/read/remove/discard/flush/checkpoint、幂等、rebuild_from_image、recover |
| range_tombstone | 有：`object_offset` + payload u64 LE `len`，discard 后读零 |
| checkpoint_marker | 有：ObjectMap 快照 payload（`checkpoint.cc`）；recover 两阶段：找最新完整 ckpt → load → replay tail |
| `segment_allocator` | 有：固定大小 segment，reserve 不双分配；满则 seal 开下一段；`segment_size=0` 退化为整盘单段 |
| `segment_seal` | 有：段滚动前若尾部够放 header，先写 seal record |
| `osd_store` | 有：format / create/delete_pg / persist_manifest / recover |
| superblock A/B + manifest | 有：盘前部 `kMetaSize` 元数据区；epoch 表跨重启 |
| write queue depth | `spdk_bdev_backend` 默认最多 32 路并发 `write_at`（可调） |
| GC skeleton | `osd_store::gc_once`：无 live 引用的 sealed segment 回 free |
| GC rewrite (M6) | `pg_store::rewrite_extent` + `osd_store::gc_rewrite_once`：gc_rewrite 搬迁 head extent，再 free |
| raft snapshot (M3) | `create/release_raft_snapshot` O(1) pin；`read(..., raft_read_index)` 历史视图；version log |
| image snapshot (M3) | `create/release_image_snapshot` O(1) pin；`read(..., raft=0, image_read_seq)` 按 `image_snap_seq` 过滤 version log |
| snapshot reader (M5) | `pg_snapshot_reader`：pin + 按 extent 流式读 snapshot 视图（install snapshot 发送端骨架） |
| snapshot ingest (M5) | `snapshot_ingest`：idle→ingesting→sealed→catchup→idle；staging epoch + CRC chunk + abort |
| osd_store apply (M4) | `apply` / `read` / `remove` 对 serving PG；`osd_stm::attach_aos` 可切 blob/aos/shadow |

已删除错误方向：`file_segment`（POSIX pwrite）。

## 构建与测试

```bash
# 单元（mem backend，无 spdk_app）
cmake --build build --target aos_m1_test -j$(nproc)
./build/src/localstore/aos/aos_m1_test

# SPDK 集成（malloc bdev + reactor）
cmake --build build --target aos_spdk_test -j$(nproc)

# 配置 2MB hugepage（需 root；/dev/hugepages 一般已挂载 hugetlbfs）
echo 2048 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
# -s 限制 SPDK 使用的内存 MB，避免 iobuf 池分配失败
./build/src/localstore/aos/aos_spdk_test -c ./build/src/localstore/aos/aos_malloc.json -s 512
```

## 下一步

- 进程内创建/恢复 AOS 设备并在 `partition_manager` 里 `attach_aos`（M4 落地）
- install snapshot RPC 接 reader/ingest（M5）
- GC 限速、live ratio victim 选择、pin 时间线精细化（M6 完善）

## vibe 约束

```text
- 框架：SPDK reactor + device_backend 异步语义
- 设计：docs/append_only_store_design.md
- 4K 对象对齐；object max 4MiB
- 禁止 blobstore / spdk_bs_create_snapshot
- 禁止 POSIX 生产 IO 路径
```

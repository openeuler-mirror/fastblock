# Append-Only Store (AOS)

设计：**`docs/append_only_store_design.md`（v2：AOS 管一切）**

## 铁律：SPDK 是整体框架

| 必须 | 禁止 |
|------|------|
| `spdk_bdev_*` 异步 IO | POSIX `pwrite` 当生产写路径 |
| completion callback | reactor 上阻塞等 IO |
| DMA 缓冲 | 非 DMA 直接下盘 |
| write pointer 尾部推进 | 随机覆盖写 segment |
| `flush` = durability 屏障 | 写完同步当 durable |

## 平台分层（v2）

```text
osd_store / aos_runtime
  ├── pg_store          # object_engine（已落地）
  ├── log_store         # log_engine（P1，进行中）
  ├── kv_engine         # kv_engine（P2，未做）
  ├── segment_allocator
  ├── superblock/manifest
  └── device_backend → spdk_bdev_backend
```

**禁止**用 4K/4MiB object API 模拟 log/kv。

## 当前进度

| 引擎 | 状态 |
|------|------|
| object | **生产唯一路径**；强制 `aos_disk` |
| log | **P1 已接入**：`log_store` + `disk_log(AOS)`；create/load PG 优先 AOS log |
| kv | 设计已写（meta WAL+checkpoint）；仍 rolling_blob |
| blobstore | 仍服务 **kv**（及无 AOS 时的 fallback log）；阶段 C 删除 |

Object 子系统细节见设计文档 §5；历史模块表见 git 历史 README。

## 启用（knife-1）

```json
{
  "aos_disk": "/var/lib/fastblock/osd-1/aos.img",
  "aos_bdev_type": "aio",
  "aos_segment_size": 67108864
}
```

或 `.../osd-<id>/aos_disk` 文件。

## 构建与测试

```bash
cmake --build build --target aos_m1_test -j$(nproc)
./build/src/localstore/aos/aos_m1_test
```

## vibe 约束

```text
- 设计：docs/append_only_store_design.md v2
- 框架：SPDK reactor + device_backend
- object：4K 对齐、max 4MiB
- 无 blob object 生产路径
- 无 POSIX 生产 IO
```

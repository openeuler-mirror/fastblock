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
  ├── pg_store          # object_engine ✅
  ├── log_store         # log_engine ✅
  ├── kv_engine         # kv_engine ✅
  ├── segment_allocator
  ├── superblock/manifest
  └── device_backend → spdk_bdev_backend
```

**禁止**用 4K/4MiB object API 模拟 log/kv。

## 当前进度

| 引擎 | 状态 |
|------|------|
| object | **生产唯一路径**；强制 `aos_disk` |
| log | **生产路径** `log_store` + `disk_log(AOS)` |
| kv | **生产路径** `kv_engine` + `attach_aos_kv` |
| blobstore | **生产不再 init/load**；代码库仍保留供 demo/test |

Object 子系统细节见设计文档 §5。

## 启用

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
cmake --build build --target aos_m1_test fastblock-osd -j$(nproc)
./build/src/localstore/aos/aos_m1_test
```

## dump / light fsck

库：`aos_dump.h` — `scan_image(image, len)` 解析 superblock A/B、manifest、扫 data 区 record，输出 `dump_report` / `format_report`。  
不依赖 SPDK reactor。扫描策略与 `osd_store::recover` 一致：扫满 data 区上界（不截断在可能滞后的 `data_high_water`）。

CLI：

```bash
cmake --build build --target fastblock-aos-dump -j$(nproc)
./build/src/localstore/aos/fastblock-aos-dump [--align 4096] /path/to/aos.img
```

Exit 0 = `report.ok()`，1 = hard errors，2 = 用法/读盘失败。

## vibe 约束

```text
- 设计：docs/append_only_store_design.md v2
- 框架：SPDK reactor + device_backend
- object：4K 对齐、max 4MiB
- 生产无 blobstore / 无 blob object/log/kv
- 无 POSIX 生产 IO
```

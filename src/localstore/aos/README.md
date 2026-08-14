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
| blobstore | **生产不再 init/load**；实现归档 `legacy_blob/`（不进生产链接） |
| P6 log=STM | **默认开启**（开发阶段，不兼容旧二进制）。用户写只落一条 data/tombstone，apply 只 publish。`FASTBLOCK_AOS_UNIFIED_LOG=0` 仅供单测关掉。 |

Object 子系统细节见设计文档 §5。

### Image snap / clone / flatten（P5 数据面）

| API | 含义 |
|-----|------|
| `create_image_snapshot` / `clone_image` | pin `image_snap_seq`（clone = re-pin） |
| `release_image_snapshot` | 减 pin；0 后 GC 可回收仅属该 snap 的历史 |
| `flatten_image` | 把 parent view 物化为 `new_image_seq` 下的 data 记录 |
| `read(..., raft, image)` | 读 snapshot 视图 |
| `osd_store::gc_pass` | rewrite 限速 + free 无 live sealed segment（object + log + kv） |

GC live-set 覆盖 object / log / kv（checkpoint 前 WAL 可裁）。  
控制面 mon/CSI 后续对接；`osd_stm::set_image_snap_seq` 供 apply 写入 image 时间线。

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
`scan_image(..., collect_live=true)` 额外收集 object data / rewrite / tombstone（含 payload），供副本对照。  
不依赖 SPDK reactor。扫描策略与 `osd_store::recover` 一致：扫满 data 区上界（不截断在可能滞后的 `data_high_water`）。

CLI：

```bash
cmake --build build --target fastblock-aos-dump -j$(nproc)
./build/src/localstore/aos/fastblock-aos-dump [--align 4096] /path/to/aos.img
```

Exit 0 = `report.ok()`，1 = hard errors，2 = 用法/读盘失败。

数据正确性（每 offset 唯一 payload，不是 block_bench 的 0x55）见
`src/tools/image_verify/`：`image_verify -M write|verify` 走 libblk；
`-M dump-compare` 离线对照三副本 `aos_file`。脚本：`scripts/run-aos-data-verify.sh`。

## vibe 约束

```text
- 设计：docs/append_only_store_design.md v2
- 框架：SPDK reactor + device_backend
- object：4K 对齐、max 4MiB
- 生产无 blobstore / 无 blob object/log/kv
- 无 POSIX 生产 IO
```

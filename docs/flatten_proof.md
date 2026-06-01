# flatten_proof 测试文档

## 概述

`flatten_proof` 是一个集成测试，验证 fastblock 的 **flatten（扁平化）** 操作的正确性。测试基于 SPDK 事件框架运行，通过完整的 "基础镜像 → 快照 → 克隆 → flatten" 流程，校验数据在各阶段的完整性以及 flatten 后的元数据状态。

源码：`src/test/flatten_proof.cc`

## 构建

```bash
mkdir -p build && cd build
cmake ..
cmake --build . --target flatten_proof
```

产物：`build/src/test/flatten_proof`

## 运行前提

- fastblock monitor + OSD 集群已启动
- 已创建目标 pool（默认 `fb`）
- RDMA 设备可用（RXE 软 RDMA 或硬件 RDMA）
- SPDK 环境（hugepages、内核模块等已就绪）
- 测试使用的 CPU core 不能与 OSD 或其他 SPDK 进程冲突

## 使用方法

### 命令行参数

| 参数 | 说明 |
|------|------|
| `-C <config>` | fastblock JSON 配置文件路径（必需） |
| `-m <coremask>` | SPDK reactor CPU 掩码，如 `0x2` 表示使用 core 1 |

### 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `FB_FLATTEN_POOL` | `fb` | pool 名称 |
| `FB_FLATTEN_IMAGE` | `flatten-proof-base` | 基础镜像名称 |
| `FB_FLATTEN_SNAPSHOT` | `snap-flatten-proof` | 快照名称 |
| `FB_FLATTEN_CLONE` | `flatten-proof-clone` | 克隆镜像名称 |

### 运行示例

```bash
# 使用 lab 配置运行
FB_FLATTEN_IMAGE="test-base-$(date +%s)" \
FB_FLATTEN_SNAPSHOT="test-snap-$(date +%s)" \
FB_FLATTEN_CLONE="test-clone-$(date +%s)" \
./build/src/test/flatten_proof -m 0x2 -C .lab-ready/etc/fastblock/fastblock.json
```

建议每次运行时使用唯一的镜像/快照/克隆名称，避免与上一次残留的已 flatten 克隆冲突。

## 测试流程

测试按 phase 顺序执行，每个 phase 异步完成后触发下一个：

```
init → write_base_seg0 → write_base_seg1 → write_base_seg2
  → create_snapshot → protect_snapshot → create_clone
  → warm_clone_lineage → write_clone_seg0
  → read_clone_before_flatten_seg0/seg1/seg2
  → flatten_clone
  → read_clone_after_flatten_seg0/seg1/seg2
  → verify_metadata → done
```

### Phase 详细说明

#### 1. 准备阶段

| Phase | 操作 | 说明 |
|-------|------|------|
| `init` | 初始化 | 读取配置，连接 monitor，启动 blk_client |
| 镜像准备 | `ensure_image_ready` | 查找或创建基础镜像，确保状态为 `ready` |
| 刷新缓存 | `refresh_cached_image_metadata` | 将镜像元数据加载到客户端缓存 |

#### 2. 基础镜像写入

| Phase | Offset | 数据模式 | 大小 |
|-------|--------|----------|------|
| `write_base_seg0` | 0 | `@@BASE_0\n` | 4096 B |
| `write_base_seg1` | 4 MiB | `@@BASE_1\n` | 4096 B |
| `write_base_seg2` | 8 MiB | `@@BASE_2\n` | 4096 B |

三个 segment 分别对应前 3 个 object（默认 object_size=4MiB），覆盖 0 / 4MiB / 8MiB 偏移。

#### 3. 快照与保护

| Phase | 操作 | 说明 |
|-------|------|------|
| `create_snapshot` | 创建或查找快照 | 快照名已存在时复用并获取 metadata |
| `protect_snapshot` | 保护快照 | 防止快照被意外删除 |
| 缓存更新 | `advance_cached_image_snap_seq` | 更新客户端缓存的 snap_seq |

#### 4. 克隆创建与写入

| Phase | 操作 | 说明 |
|-------|------|------|
| `create_clone` | **创建或查找克隆** | 克隆不存在时调用 `create_clone_from_snapshot`；已存在则复用 |
| `warm_clone_lineage` | **lineage 预热** | 打开克隆镜像，触发 lineage 元数据加载 |
| `write_clone_seg0` | 写入克隆 SEG0 | 数据模式 `@@CLONE0\n`，覆盖父卷 seg0 |

**E_BUSY 重试机制**：克隆创建后 lineage 可能尚未就绪，写操作返回 `E_BUSY` 时会自动重试（100μs 间隔，最多 30 次）。

#### 5. Flatten 前读取验证

| Phase | Offset | 期望数据 | 期望来源 |
|-------|--------|----------|----------|
| `read_clone_before_flatten_seg0` | 0 | `@@CLONE0\n` | **克隆自身**（已被写入覆盖） |
| `read_clone_before_flatten_seg1` | 4 MiB | `@@BASE_1\n` | **父卷**（通过快照 lineage 继承） |
| `read_clone_before_flatten_seg2` | 8 MiB | `@@BASE_2\n` | **父卷**（通过快照 lineage 继承） |

关键语义：flatten 前，克隆只有 seg0 是自己的数据，seg1/seg2 通过快照引用父卷。

#### 6. Flatten 执行

| Phase | 操作 | 说明 |
|-------|------|------|
| `flatten_clone` | 调用 `flatten_image` | 将父卷数据拷贝到克隆中，断开快照依赖 |

flatten 通过异步回调返回结果，state=0 表示成功。

#### 7. Flatten 后读取验证

| Phase | Offset | 期望数据 | 期望来源 |
|-------|--------|----------|----------|
| `read_clone_after_flatten_seg0` | 0 | `@@CLONE0\n` | **克隆自身数据不变** |
| `read_clone_after_flatten_seg1` | 4 MiB | `@@BASE_1\n` | **已物化**（从父卷拷贝到克隆） |
| `read_clone_after_flatten_seg2` | 8 MiB | `@@BASE_2\n` | **已物化**（从父卷拷贝到克隆） |

关键语义：flatten 后数据内容不变，但 seg1/seg2 不再依赖快照，数据已被"物化"到克隆自身。

#### 8. 元数据验证

| 检查项 | 期望值 | 说明 |
|--------|--------|------|
| `parent_snapshot_id` | 空 | 克隆不再挂载任何快照 |
| `depth` | 0 | 克隆已是独立的根镜像 |

## 测试数据布局

```
   offset          base image         clone (before flatten)    clone (after flatten)
  ─────────────────────────────────────────────────────────────────────────────────
   0        (seg0)  @@BASE_0\n         @@CLONE0\n (COW)         @@CLONE0\n (own)
   4 MiB    (seg1)  @@BASE_1\n         @@BASE_1\n (parent)      @@BASE_1\n (materialized)
   8 MiB    (seg2)  @@BASE_2\n         @@BASE_2\n (parent)      @@BASE_2\n (materialized)
```

## 退出码

| 退出码 | 含义 |
|--------|------|
| 0 | 测试通过 |
| 5 (EIO) | 任何阶段失败 |
| 其他 | SPDK 框架错误 |

## 注意事项

1. **CPU core 冲突**：默认使用 core 0，如 OSD 占用了 core 0，需通过 `-m 0x2` 指定其他 core
2. **残留状态**：如果上一次运行成功，克隆已被 flatten（`parent_snapshot_id` 为空），再次运行会因无法重复 flatten 而失败。建议每次使用唯一名称
3. **E_BUSY 重试**：克隆创建后写操作可能因 lineage 未就绪返回 `E_BUSY`，测试内置了最多 30 次、每次 100μs 的重试逻辑，属于正常行为
4. **镜像复用**：测试对基础镜像、快照、克隆均支持幂等复用，如果已存在则跳过创建直接使用已有数据

# OSD

生产存储路径是 **AOS（AppendOnlyStore）**，不是 SPDK blobstore。  
object / raft log / meta KV 共用一块 `aos_disk` 设备（或镜像文件）。

## 主要参数

| 参数 | 说明 |
|------|------|
| `-C` | OSD 配置文件（json） |
| `--id` | OSD id |
| `--mkfs` | 初始化 AOS 布局（superblock + empty manifest） |
| `--uuid` | mkfs 时注册用的 uuid |

配置文件里还需包含 monitor / RPC 等；RPC 说明见 `src/msg/README.md`。  
样例：`fastblock.json`。

### AOS 盘配置（必填）

任选其一：

1. json 键：`"aos_disk": "/var/lib/fastblock/osd-1/aos.img"`  
2. 文件：`<conf_dir>/osd-<id>/aos_disk`（内容为一行设备/镜像路径）

可选：

```json
{
  "aos_disk": "/var/lib/fastblock/osd-1/aos.img",
  "aos_bdev_type": "aio",
  "aos_segment_size": 67108864
}
```

`vstart.sh` 会为每个 OSD 自动创建 `aos.img` 并写入 `aos_disk`。

### 历史 blob 配置（不再用于生产）

旧文档里的 `osds` / 主盘 blobstore 路径**不再**在生产启动时 `blobstore_init/load`。  
demo / 单测仍可能链接 blob 代码。

---

## 使用 AIO 文件盘（开发）

```bash
# 准备镜像（建议 ≥ 2GiB）
mkdir -p /var/lib/fastblock/osd-1
dd if=/dev/zero of=/var/lib/fastblock/osd-1/aos.img bs=1M count=2048

# 配置示例片段
# "aos_disk": "/var/lib/fastblock/osd-1/aos.img"
# "aos_bdev_type": "aio"

# mkfs（AOS format，非 blobstore）
./build/src/osd/fastblock-osd -s 1024 -m 0x1 -C fastblock.json --mkfs --id 1 --uuid 1

# 启动
./build/src/osd/fastblock-osd -s 1024 -m 0x1 -C fastblock.json --id 1
```

注册 uuid 到 monitor（按环境使用 `fastblock-client` / `fbclient`）：

```bash
fastblock-client -op=fakeapplyid -uuid=`uuidgen`
```

本地一键：`./vstart.sh`（已按 AOS 布局建盘）。

---

## 使用 NVMe（生产）

### 1. SPDK 接管磁盘

```bash
sh setup.sh status
sh setup.sh
```

`setup.sh` 位于 `build/deps_build/spdk-prefix/src/spdk/scripts`。

### 2. 配置

`setup.sh status` 拿到 BDF 后，将 `aos_disk` 设为该盘（或把 `aos_bdev_type` 设为 `nvme` 并写 traddr，与现网配置一致）。

### 3. mkfs + 启动

```bash
./build/src/osd/fastblock-osd -s 1024 -m 0x1 -C fastblock.json --mkfs --id 1 --uuid 1
./build/src/osd/fastblock-osd -s 1024 -m 0x1 -C fastblock.json --id 1
```

---

## 离线 dump / light fsck

```bash
./build/src/localstore/aos/fastblock-aos-dump /var/lib/fastblock/osd-1/aos.img
```

输出 superblock A/B、manifest、data 区 record 统计与错误/警告。  
库 API：`src/localstore/aos/aos_dump.h`。

---

## 设计文档

- `docs/append_only_store_design.md`（v2）
- `src/localstore/aos/README.md`

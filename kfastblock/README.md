# kfastblock

`kfastblock` 是 fastblock 内核态块客户端的第一版骨架。当前这批文件只固定模块边界、控制面入口和后续实现路径，不接入 monitor、也不提交真实数据 I/O。

## 目录

- `include/kfastblock/`
  内核模块内部头文件，按 control/meta/request/transport/volume 分层。
- `src/`
  内核模块骨架实现。
- `tool/`
  用户态 attach/detach 辅助工具，负责向 sysfs 写参数。
- `TASKS.md`
  按模块拆开的后续工作清单。

## 当前状态

- 已有 `register_blkdev + sysfs attach/detach + blk-mq + gendisk` 的基础框架。
- 已有请求拆分层，占位表示未来的 image -> object -> pg 子请求切分。
- 已有 metadata/transport 的占位接口，用来接 monitor 控制面和 OSD 数据面。
- 未实现 monitor bootstrap、cluster map 缓存、leader 查询、真实读写、故障恢复。

## 为什么单独放在顶层

fastblock 当前主线是 `CMake + SPDK + 用户态客户端`。内核客户端的构建模型更接近 `cubs-client`，独立成顶层目录可以避免污染现有用户态构建和运行路径。

## 计划中的 attach 参数

当前 sysfs/admin 工具接受以下键值对：

- `monitor_addr`
- `pool_name`
- `image_name`
- `token`
- `snapshot_name`
- `read_only`
- `debug_size_bytes`
- `debug_object_size`

其中 `debug_*` 仅用于骨架调试。后续接入 monitor 之后，这两个字段应被真正的 `GetImageInfo/GetClusterMap` 结果替换。

## 现阶段如何理解这套骨架

这不是一个“可用的 fastblock 内核客户端”，而是把后面的实现工作拆成了可落地的模块：

1. `control`: map/unmap 和参数解析
2. `meta`: image / osdmap / pgmap / leader cache
3. `request`: 一个块请求拆成多个 object 子请求
4. `transport`: monitor 控制面和 OSD 数据面
5. `volume`: gendisk、blk-mq、生命周期管理

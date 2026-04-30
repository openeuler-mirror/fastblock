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
- 已有请求拆分层，包含 fastblock 的 object 命名规则和 `object -> pg` hash 路由规则。
- 已有 monitor 地址解析和 endpoint 连接骨架，支持逗号分隔的多个 `monitor_addr`。
- 已能通过 raw TCP 向 monitor 发起 `GET_IMAGE_INFO` 和 `GET_CLUSTER_MAP`，并在 bootstrap 阶段填充 image 基本信息、完整的 OSD/shard 表和 PG route 表。
- 已有 metadata lookup helper，可直接做 `osd_id -> endpoint`、`pool_id + pg_id -> route` 和 `pg + osd_id -> shard endpoint` 查询。
- 已能通过 osd raw TCP 向 PG 副本发起 `GET_LEADER`，并把 leader 地址/端口缓存到对应的 PG route 上。
- 已有第一版 object I/O 提交路径，可把块请求按 object 切分后并发走 `GET_LEADER + READ_OBJECT/WRITE_OBJECT/DELETE_OBJECT`。
- 已有每卷一个后台 refresh work，周期向 monitor raw 重新拉取 metadata 并更新本地视图。
- 已有最小 OSD/monitor socket cache、backoff、timeout 和请求级/对象级并发提交。
- 已通过 sysfs 导出 `osd_count`、`route_count`、`osdmap_epoch`、`pgmap_epoch`、`leader_epoch`，便于确认 monitor raw 拉下来的缓存是否生效。
- 已有 metadata/transport 的占位接口，用来接 monitor 控制面和 OSD 数据面。
- 未实现更细粒度的连接池调度、完整重试/故障恢复。

## 为什么单独放在顶层

fastblock 当前主线是 `CMake + SPDK + 用户态客户端`。内核客户端的构建模型更接近 `cubs-client`，独立成顶层目录可以避免污染现有用户态构建和运行路径。

## 计划中的 attach 参数

当前 sysfs/admin 工具接受以下键值对：

- `monitor_addr`
- `pool_name`
- `image_name`
- `read_only`
- `debug_size_bytes`
- `debug_object_size`
- `debug_pool_id`
- `debug_pg_count`

其中 `monitor_addr` 支持 `ip:port,ip:port,...`，未指定端口时默认使用 raw monitor 端口 `3334`。`debug_*` 仅用于骨架调试。后续接入 monitor 之后，这些字段应被真正的 `GetImageInfo/GetClusterMap` 结果替换。

## 现阶段如何理解这套骨架

这不是一个“可用的 fastblock 内核客户端”，而是把后面的实现工作拆成了可落地的模块：

1. `control`: map/unmap 和参数解析
2. `meta`: image / osdmap / pgmap / leader cache
3. `request`: 一个块请求拆成多个 object 子请求
4. `transport`: monitor 控制面和 OSD 数据面
5. `volume`: gendisk、blk-mq、生命周期管理

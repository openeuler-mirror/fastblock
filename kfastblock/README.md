# kfastblock

`kfastblock` 是 fastblock 的内核态块客户端，当前阶段定位为“研发验证版 / 集成联调前夜”。它已经接入 monitor/OSD raw TCP，具备 attach、元数据拉取、对象级读写和最小块设备 smoke 闭环，但还不适合直接进入生产级稳定性和性能验收。

## 目录

- `include/kfastblock/`
  内核模块内部头文件，按 control/meta/request/transport/volume 分层。
- `src/`
  内核模块实现。
- `tool/`
  用户态 attach/detach/卷级运维工具，以及 raw TCP 协议调试工具。
- `TASKS.md`
  按模块拆开的后续工作清单。

## 当前状态

- 已实现 `register_blkdev + sysfs attach/detach + blk-mq + gendisk`，attach 后可得到 `/dev/kfb*`。
- 已实现请求拆分层，包含 fastblock 的 object 命名规则和 `object -> pg` hash 路由规则。
- 已支持 monitor 地址解析和 endpoint 连接，支持逗号分隔的多个 `monitor_addr`。
- 已能通过 raw TCP 向 monitor 发起 `GET_IMAGE_INFO` 和 `GET_CLUSTER_MAP`，并在 bootstrap 阶段填充 image 基本信息、完整的 OSD/shard 表和 PG route 表。
- 已有 metadata lookup helper，可直接做 `osd_id -> endpoint`、`pool_id + pg_id -> route` 和 `pg + osd_id -> shard endpoint` 查询。
- 已能通过 osd raw TCP 向 PG 副本发起 `GET_LEADER`，并把 leader 地址/端口缓存到对应的 PG route 上。
- 已有对象 I/O 提交路径，可把块请求按 object 切分后并发走 `GET_LEADER + READ_OBJECT/WRITE_OBJECT/DELETE_OBJECT`。
- 已有每卷一个后台 refresh work，周期向 monitor raw 重新拉取 metadata 并更新本地视图。
- 已有请求级 PG 快照、leader hint、最小 OSD/monitor socket cache、backoff、timeout 和请求级/对象级并发提交。
- 已通过 sysfs 导出 `osd_count`、`route_count`、`osdmap_epoch`、`pgmap_epoch`、`leader_epoch`、`open_count`、`queue_paused` 等状态。
- 已通过 debugfs 导出 `summary/osds/routes/sockets/stats/events` 观察面。
- 已有 `kfastblock-rawctl` 用户态调试工具，可直接对 monitor/osd raw TCP 发起 `GET_IMAGE_INFO/GET_CLUSTER_MAP/GET_LEADER/READ/WRITE/DELETE_OBJECT`。
- 已有 `kfastblock-admin`，支持 `attach/detach/list/show` 以及 `force-refresh/reset-backoff/drop-transport/reset-leaders/pause-queue/resume-queue` 等卷级运维动作。
- 已通过开发环境 smoke：attach 后可写入 `/dev/kfb0`、读回并完成 `cmp` 校验。

## 为什么单独放在顶层

fastblock 当前主线是 `CMake + SPDK + 用户态客户端`。内核客户端的构建模型更接近 `cubs-client`，独立成顶层目录可以避免污染现有用户态构建和运行路径。

## 构建与验证

- 构建模块与工具：`make -C kfastblock clean all`
- 最小闭环 smoke：`./scripts/run-kfastblock-dev-smoke.sh`
- 并发入口：`./scripts/run-kfastblock-concurrency.sh`
- sanitizer 入口：`./scripts/run-kfastblock-sanitizer.sh`

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

其中 `monitor_addr` 支持 `ip:port,ip:port,...`，未指定端口时默认使用 raw monitor 端口 `3334`。`debug_*` 仅用于 monitor metadata 不可用时的开发调试兜底，正常 attach 路径应以 `GET_IMAGE_INFO/GET_CLUSTER_MAP` 返回结果为准。

## 当前实现边界

当前版本可以支撑：

- 内核态协议联调
- monitor/OSD 的最小系统级联调
- 块设备闭环验证
- 可观测性和运维入口打磨

当前版本仍不建议直接进入：

- 长稳 soak 测试
- 高并发性能压测
- 生产预验证

已知限制包括：

- 对象 I/O 仍采用整对象 bounce buffer，当前已切到 `kvmalloc/kvzalloc` 止血，但还未做到 `bio_vec/iov_iter` 直拼。
- sanitizer 运行态回归依赖专用 KASAN/KCSAN 内核或外部 runner，默认开发机可能只走 build-only 路径。
- 连接池调度、长期重试策略和更细粒度性能优化仍在后续阶段。

## 调试工具

- `make kfastblock-rawctl`
- `make kfastblock-admin`
- `tool/kfastblock-admin list`
- `tool/kfastblock-admin show --pool-name <pool> --image-name <image>`
- `tool/kfastblock-rawctl get-image-info --addr 127.0.0.1:3334 --pool-name <pool> --image-name <image>`
- `tool/kfastblock-rawctl get-cluster-map --addr 127.0.0.1:3334 --pool-id <id>`
- `tool/kfastblock-rawctl get-leader --addr <osd_raw_addr:port> --pool-id <id> --pg-id <id>`
- `tool/kfastblock-rawctl read-object --addr <osd_raw_addr:port> --pool-id <id> --pg-id <id> --object <name> --offset 0 --length 4096`

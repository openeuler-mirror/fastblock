# kfastblock Tasks

## P0: 控制面和元数据边界

- 把 `create image` 和 `map image` 完全解耦。
- 明确 `kfastblock` 只对接 `monitor`，不直接对接 etcd。
- 给 monitor 增补内核客户端真正需要的聚合接口：
  `GetClusterMap`
  `GetImageInfo`
  如有必要，增加 `GetLeaderHints` 或等价能力。
- 固定 image 元数据语义：
  `pool_id`
  `pool_name`
  `image_name`
  `size`
  `object_size`
  `block_size`
- 修掉当前用户态路径里 `object_size` 元数据和固定 `4 MiB` 切分不一致的问题。

## P1: 内核元数据缓存

- 在 `meta.c` 里实现 attach 时的 bootstrap：
  拉 image info
  拉 osdmap/pgmap
  初始化本地 cluster view
- 增加后台刷新线程或 delayed work：
  周期刷新 osdmap/pgmap
  处理 monitor 切主
  更新 leader cache
- 定义“视图代际”：
  image epoch
  osdmap epoch
  pgmap epoch
  leader epoch
- 明确 stale view 下的行为：
  冻结队列
  快速失败
  重试

## P1: 请求拆分和路由

- 按 `image + object_size` 完成块请求到 object 请求的切分。
- 固化 object 命名规则，保证用户态和内核态一致。
- 固化 object -> pg 的 hash 规则。
- 增加父请求/子请求聚合：
  读：多 object 回填到原始 bio
  写：全部 object 成功后完成父请求
- 定义部分失败、重试、超时和 cancel 的状态机。

## P1: 数据面传输

- 明确第一版数据面实现：
  推荐先做 monitor TCP + OSD TCP
  暂不直接做内核 RDMA
- 为 OSD 增加内核友好的数据协议，避免把 SPDK/protobuf/userspace RDMA stub 直接搬进内核。
- 做连接池、发送队列、接收匹配、超时、backoff 和重连。
- 加入 request id / seq / correlation id，确保多请求并发下可匹配响应。

## P2: 卷生命周期

- attach:
  参数解析
  bootstrap 元数据
  创建设备
- detach:
  停止新 I/O
  等待 inflight 或超时收敛
  删除设备
- resize:
  刷新 image info
  更新 capacity

## P2: 故障和一致性

- leader 变化时重查并重投请求。
- pg remap 时识别旧路由失效。
- monitor 不可达时限制 I/O 行为。
- osd down / starting / pg initializing 时区分可重试和不可重试错误。
- 定义 queue freeze / unfreeze 触发条件。

## P2: 测试

- 请求拆分单元测试。
- 参数解析单元测试。
- metadata cache 刷新单元测试。
- fio 冒烟测试。
- monitor failover、osd down、pg remap 集成测试。

## 当前代码到模块的映射

- `control.c`
  sysfs attach/detach
  参数解析
- `meta.c`
  未来对应 monitor client 和 cluster view 缓存
- `request.c`
  对应当前用户态 `libfblock` 的 object 切分逻辑
- `transport.c`
  monitor/OSD 传输层占位
- `volume.c`
  gendisk 和 blk-mq 主体

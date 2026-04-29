# kfastblock raw TCP 设计与实现路线

## 1. 背景

当前 fastblock 的客户端链路分成两类：

- monitor 控制面：TCP + protobuf
- osd 数据面：protobuf + RDMA

这条链路对用户态客户端是合理的，但对内核态客户端 `kfastblock` 有两个明显问题：

1. 内核里直接接 protobuf 成本高，维护难度大。
2. 内核里直接接 RDMA 成本更高，且会把 `kfastblock` 的 bring-up 难度推得过高。

因此，本设计选择为 `kfastblock` 新增一套 **raw TCP 协议**：

- monitor 新增 raw TCP 控制面接口
- osd 新增 raw TCP 数据面接口
- `kfastblock` 只实现 raw TCP client
- 保留现有 protobuf/TCP 和 protobuf/RDMA 路径，不影响现有用户态客户端和性能路径

## 2. 目标

### 2.1 目标

- 为 `kfastblock` 提供不依赖 protobuf 和 RDMA 的控制面/数据面协议。
- 保持现有 monitor/osd 业务逻辑可复用，新增的主要是协议接入层。
- 让 `kfastblock` 具备以下能力：
  - attach / detach image
  - 周期获取 image info / osdmap / pgmap
  - 请求切分为 object
  - 根据 pg 路由到 leader
  - 通过 TCP 完成 object read / write / delete
- 不引入常驻用户态代理进程做 monitor 数据同步或数据转发。

### 2.2 非目标

- 不替换现有 protobuf/RDMA 路径。
- 不在第一期支持 snapshot。
- 不在第一期支持卷共享、多 attach 协调、QoS 完整实现。
- 不在第一期追求和 RDMA 路径相同的性能。

## 3. 关键设计选择

### 3.1 为什么不用内核 protobuf

原因很简单：

- `GetClusterMapResponse` 这类消息结构复杂，包含 map / repeated / 嵌套 message。
- 内核里没有合适的 protobuf runtime 可直接复用。
- 手写 protobuf 编解码长期维护成本高。

因此内核不直接消费 protobuf，而是消费更简单的 raw 协议。

### 3.2 为什么不用用户态常驻代理

不采用 `kfastblockd` 这类常驻用户态代理的主要原因：

- 架构不够简洁。
- 生命周期、升级、异常恢复更复杂。
- 会把控制面状态同步问题从协议层转成进程协同问题。

本方案倾向于把复杂度放在服务端新增 raw 协议，而不是新增长期驻留的用户态桥接层。

### 3.3 为什么 raw 协议分 monitor 和 osd 两套

因为 monitor 和 osd 的职责完全不同：

- monitor：元数据和集群视图
- osd：数据 I/O 和 leader 查询

这两类消息的数据形态、频率、错误语义都不同，拆开设计更清晰。

## 4. 总体架构

### 4.1 逻辑结构

- `kfastblock`
  - control
  - meta cache
  - request split / aggregate
  - monitor raw client
  - osd raw client
  - block device
- `monitor`
  - 现有 protobuf/TCP 服务继续保留
  - 新增 raw/TCP 服务
  - 复用现有 pool/image/osdmap/pgmap 业务逻辑
- `osd`
  - 现有 protobuf/RDMA 服务继续保留
  - 新增 raw/TCP 服务
  - 复用现有 `process_write/process_read/process_delete/process_get_leader` 语义

### 4.2 启动流程

1. 用户执行 `kfastblock-admin attach`。
2. 内核模块解析 `monitor_addr/pool_name/image_name`。
3. `kfastblock` 通过 monitor raw TCP 获取：
   - `GetImageInfo`
   - `GetClusterMap`
4. 内核建立 image view / osdmap / pgmap 缓存。
5. 创建 gendisk。
6. 后台周期轮询 monitor，刷新 metadata。
7. 读写请求到来后：
   - 按 `object_size` 切分
   - 按 object hash 定位 pg
   - 根据 pg 找 leader
   - 通过 osd raw TCP 发 object I/O
   - 聚合完成父请求

## 5. raw 协议总则

### 5.1 传输层

- 使用 TCP。
- 每个连接上允许多请求顺序发送，支持多连接并发。
- 每个请求和响应都带统一头部。
- 头部与 body 都使用 little-endian。
- 不做通用 TLV，采用固定头 + 按 opcode 定义 body。

### 5.2 通用头

建议统一定义：

```c
struct fbraw_hdr {
    __le32 magic;
    __u8 version_major;
    __u8 version_minor;
    __u8 service;
    __u8 opcode;
    __le32 flags;
    __le64 seq;
    __le32 status;
    __le32 body_len;
};
```

字段含义：

- `magic`
  固定魔数，区分 raw 协议和其他流量
- `version_major/version_minor`
  协议版本
- `service`
  `1 = monitor`, `2 = osd`
- `opcode`
  请求类型
- `flags`
  预留，例如 `response`、`partial`、`snapshot` 等
- `seq`
  连接内请求序号，用于匹配响应
- `status`
  响应错误码，请求时填 0
- `body_len`
  body 长度

### 5.3 错误码

建议维护统一的一组 raw 协议错误码，并和现有 `err_num` 做映射：

- `0`: success
- `1`: invalid_request
- `2`: not_found
- `3`: stale_epoch
- `4`: retry_later
- `5`: not_leader
- `6`: pg_initializing
- `7`: osd_down
- `8`: internal_error

监控面和数据面内部都可以再映射到现有错误体系。

## 6. monitor raw 协议设计

### 6.1 监听方式

建议 monitor 新增 **独立 raw TCP 端口**，不与现有 protobuf/TCP 端口复用。

原因：

- 避免靠 magic 在同一个 listener 上分流
- 错误定位更简单
- 回滚更容易

配置项建议新增：

- `raw_monitor_port`

默认可以设为 `3334`。

### 6.2 monitor raw 操作集合

第一期只做两个：

- `GET_IMAGE_INFO`
- `GET_CLUSTER_MAP`

后续如有需要，可增加：

- `WATCH_CLUSTER_MAP`
- `GET_POOL_INFO`
- `GET_LEADER_HINTS`

### 6.3 GET_IMAGE_INFO

#### 请求

```c
struct fbraw_get_image_info_req {
    __le64 image_epoch;
    __le16 pool_name_len;
    __le16 image_name_len;
    char payload[]; // pool_name + image_name
};
```

说明：

- `image_epoch`
  客户端当前看到的 image epoch
  第一版可先传 0

#### 响应

```c
struct fbraw_get_image_info_rsp {
    __le64 image_epoch;
    __le32 pool_id;
    __le32 block_size;
    __le32 object_size;
    __le32 flags; // bit0 = read_only
    __le64 size_bytes;
};
```

说明：

- 第一版可以先把 `block_size` 固定返回 4096
- 后面可扩展 image feature flags

### 6.4 GET_CLUSTER_MAP

#### 请求

```c
struct fbraw_get_cluster_map_req {
    __le64 osdmap_epoch;
    __le32 pool_id;
    __le32 reserved;
    __le64 pgmap_epoch;
};
```

#### 响应

响应分为三个部分：

1. 总体头
2. osd 数组
3. pg 数组

```c
struct fbraw_get_cluster_map_rsp {
    __le64 osdmap_epoch;
    __le64 pgmap_epoch;
    __le32 osd_count;
    __le32 pg_count;
};
```

##### osd entry

```c
struct fbraw_osd_entry_hdr {
    __le32 osd_id;
    __le32 flags; // bit0=in, bit1=up
    __le16 address_len;
    __le16 shard_count;
    char payload[]; // address + shard entries
};

struct fbraw_osd_shard_entry {
    __le32 shard_id;
    __le16 raw_tcp_port;
    __le16 reserved;
};
```

说明：

- `address` 先支持 IPv4 字符串
- `shard_id` 对齐现有 shard/core 逻辑
- raw TCP 数据面端口通过 shard entry 下发

##### pg entry

```c
struct fbraw_pg_entry {
    __le32 pool_id;
    __le32 pg_id;
    __le64 version;
    __le32 state;
    __le32 primary_shard_id;
    __le32 replica_count;
    __le32 osd_ids[0];
};
```

说明：

- 请求中的 `pool_id` 用于限定只返回目标 pool 的 pgmap，避免每次轮询都下发所有 pool 的 PG 路由。
- 当请求里的 `osdmap_epoch` 与目标 pool 的 `pgmap_epoch` 都和服务端一致时，服务端可直接返回 `STALE_EPOCH`，不再回传全量 body。
- `primary_shard_id` 对应当前 `PGInfo.coreindex`
- 第一版不把 leader 塞进 cluster map
- leader 仍由 osd 数据面上的 `GET_LEADER` 获取

### 6.5 monitor 轮询策略

`kfastblock` 需要持续获取 monitor 视图。

建议：

- `GET_CLUSTER_MAP`: 默认每 `3s` 轮询一次
- `GET_IMAGE_INFO`: 默认每 `30s` 轮询一次，或者在 attach 后立即拉一次，再在 resize / 错误恢复路径下重拉

如果你坚持和当前用户态行为更接近，也可以把 `GET_IMAGE_INFO` 轮询周期压到和 `GET_CLUSTER_MAP` 一样，但第一期没有必要。

## 7. osd raw 协议设计

### 7.1 监听方式

建议 osd 也新增独立 raw TCP 数据面端口。

这里有两个方案：

#### 方案 A：每 shard 一个 raw TCP 端口

优点：

- 最贴近当前 OSD shard 架构
- 与现有 `sharded_ports` 模型一致
- 请求到端口后天然落到对应 shard

缺点：

- monitor 下发的 osd entry 稍复杂

#### 方案 B：每 osd 一个 raw TCP 端口

优点：

- 客户端更简单

缺点：

- osd 内部还要再做一次分发
- 与现有 shard 化实现风格不一致

建议第一期采用 **方案 A：每 shard 一个 raw TCP 端口**。

### 7.2 osd raw 操作集合

第一期做：

- `GET_LEADER`
- `READ_OBJECT`
- `WRITE_OBJECT`
- `DELETE_OBJECT`

### 7.3 GET_LEADER

#### 请求

```c
struct fbraw_get_leader_req {
    __le32 pool_id;
    __le32 pg_id;
};
```

#### 响应

```c
struct fbraw_get_leader_rsp {
    __le32 leader_id;
    __le16 leader_port;
    __le16 address_len;
    char address[];
};
```

说明：

- 这个接口本质上对应现有 `process_get_leader`
- 仍由当前业务逻辑计算 leader

### 7.4 READ_OBJECT

#### 请求

```c
struct fbraw_read_object_req {
    __le32 pool_id;
    __le32 pg_id;
    __le64 offset;
    __le32 length;
    __le16 object_name_len;
    __le16 reserved;
    char object_name[];
};
```

#### 响应

```c
struct fbraw_read_object_rsp {
    __le32 data_len;
    __le32 reserved;
    char data[];
};
```

### 7.5 WRITE_OBJECT

#### 请求

```c
struct fbraw_write_object_req {
    __le32 pool_id;
    __le32 pg_id;
    __le64 offset;
    __le32 data_len;
    __le16 object_name_len;
    __le16 reserved;
    char payload[]; // object_name + data
};
```

#### 响应

无额外 body，只看头部 `status`。

### 7.6 DELETE_OBJECT

#### 请求

```c
struct fbraw_delete_object_req {
    __le32 pool_id;
    __le32 pg_id;
    __le16 object_name_len;
    __le16 reserved;
    char object_name[];
};
```

#### 响应

无额外 body，只看头部 `status`。

## 8. kfastblock 内核侧实现路线

### 8.1 目录建议

当前 `transport.c` 后续建议拆分为：

- `transport_common.c`
- `transport_monitor.c`
- `transport_osd.c`

同时可新增：

- `route.c`
  负责 pg -> leader -> endpoint
- `cache.c`
  负责 image/osdmap/pgmap 缓存

### 8.2 monitor client

内核 monitor client 需要做：

- 维护 monitor endpoint 列表
- 轮询 `GET_CLUSTER_MAP`
- 按需或周期拉 `GET_IMAGE_INFO`
- 维护 epoch
- 维护 osd entry / pg entry 缓存
- 在 monitor 故障时重连其他 monitor

### 8.3 osd client

内核 osd client 需要做：

- 连接缓存：`osd_id + shard_id -> socket`
- leader 查询缓存：`pool_id + pg_id -> leader`
- 请求序号管理
- 同步请求 / 异步完成
- 读写请求的聚合和重试

### 8.4 请求处理

块层请求处理流程：

1. bio/request 进入 `queue_rq`
2. 切分为 object extents
3. 每个 object 计算 pg
4. 查询 pg leader
5. 发送 object I/O
6. 聚合完成父请求

### 8.5 刷新和恢复

- monitor 周期刷新 cluster map
- cluster map epoch 更新后，失效旧 leader cache
- leader 查询失败或收到 `not_leader` 时，重查 leader
- monitor 长时间不可达时，冻结或快速失败队列

## 9. monitor 侧改动范围

主要新增 raw front-end，不重写业务逻辑：

- `monitor/monitor.go`
  - 增 raw listener
  - 增 raw conn handler
  - 增 raw request dispatch
- 新增 `monitor/rawproto/*.go`
  - 头部定义
  - 编解码
  - 请求体解析
  - 响应体拼装

复用的业务函数：

- `osd.ProcessGetPgMapMessage`
- `osd.ProcessGetOsdMapMessage`
- `osd.ProcessGetImageMessage`

## 10. osd 侧改动范围

主要新增 raw front-end，不重写业务语义：

- `src/osd/`
  - raw TCP acceptor
  - raw request parser
  - raw response encoder

复用的业务语义：

- `process_get_leader`
- `process_write`
- `process_read`
- `process_delete`

如果复用不方便，也至少保证 raw path 与当前 RDMA path 在语义层对齐。

## 11. 分阶段实现计划

### Phase 0: 协议冻结

- 冻结 monitor raw 协议头和 `GET_IMAGE_INFO/GET_CLUSTER_MAP`
- 冻结 osd raw 协议头和 `GET_LEADER/READ/WRITE/DELETE`
- 写协议文档和测试样例

### Phase 1: monitor raw server

- monitor 新增 raw listener
- 完成 `GET_IMAGE_INFO`
- 完成 `GET_CLUSTER_MAP`
- 写一个用户态调试客户端验证协议

### Phase 2: kfastblock monitor client

- 内核 monitor socket 管理
- image/osdmap/pgmap 缓存
- 周期刷新
- sysfs/debugfs 导出 metadata

### Phase 3: osd raw server

- osd 新增 raw TCP acceptor
- 完成 `GET_LEADER`
- 完成 `READ_OBJECT`
- 完成 `WRITE_OBJECT`
- 完成 `DELETE_OBJECT`

### Phase 4: kfastblock osd client

- 连接池
- leader cache
- object I/O
- 父子请求聚合

### Phase 5: 故障恢复

- monitor failover
- leader 切换
- pg remap
- osd down / restart
- queue freeze / unfreeze

### Phase 6: 测试和性能

- KUnit / 单元测试
- 集成测试
- fio 冒烟
- 对比 raw TCP 和现有 RDMA 路径语义一致性

## 12. 代码量预估

### 12.1 monitor Go 侧

- raw 协议编解码 + listener + handler：`800-1500` 行

### 12.2 osd C++ 侧

- raw 协议编解码 + listener + handler：`1500-2500` 行

### 12.3 kfastblock 内核侧

- monitor client + cache：`1500-2500` 行
- osd client + request path：`2500-3500` 行
- volume/control/debug：`2000-3000` 行

总体上，内核代码达到 `8000-12000` 行是现实的。

## 13. 风险与权衡

### 13.1 优点

- 不需要内核 protobuf
- 不需要内核 RDMA
- 不需要用户态常驻代理进程
- 协议清晰，调试简单

### 13.2 缺点

- monitor 和 osd 都要维护双协议
- raw TCP 性能不如现有 RDMA 数据面
- 需要保证 raw path 和现有 path 的业务语义一致

### 13.3 当前建议

这是目前最均衡的方案：

- 架构上比用户态代理更干净
- 实现上比内核 protobuf + 内核 RDMA 简单
- 可以让 `kfastblock` 在合理成本下真正跑起来

## 14. 评审结论建议

建议评审时重点确认以下四点：

1. monitor/osd 是否接受新增 raw TCP 接口并长期维护
2. osd raw TCP 是否采用每 shard 一个端口
3. `GET_CLUSTER_MAP` 是否需要额外携带 leader hint
4. `GET_IMAGE_INFO` 的轮询周期和 epoch 语义如何定义

如果以上四点达成一致，就可以开始 Phase 0 和 Phase 1。

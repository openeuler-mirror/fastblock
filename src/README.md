代码结构与主要功能
============

| 目录 |  主要功能 |
| -------- | -------- |
| client   |  客户端 |
| mon    |   对应ceph的monitor模块，负责集群的管理 |
| base |   基础模块，主要包含多核支持 |
| bdev |   实现fastblock bdev，用于对接qemu|
| osd      |   Storage Daemon，对应ceph的osd模块，负责存储数据 |
| msg    |   定义了osd和osd、client通信逻辑 |
| rpc |   实现rdma通信 |
| raft |    raft模块，负责raft协议的实现 |
| localstore  | 本地存储，实现基本的读写快照等功能 |
| proto  |     protobuf文件 |
| utils |     基础工具 |
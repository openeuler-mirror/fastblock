模块介绍
============

| client   |  客户端 |
| -------- | -------- |
| mgr    |   对应ceph的monitor模块，负责集群的管理 |
|sd      |   Storage Daemon，对应ceph的osd模块，负责存储数据 |
| raft |    raft模块，负责raft协议的实现 |
| common  |   公共模块，包含一些公共的数据结构和函数 |
| slimfs  |   轻量化的文件系统，用于存放raftlog |
| localstore  | 本地存储，实现基本的读写快照等功能 |
| proto  |     定义了mgr的rpc接口 |
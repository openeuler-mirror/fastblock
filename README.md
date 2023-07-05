# FastBlock
  一款使用成熟raft协议，专注于全闪场景的分布式块存储

# Why FastBlock？
  <li>使用成熟raft协议，保证数据的一致性和高可用性，减少初始上线的难度</li>
  <li>专注在全闪块细分场景，极大简化了方案复杂度，采用了etcd等成熟开源组件，降低了工程难度</li>
  <li>同等硬件介质下，性能是ceph的3倍以上，性价比是ceph的2倍以上</li>

# FastBlock  Overview

# How to build

# Q && A
```
1. 为什么不用Ceph？
    Ceph是一个出色的通用的分布式存储系统，支持多种介质，包括HDD、SSD、NVM等，支持多种场景，包括对象存储、块存储、文件存储等，但是这也导致了Ceph的复杂性， Ceph的架构复杂，包括OSD、MON、MDS、RGW等多个组件，每个组件都有自己的复杂性，改造起来难度较大，无法满足业务快速迭代的需求，而且Ceph的性能也不是很好，特别是在全闪场景下，性价比不高。
```
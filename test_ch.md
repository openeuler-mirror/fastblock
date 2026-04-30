# fastblock CSI 测试整理（中文）

## 1. 目的

本文档用于记录当前 fastblock CSI 方案已经通过的测试、测试过程中暴露并修复的问题，以及仍然存在的风险与后续建议测试项。

本文档聚焦两类信息：

1. 已经明确通过的测试
2. 在生产前验证过程中暴露并已修复的问题

## 2. 当前测试范围

当前已经覆盖的主要范围：

- CSI 控制面单元测试
- exporter 单元测试
- monitor 可执行构建验证
- 单机 Kubernetes 实验环境回归
- controller 重启后的卷恢复与 Pod 重建
- attach/publish 幂等和冲突验证
- node 侧 NVMe stale connection 自愈
- 两节点 Kubernetes 环境搭建与初步多节点验证

## 3. 当前测试环境

当前验证包含两类环境：

### 3.1 单机生产前验证环境

- 节点：`kerneldev`
- Kubernetes：`k3s v1.31.5+k3s1`
- CSI 驱动：`csi.fastblock.io`
- StorageClass：`fastblock-rdma`
- 卷模式：`Block`
- 后端 host 服务：
  - `fastblock-mon`
  - `fastblock-osd`
  - `fastblock-nvmf-tgt`
  - `fastblock-exporter`

当前验证时观测到的稳定状态包括：

- `fastblock-csi-controller` 为 `Running`
- `fastblock-csi-node` 为 `Running`
- `fastblock-pvc` 为 `Bound`
- `fastblock-block-pod` 为 `Running`
- monitor 监听 `10.211.55.27:3333`
- exporter 监听 `:9500`

### 3.2 多节点验证环境

- control-plane / backend 节点：`kerneldev` `10.211.55.27`
- worker 节点：`fastblockdev` `10.211.55.29`
- Kubernetes：`k3s v1.31.5+k3s1`
- CSI controller Pod 运行在 `kerneldev`
- CSI node DaemonSet 在两台节点都成功运行
- fastblock host backend 仍只部署在 `10.211.55.27`
- 多节点测试阶段优先使用 `fastblock-tcp` StorageClass，降低 RDMA 变量干扰

## 4. 已通过测试

### 4.1 Go 单元测试 / 组件级验证

已通过：

- `cd csi && go test ./...`
- `cd exporter && go test ./...`
- `cd monitor && go build -o /tmp/fastblock-mon monitor.go`

这些验证覆盖了：

- controller 元数据、lease、reconcile 逻辑
- monitor metadata / lease client
- exporter 导出查询、删除、ACL 幂等行为
- node backend preflight 与 stale session 恢复

### 4.2 单机 Kubernetes 基本闭环

已通过：

- 创建 `PersistentVolumeClaim`
- PVC 成功 `Bound`
- `Pod` 成功进入 `Running`
- block 设备 `/dev/fastblock0` 成功映射到 Pod
- 在 Pod 中对块设备执行写入成功

这说明以下主链路在单机环境成立：

- `CreateVolume`
- `ControllerPublishVolume`
- `NodeStageVolume`
- `NodePublishVolume`

### 4.3 Controller 重启后的恢复回归

已通过：

1. 先完成首个测试 Pod 的成功挂载与写入
2. 重启 `fastblock-csi-controller`
3. 删除测试 Pod
4. 仅重建测试 Pod，不重建 PVC
5. 新 Pod 再次成功进入 `Running`
6. 块设备再次映射成功并可写

这说明以下能力已经在单机环境中得到验证：

- monitor-backed CSI metadata 持久化有效
- controller startup reconcile 生效
- lease/attachment/export 元数据在 controller 重建后可恢复
- 当前卷不会在 controller 重启后被错误丢失

### 4.4 幂等与冲突语义

已通过：

- 同卷同节点重复 `ControllerPublishVolume` 成功
- 同卷跨节点 `ControllerPublishVolume` 稳定拒绝
- smoke 中已显式验证重复 publish 和跨节点冲突

### 4.5 Node 侧恢复能力

已通过：

- `nvme-cli`、`hostnqn`、传输模块的基础 preflight
- 对 `nvme connect ... already connected` 场景的恢复
- stale NVMe session 会先断开再重连，而不是直接报错退出

### 4.6 多节点环境准备

已通过：

- 第二节点 `10.211.55.29` 网络连通性验证
- 第二节点 SSH 远程执行验证
- 第二节点加入 K3s 集群
- 第二节点 `fastblock-csi-node` Pod 成功启动
- 第二节点 `hostnqn`、`nvme-cli`、`nvme_fabrics`、`nvme_tcp`、`nvme_rdma` 基础环境准备完成

这说明当前代码和部署方式已经可以进入真正的多节点验证阶段，而不是停留在单机实验。

## 5. 测试中暴露并已修复的问题

以下问题是在本轮生产前验证过程中真实暴露出来，并已在代码中修复的：

### 5.1 exporter 对“导出不存在”的错误归一不正确

问题现象：

- `GetExport` 在 SPDK 返回 `No such device` 时，被当成内部错误处理
- controller publish 无法正确进入“导出不存在 -> 创建导出”路径

修复结果：

- SPDK `-19` 被归一为 `ErrExportNotFound`
- 查询不到导出时，controller/exporter 能进入正确的重建逻辑

### 5.2 exporter `DenyHost` 非幂等，导致 detach 卡死

问题现象：

- SPDK `remove_host` 返回 `Invalid parameters`
- external-attacher detach 一直失败
- 历史 `VolumeAttachment` 无法清理

修复结果：

- `Invalid parameters` 被视为幂等清理成功
- 旧 `VolumeAttachment` 能正常收敛删除

### 5.3 `ControllerUnpublishVolume` 过度依赖请求携带 owner 信息

问题现象：

- 某些清理场景里请求没有带完整 `node_id/hostNQN`
- driver 直接拒绝 unpublish

修复结果：

- unpublish 会优先从持久 attachment / lease 中恢复 owner
- 清理路径不再依赖请求侧必须携带完整 owner

### 5.4 node 侧 stale NVMe session 导致 Pod 重建失败

问题现象：

- controller 重启后重建 Pod 时，kubelet 报 `already connected`
- node 侧把它当硬错误，无法继续恢复

修复结果：

- 若 `already connected` 且设备未 ready，node 会主动 `disconnect` 再 `connect`
- 恢复路径可继续向下推进

### 5.5 测试脚本第二阶段混入了 PVC/Namespace 变量

问题现象：

- controller 重启回归阶段重新 `apply` PVC/Namespace
- 测试变量和“纯 Pod 重建恢复”目标混在一起

修复结果：

- 第二阶段只重建 Pod，不再重新 apply PVC/Namespace
- 恢复测试更聚焦，也更接近真实重启场景

### 5.6 第二节点 `plugin-bin` 宿主路径缺失，导致 CSI node 无法启动

问题现象：

- 第二节点加入集群后，`fastblock-csi-node` Pod 停留在 `ContainerCreating`
- kubelet 报错：
  `hostPath type check failed: /root/fastblock-addcsi/csi/bin is not a directory`

修复结果：

- 将当前节点的 `csi/bin` 同步到 `10.211.55.29`
- 重新滚动 `fastblock-csi-node` DaemonSet
- 第二节点 node Pod 最终成功 `Running`

### 5.7 第二阶段 controller 重启后误删当前卷

问题现象：

- controller 重启回归阶段，当前仍然 `Bound` 的卷 image 被 monitor 删除
- 后续 attach 报 `getImageNotFound`

修复结果：

- 修正 exporter `GetExport` 对 not-found 的归一
- 修正 exporter `DenyHost` 幂等
- 放宽 `ControllerUnpublishVolume` 对请求 owner 的依赖
- 收紧重启测试脚本变量，避免第二阶段重复 apply PVC/Namespace
- 修复后，单机环境下 controller 重启后重建 Pod 已再次成功

### 5.8 controller 重启后 node 侧 stale NVMe session 阻塞恢复

问题现象：

- controller 重启后新 Pod 重建时，node 侧 `nvme connect` 报 `already connected`
- kubelet 无法完成后续块设备映射

修复结果：

- node backend 在 `already connected` 且设备未 ready 的场景下，会主动 `disconnect` 后重新 `connect`
- 单机回归已证明该修复能够恢复第二阶段 Pod 重建

## 6. 当前判断

基于当前结果，可以认为：

- 单机环境下，CSI 主链路已经具备生产前验证价值
- 单机 pre-prod 回归已经基本跑通
- controller 重启恢复路径在单机 block 卷场景下已经得到实证验证

也就是说，当前状态已经不再是“只能跑 happy path demo”，而是具备了单机生产前验证的基础稳定性。

## 7. 当前仍然存在的风险

虽然单机 pre-prod 已经基本通过，但以下风险仍然存在：

- 多节点 K8s 验证尚未完整通过
- 还没有完成 exporter / node / host backend 的更系统化重启矩阵测试
- 当前 lease/fencing 主要仍是控制面语义，尚未进入 fastblock 数据面强 fencing
- 正式部署物仍然不是最终生产形态
- snapshot / 文件系统卷 / 扩容 / 安全能力仍未完成

## 8. 多节点测试详细记录

### 8.1 测试目标

多节点测试当前目标分成三层：

1. 第二节点是否能成功加入集群并运行 CSI node
2. 控制面是否能在多节点下正确创建 PVC、创建 `VolumeAttachment`
3. 第二节点本地是否能真正完成 block 设备映射并把 Pod 拉起

### 8.2 执行步骤

本轮执行过的关键步骤如下：

1. 验证 `10.211.55.29` 与当前节点网络联通
2. 验证并修复 SSH 访问
3. 使用与当前 control-plane 相同的 `k3s v1.31.5+k3s1` 将 `10.211.55.29` 加入集群
4. 检查集群节点：
   - `kerneldev` control-plane
   - `fastblockdev` worker
5. 为第二节点准备运行环境：
   - 创建 `/root/fastblock-addcsi/csi/bin`
   - 同步 `fastblock-csi-node` 二进制
   - 准备 `/etc/nvme/hostnqn`
   - 加载 `nvme_fabrics` / `nvme_tcp` / `nvme_rdma`
6. 滚动重启 `fastblock-csi-node` DaemonSet
7. 使用 `fastblock-tcp` StorageClass 开始多节点 PVC/Pod 测试
8. 分别尝试：
   - Pod 固定到第二节点 `fastblockdev`
   - Pod 固定到第一节点 `kerneldev`

### 8.3 已观察到的结果

#### 已通过

- 第二节点成功加入 K3s
- 第二节点 `fastblock-csi-node` DaemonSet 最终 `Running`
- 多节点测试用 PVC 成功 `Bound`
- `VolumeAttachment` 成功创建
- `SuccessfulAttachVolume` 事件明确出现

#### 当前 blocker

当前多节点场景还没有完整通过，卡在：

- Pod 已经完成 `AttachVolume`
- 但长时间停留在 `ContainerCreating`
- 问题位置已经明显下沉到：
  - node 本地块设备映射
  - kubelet block device publish
  - `/sys/block` / `/dev` 呈现差异

### 8.4 第二节点本地观测

在 `10.211.55.29` 上已经确认：

- `/dev/nvme0n1` 存在
- stage state 中记录的 `device_path` 为 `/dev/nvme0n1`
- staging symlink 也指向 `/dev/nvme0n1`

但同时系统还存在额外的控制器视图：

- `/sys/block/nvme0c0n1`

也就是说：

- 控制面和 node 插件记录的设备路径本身是正确的
- 多节点剩余问题更像是 kubelet / 内核 / 设备命名视图差异导致的 block publish 阶段异常

### 8.5 当前多节点阶段性结论

当前多节点测试可以明确得出：

- **多节点集群扩容成功**
- **第二节点 CSI node 部署成功**
- **PVC 供给成功**
- **VolumeAttachment 创建成功**
- **真正的剩余 blocker 已经收敛到第二节点本地 block 设备映射阶段**

也就是说，多节点问题已经不再是 controller / exporter / lease 主链问题，而是 node / kubelet / NVMe 设备视图层面的兼容性问题。

## 8. 下一步建议测试

建议后续优先做：

### 8.1 多节点验证

- 两节点或三节点 K8s 环境
- 同卷跨节点挂载冲突
- node failover
- 第二节点 block device publish 完整打通

### 8.2 重启矩阵

- controller 重启
- exporter 重启
- node 插件重启
- host backend（monitor/exporter/nvmf/osd）重启

### 8.3 清理与收敛

- stale attachment
- stale export
- stale lease
- 残留 `VolumeAttachment`
- 残留 `Released PV`

### 8.4 强 fencing 评估

- 评估是否需要把 lease/token 检查下沉到 fastblock 数据面
- 评估 exporter ACL 与数据面写入拒绝之间是否需要进一步联动

## 9. 当前结论

当前 fastblock CSI 已经达到：

- **可以进行生产前测试**
- **单机场景下主链路基本可用**
- **重启恢复已具备初步可靠性**

但还没有达到：

- 多节点生产可放量
- 数据面强 fencing 完整落地
- 部署物彻底正式化

因此当前最合理的工程阶段定义是：

- **实现主干基本完成**
- **单机 pre-prod 验证通过**
- **多节点测试已进入真实问题定位阶段**
- **下一阶段重点转向多节点 node/kubelet block 映射兼容性与更强故障测试**

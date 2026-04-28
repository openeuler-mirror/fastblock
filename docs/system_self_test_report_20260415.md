# fastblock 系统级自测总报告

## 0. 测试人员入门
本节面向第一次接触 fastblock 的测试人员，目标不是讲清所有存储原理，而是帮助读者快速理解：
- 这个系统是做什么的
- 报告里常见名词分别代表什么
- 每类测试想证明什么
- 看到哪些现象通常算通过，看到哪些现象需要重点关注

### 0.1 用一句话理解 fastblock
fastblock 可以先理解为一个“分布式块存储系统”：
- 后端由多个 OSD 节点保存数据副本
- 前端可以把存储能力以 block device 的形式提供给客户端、虚拟机或主机

如果只从测试角度理解，可以把它看成：
- 一套由 monitor 负责管理、由多个 OSD 负责存数据、由 client 发起读写和查询状态的分布式存储系统

### 0.2 测试里最常见的几个组件
- `monitor`：负责维护集群元数据，记录 OSD 状态、pool/pg 信息、成员变更结果，并向各组件下发集群视图
- `OSD`：真正保存数据和副本的存储进程；故障恢复、成员变更、snapshot 等核心动作最终都发生在 OSD 上
- `client`：负责查询集群状态、创建 pool/image、发起基础读写验证
- `vhost` / `nvmf-tgt`：把 fastblock 的 image 以外部协议导出给虚拟机或物理主机使用
- `block_bench` / 测试程序：用于执行基础写读校验、压力写入、专项场景触发和结果验证

### 0.3 看报告前需要认识的术语
- `pool`：逻辑存储池，可以理解为一组逻辑上的存储空间
- `PG`：Placement Group，可理解为数据分布和副本管理的基本单元；报告里很多状态都是围绕 PG 变化展开的
- `replica`：副本数；例如 `3 replica` 表示同一份数据通常会保留 3 份副本
- `leader / follower`：某个 PG 对应的 raft 组里会有一个 leader 和若干 follower；写入、复制、成员变更等流程通常由 leader 主导
- `snapshot`：当落后副本已经追不上旧日志时，用对象快照和索引状态把它一次性拉回到可继续追平的状态
- `remap`：某个 PG 从一组 OSD 迁移到另一组 OSD 的过程，常见于 OSD 故障、out/in 或成员变化后

下面这些状态词在看测试结果时最常见：
- `up`：对应 OSD 当前在线、可通信
- `down`：对应 OSD 当前离线或不可通信
- `in`：对应 OSD 仍被视为集群成员，仍参与数据放置
- `out`：对应 OSD 已被移出当前数据放置范围
- `active`：PG 已收敛到正常可工作的状态
- `undersize`：PG 的副本数暂时不足，通常表示某个副本缺失或还没恢复完成
- `remapped`：PG 正在从旧的 OSD 组合迁移到新的 OSD 组合

测试时可以用一个简单判断原则：
- `active` 往往代表“当前已经恢复到可工作状态”
- `down`、`out`、`undersize`、长时间 `remapped` 往往代表“过程还没收敛或者已经出现异常”

### 0.4 这份报告到底在测什么
本报告不是单纯验证“命令能不能跑”，而是按系统能力分域回答下面几个问题：
- 构建与交付：代码能不能编译、安装，二进制是否齐全
- 控制面：monitor、OSD、pool、pgmap、osdmap 是否能正常建立和收敛
- 数据面与正确性：基础写读是否可用，写后读校验是否正确，RDMA 路径是否真的被走到
- 分布式一致性与副本管理：raft 选主、日志复制、成员变更、snapshot 恢复是否具备可验证证据
- 故障处理与恢复：OSD 故障、out/in、remap 后，集群和 IO 是否能恢复
- 对外接入协议：vhost、NVMe-oF 是否能被外部主机发现、连接并执行基础读写
- 性能基线：至少是否有一组基础性能数据可参考
- 运维与配置约束：部署、日志、内存参数等是否已有清晰说明

### 0.5 测试人员怎么判断“通过”或“异常”
常见“通过”信号包括：
- `status`、`getpgmap`、`getosdmap` 这类基础查询命令能够稳定返回
- 集群最终收敛到类似 `8 active` 这样的状态
- 基础写读脚本最终输出 `Write-read-verify completed`
- 成员变更后，`pgmap` 能切换到目标成员集合，且后续基础 IO 仍可通过
- snapshot 恢复后，落后副本能够重新追平，对象内容可以正确读回
- NVMe-oF 或 vhost 场景下，外部主机能够 discover/connect，并完成 `mkfs`、挂载或 `fio` 烟测

常见“异常”信号包括：
- 查询命令经常超时或无返回
- PG 长时间停留在 `down`、`undersize`、`remapped`，没有回到 `active`
- `change_nodes` 返回非 0，或看不到成员变更完成后的 `pgmap` 收敛
- 日志里已经看到 `snapshot` 开始，但集群最终没有恢复到健康状态
- 写后读校验失败，或者对象内容读回不一致
- 协议侧可以发现设备，但无法连接、格式化、挂载或完成最小读写

说明：
- 在故障恢复、成员变更、snapshot 执行过程中，短时间出现 `undersize` 或 `remapped` 并不一定是失败
- 真正需要关注的是这些状态是否能在预期时间内重新收敛到 `active`

### 0.6 建议的阅读和执行顺序
对第一次接触 fastblock 的测试人员，建议按下面顺序使用这份报告：
1. 先看“5.2 基础健康检查证据”，理解最基础的可见性检查命令和输出。
2. 再看“4. 当前覆盖矩阵”，知道哪些能力已经验证、哪些仍是缺口。
3. 然后按测试域阅读“5. 关键测试证据摘要”，重点看每一类测试想证明什么、最终结果是什么。
4. 如果要执行专项测试，再看对应的成员变更、snapshot 或协议层结果摘录。
5. 最后看“8. 后续补测优先级”，理解当前最关键的待修问题在哪里。

执行测试前，建议先完成这 3 个最小检查：
- `status` 是否能正常返回
- `getpgmap` 是否显示 PG 已收敛
- `getosdmap` 是否能看到 OSD 的 `up/in` 状态

如果测试过程中出现异常，优先保留以下证据：
- monitor 日志
- 相关 OSD 日志
- 客户端命令输出
- `status`、`getpgmap`、`getosdmap` 的现场结果

### 0.7 常见操作速查
本节整理测试人员最常用的一组命令，优先面向“在仓库根目录下、本地使用 `vstart.sh` 启动 dev cluster”的场景。

说明：
- 以下命令默认在仓库根目录执行
- 以下命令默认使用本地开发集群配置文件：`.vstart/etc/fastblock/fastblock.json`
- 如果是 systemd 或生产环境，请把配置文件路径、日志路径和 IP 地址替换为实际值

#### 0.7.1 准备 RDMA 开发环境并启动本地集群
如果机器没有现成 RDMA 网卡，而是使用 `rdma_rxe` 做开发验证，可先执行：

```bash
./scripts/create-rdma-rxe.sh -n rdmanic
```

启动一个 `3 OSD / 3 replica` 的本地开发集群：

```bash
./vstart.sh -m dev -c 3 -r 3 -C 1 -n rdmanic
```

如果只是本机最简开发环境，也可以直接使用：

```bash
./vstart.sh -m dev -c 3 -r 3 -C 1
```

通常看到 monitor 和 OSD 进程起来后，就可以继续做状态检查。

#### 0.7.2 查询集群状态
最常用的 5 个检查命令如下：

```bash
./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=status
./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=getpgmap
./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=getosdmap
./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=getpgosds -poolid=1
./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=listpools
```

可按下面方式理解输出：
- `status`：先看有多少个 OSD 是 `up/in`
- `getpgmap`：先看 PG 是否大部分或全部回到 `active`
- `getosdmap`：看 OSD 的 `up/down`、`in/out`、地址和端口
- `getpgosds`：看某个 pool 下 PG 到 OSD 的映射关系
- `listpools`：看当前已经创建了哪些 pool

#### 0.7.3 创建 pool 和 image
如果需要手工准备测试对象，可执行：

```bash
./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=createpool -poolname=fbtest -pgcount=8 -pgsize=3
./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=createimage -poolname=fbtest -imagename=fbtestimg -imagesize=536870912
```

说明：
- `createpool` 用于创建逻辑存储池
- `createimage` 用于在指定 pool 下创建测试 image
- `imagesize=536870912` 表示创建一个 `512 MiB` 的测试 image

如果只是使用 `vstart.sh` 默认创建的开发 pool，也可以直接沿用已有的 `fb` pool 做测试。

#### 0.7.4 执行基础写读校验
最常用的基础数据面校验脚本是：

```bash
BLOCK_BENCH_CPU_MASK=0x8 ./scripts/run-dev-write-read-verify.sh
```

默认行为：
- 脚本会自动生成一个临时测试 image 名称
- 先执行一轮顺序写
- 再执行一轮带读回校验的顺序读

通常看到下面这类输出，就可以认为基础写读验证通过：
- 写阶段和读阶段都正常完成
- 最终输出 `Write-read-verify completed`

#### 0.7.5 执行成员变更专项命令
如果需要复现成员变更基础场景，可使用报告中已经验证过的示例：

```bash
./build/src/test/raft_membership \
  --no-huge -s 1024 \
  -m 0x10 \
  -C .vstart/etc/fastblock/fastblock.json \
  -I 1 \
  -o 10.211.55.29 \
  -t 9818 \
  -P 1 \
  -G 0 \
  -N change \
  -O '2:10.211.55.29:9087,3:10.211.55.29:9232,4:10.211.55.29:9965'
```

结果判读：
- 如果输出 `change membership in the pg 1.0 result: 0`，表示这次成员变更请求提交成功
- 提交成功后，还需要继续执行 `getpgmap`，确认 PG 成员集合已经切换到目标成员集合
- 最后最好再补一轮基础写读校验，确认变更后基础 IO 仍可工作

说明：
- 这个命令依赖实际 leader 地址和目标 OSD 地址，复用时要按现场环境替换 IP 和端口
- 测试人员如果只是做日常回归，不建议手改命令参数后盲跑，应先对照专项报告确认目标 PG 和目标成员集合

#### 0.7.6 查看日志和现场证据
本地 `vstart.sh` 开发集群下，最常看的日志路径通常是：

```bash
tail -f .vstart/var/log/fastblock/monitor.log
tail -f .vstart/var/log/fastblock/osd1.log
tail -f .vstart/var/log/fastblock/osd2.log
tail -f .vstart/var/log/fastblock/osd3.log
```

如果是安装到系统目录或使用 systemd 的场景，日志通常在：

```bash
tail -f /var/log/fastblock/monitor.log
tail -f /var/log/fastblock/osd1.log
```

排查问题时，建议至少同时保留：
- monitor 日志
- 涉及到的 OSD 日志
- 执行命令时的终端输出
- 当时的 `status`、`getpgmap`、`getosdmap` 结果

对测试人员来说，最实用的做法不是一次看完所有日志，而是：
- 先用 `status` / `getpgmap` 判断问题大致发生在哪个测试域
- 再回到对应的 monitor / OSD 日志里查关键时间点附近的报错或状态变化

### 0.8 测试执行 Checklist
本节给出一份适合测试人员直接照着执行的最小 checklist。建议先完成“最小回归 checklist”，确认基础环境正常，再进入成员变更、snapshot 或协议层专项测试。

#### 0.8.1 最小回归 checklist
| 步骤 | 操作 | 预期结果 | 常见失败信号 | 建议保留证据 |
| --- | --- | --- | --- | --- |
| 1 | 准备 RDMA 开发环境（如需）并启动本地集群：`./scripts/create-rdma-rxe.sh -n rdmanic`，`./vstart.sh -m dev -c 3 -r 3 -C 1 -n rdmanic` | monitor 和 OSD 进程正常启动 | 进程起不来；启动后很快退出；提示 hugepage、网卡或配置文件问题 | 启动命令输出、`.vstart/var/log/fastblock/monitor.log`、对应 `osd*.log` |
| 2 | 执行基础状态检查：`status`、`getpgmap`、`getosdmap` | 查询命令能稳定返回；OSD 大多为 `up/in`；PG 最终回到 `active` | 查询超时；返回为空；PG 长时间停在 `down`、`undersize`、`remapped` | 命令输出、当时的 monitor 日志 |
| 3 | 如需手工准备对象，执行 `createpool`、`createimage` | pool 和 image 创建成功，没有明显报错 | `createpool` 或 `createimage` 返回失败；后续 `listpools` 看不到目标 pool | 创建命令输出、`listpools` 输出 |
| 4 | 执行基础写读校验：`BLOCK_BENCH_CPU_MASK=0x8 ./scripts/run-dev-write-read-verify.sh` | 写阶段和读阶段完成；最终输出 `Write-read-verify completed` | 写阶段失败；读校验失败；脚本中途退出 | 终端输出、相关 OSD 日志、校验时的 `getpgmap` |
| 5 | 再次执行 `status`、`getpgmap`，确认 IO 后集群状态 | 集群仍保持健康；PG 回到 `active` | 写读虽然结束，但 PG 长时间不恢复；OSD 变成 `down/out` | 命令输出、monitor 日志 |

这 5 步通过后，可以认为：
- 当前环境至少具备“可启动、可查询、可做基础写读验证”的最小测试条件
- 后续专项测试如果失败，更大概率是专项逻辑问题，而不是最基础环境没有起来

#### 0.8.2 专项测试 checklist
下面这些测试不一定每次都全跑，但如果进入专项回归，建议按表执行并记录结果。

| 测试域 | 典型操作 | 通过标准 | 失败特征 | 备注 |
| --- | --- | --- | --- | --- |
| 成员变更基础回归 | 执行 `raft_membership` 的 `change_nodes` 示例命令，再检查 `getpgmap` 并补一轮基础写读 | `change membership ... result: 0`；`pgmap` 切到目标成员集合；基础写读仍通过 | 请求返回非 0；成员集合不收敛；变更后基础 IO 失败 | 适合在相关代码修改后回归 |
| snapshot 基础回归 | 按专项程序触发 follower 掉队、补写、恢复和追平，观察 `snapshot_check` / `installsnapshot` 相关日志 | 能看到 `snapshot_check + installsnapshot` 闭环；落后副本重新追平；对象读校验通过 | 日志里没有进入 snapshot；进入后不收敛；对象内容读回不一致 | 属于中高级专项测试，建议对照专项报告执行 |
| 对外协议 smoke | 执行 vhost 或 NVMe-oF 的 discover / connect / `mkfs` / 挂载 / 最小 `fio` | 主机能发现和连接设备；最小文件系统读写可完成 | 发现不到设备；连接失败；`mkfs`、挂载或 `fio` 失败 | 适合协议层改动后回归 |
| 故障恢复回归 | 在有基础 IO 的前提下执行 OSD 故障、`outosd` 或重启类操作，再观察集群和 IO 恢复 | 短暂波动后重新收敛到 `active`；基础 IO 可恢复 | 集群长时间 `undersize` / `down`；恢复后 IO 失败 | 适合故障处理逻辑改动后回归 |

#### 0.8.3 测试记录模板
建议测试人员执行每一项时，至少记录下面这些信息，避免后续无法复盘：

| 记录项 | 建议填写内容 |
| --- | --- |
| 测试日期 | 例如 `2026-04-17` |
| 测试代码基线 | 提交号或分支名 |
| 测试环境 | 本地 dev cluster / 物理机 / 虚拟机；OSD 数量；副本数；是否使用 `rdma_rxe` |
| 执行命令 | 实际运行的完整命令 |
| 结果摘要 | 通过 / 失败 / 阻塞 |
| 关键现象 | 例如 `8 active`、`change membership ... result: 0`、`Write-read-verify completed` |
| 异常现象 | 例如 `undersize` 长时间不恢复、进程崩溃、超时、segfault |
| 证据位置 | monitor 日志、OSD 日志、终端输出、截图或保存的命令结果 |

#### 0.8.4 建议的执行节奏
如果测试时间有限，建议按下面顺序裁剪：
1. 必跑：集群启动、基础状态检查、基础写读校验。
2. 有相关代码改动时必跑：对应测试域的专项回归，例如成员变更、snapshot 或协议层 smoke。
3. 时间充足再跑：故障联动、长稳、复杂场景。

对于第一次接手 fastblock 的测试人员，建议先把“最小回归 checklist”跑顺，再进入更复杂的专项场景。

## 1. 文档定位
- 文档名称: fastblock 系统级自测总报告
- 文档日期: 2026-04-17
- 当前基线: `9f6dbbb`
- 文档目标: 以“分布式存储系统”的视角，对 fastblock 当前已验证能力、已知缺口、关键测试证据和后续补测优先级做统一归档
- 编制原则: 先定义系统级测试大纲，再按测试域逐项填充证据与结论；报告正文应尽量自包含，不要求读者再跳到其他专项文档才能理解主结论

## 2. 报告范围
本报告覆盖的不是“某一个功能是否能跑”，而是分布式存储系统应具备的几个核心维度：
- 可构建、可部署
- 控制面可工作
- 数据面可工作
- 基础一致性与正确性可验证
- 故障与恢复路径可验证
- 共识与副本管理能力可验证
- 对外协议能力可验证
- 基础性能有证据
- 运维与参数约束可说明

本报告当前不把以下项目表述为“已整体签收”：
- raft recovery / snapshot 复杂场景与完整签收
- raft 成员变更复杂场景
- 长稳压测
- 升级 / 回滚
- 自动化回归覆盖证明

## 3. 系统级自测大纲

### 3.1 构建与交付
关注点：
- 依赖安装是否可重复
- monitor / osd / client / vhost / nvmf-tgt / block_bench 是否可编译
- 安装后二进制是否完整可用

### 3.2 控制面
关注点：
- monitor 启动
- OSD 申请 ID / boot / 注册
- pool 创建
- pgmap 下发与收敛
- osdmap 下发与状态传播

### 3.3 数据面与正确性
关注点：
- block client 基础写读
- 写后读校验
- RDMA data path / write-ring 路径是否真正被走到
- 变更或故障后基础 IO 是否仍可工作

### 3.4 分布式一致性与副本管理
关注点：
- raft 基础选主
- raft 日志复制
- pg active 收敛
- raft 成员变更
- raft recovery / snapshot

### 3.5 故障处理与恢复
关注点：
- kill OSD
- out / in
- remap
- OSD 重启
- 故障后集群状态与 IO 恢复

### 3.6 对外接入协议
关注点：
- vhost + QEMU
- NVMe-oF TCP/RDMA
- 主机侧发现、连接、读写

### 3.7 性能基线
关注点：
- 单副本
- 三副本
- block_bench
- bdevperf
- vhost 性能
- nvmf 性能

### 3.8 运维与配置约束
关注点：
- RPC / 内存参数约束
- OSD / vhost 日志配置
- 本地 dev 环境与 systemd 运行方式说明

### 3.9 尚未覆盖的系统级项目
关注点：
- 长时间稳定性
- 升级 / 回滚
- 混合故障场景
- CI 自动化回归

## 4. 当前覆盖矩阵

| 测试域 | 关键项 | 当前状态 | 当前结论 | 主要缺口 |
| --- | --- | --- | --- | --- |
| 构建与交付 | 依赖安装、完整编译、安装二进制 | 已验证 | 当前代码基线可以在开发环境完成依赖安装、编译和安装 | 仍缺离线/内网交付链路专项验证 |
| 控制面 | monitor 启动、OSD boot、pool 创建、pgmap 收敛 | 已验证 | monitor、osd、pool、PG 基础管理可工作 | 复杂异常场景覆盖不足 |
| 数据面与正确性 | block 基础写读、写后读校验、RDMA write-ring | 已验证 | 基础 IO 与 RDMA write-ring 路径可工作 | 协议间交叉验证仍可补充 |
| 分布式一致性 | raft 选主、日志复制、pg active | 已验证（基础场景） | 基础 raft 运行路径可工作 | snapshot / recovery 复杂场景仍缺补测 |
| raft recovery / snapshot | 日志不足 recovery、snapshot_check、installsnapshot、恢复后继续追平 | 已验证（基础场景） | 已验证落后 follower 在 leader 旧日志被 trim 后，可通过 `snapshot_check + installsnapshot` 完成恢复并重新追平，且恢复后对象读校验通过 | leader 切换中的 snapshot 当前失败；成员变更联动场景当前失败；从 0 bootstrap、长稳仍需补测 |
| raft 成员变更 | 无预创建 PG 的 `change_nodes`、monitor pgmap 更新、变更后基础 IO 回归 | 已验证（基础场景） | 基础成员变更路径已打通，变更后 pgmap 能切到新成员集合，且可继续完成基础写读校验 | leader 切换中的成员变更场景当前失败；与 snapshot/recovery 联动场景当前失败；长稳未补 |
| 故障处理与恢复 | kill OSD、out/in、remap、故障后基础 IO | 已验证（基础场景） | 基础故障恢复路径可工作 | 更复杂多故障场景未补 |
| 对外接入协议 | vhost、QEMU、NVMe-oF | 已验证 | 对外协议面已有功能证据 | 与成员变更/故障联动的协议层回归未补 |
| 性能基线 | 单副本、三副本、vhost、nvmf、block_bench | 已验证 | 性能已有基础数据集 | 长稳性能与回归基线未自动化 |
| 运维与配置约束 | RPC 参数、日志配置 | 已验证 / 已有文档沉淀 | 可支撑部署与调参说明 | 缺统一运维手册 |
| 系统级长期能力 | 长稳、升级、CI 回归 | 待验证 | 当前不能给出系统级签收结论 | 需要单独补测 |

## 5. 关键测试证据摘要

### 5.1 构建与交付证据
已完成以下关键动作：
- `./install-deps.sh`
- `./build.sh -t Release -c monitor`
- `./build.sh -t Release -c osd`
- `cd build && make install`

结果：
- `fastblock-mon`
- `fastblock-client`
- `fastblock-osd`
- `fastblock-vhost`
- `fastblock-nvmf-tgt`
- `block_bench`
均可生成并安装。

结论：
- 当前代码基线具备可构建、可安装的交付能力。

### 5.2 基础健康检查证据
系统级自测最前面应有一层“基础健康检查/可见性检查”，用于回答：
- monitor 是否可访问
- 集群状态是否可见
- pgmap 是否可见
- osdmap 是否可见
- PG 到 OSD 的映射是否可解释

当前可直接使用的检查命令包括：
- `./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=status`
- `./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=getpgmap`
- `./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=getosdmap`
- `./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=getpgosds -poolid=<pool_id>`
- `./monitor/fastblock-client -conf=.vstart/etc/fastblock/fastblock.json -op=listpools`

说明：
- 当前工具集中没有单独名为 “osd tree” 的直接命令。
- 但 `getosdmap` 可以展示 OSD 的 up/in、地址、端口等动态信息；`getpgosds` 和 `getpgmap` 可以展示 PG 到 OSD 的映射关系。
- 对当前这套系统，自测报告里可以把这三类输出作为“基础拓扑可见性”的实际证据。

建议口径：
- 基础健康检查通过后，才进入数据面、一致性、故障恢复和协议层验证。
- 如果基础健康检查都无法稳定返回，那么后续深层测试结论不应视为有效。

### 5.3 控制面证据
在本地 dev 环境中，已多次验证：
- monitor 可启动
- OSD 可完成 `applyid` / `boot`
- pool `fb` 可创建
- `getpgmap` 可正常返回
- PG 可收敛到 `active`

典型结果：
- `4 osds: 4 up, 4 in`
- `8 active`

结论：
- 控制面基础链路可工作。

### 5.4 数据面与正确性证据
已完成本地基础写读校验：
- 通过 `run-dev-write-read-verify.sh` 完成写后读校验
- 写阶段完成 64 次 IO
- 读阶段完成 64 次 IO
- 脚本最终输出 `Write-read-verify completed`

同时，开发环境再次确认客户端运行日志中出现 `reacquiring write ring`，说明 RDMA write-ring 路径确实被走到。

结论：
- 数据面基础写读链路可工作。
- RDMA write-ring 路径有直接运行证据。

### 5.5 故障与恢复证据
已有验证覆盖：
- kill OSD
- `outosd`
- PG remap
- 故障后基础 IO 恢复

此前已修复过 failover 路径中的 raft/连接清理问题，并通过基础 failover 烟测：
- 故障后集群可维持 `3 up / 3 in`
- PG 可保持 `active`
- 故障后写读校验可通过

结论：
- 基础故障恢复路径可工作。

### 5.6 对外协议与性能证据
已有资料显示以下能力具备独立证据：
- vhost + QEMU 功能验证
- NVMe-oF TCP/RDMA 功能验证
- 单副本 / 三副本性能数据
- vhost 性能数据
- nvmf 性能数据

结论：
- 当前系统不仅有控制面和数据面基础能力，也有对外协议和性能侧的初步证据支撑。

#### 5.6.1 本轮新增：NVMe-oF TCP 功能链实测
本轮新增对 `fastblock-nvmf-tgt` 做了一次从 target 到内核 initiator 的完整功能验证，目标是确认：
- fastblock image 能否被 `fastblock-nvmf-tgt` 正常导出为 NVMe-oF namespace
- Linux 内核 initiator 能否完成发现、连接、枚举设备
- 导出块设备是否能完成基础写读校验

测试环境：
- `3 OSD / 3 replica` 本地 RDMA dev cluster
- pool: `fb`
- image: `nvmfimg2`
- nvmf target: `fastblock-nvmf-tgt`
- initiator: Linux 内核 `nvme-tcp`

测试步骤：
- 创建测试 image：
  - `fastblock-client -op=createimage -poolname=fb -imagename=nvmfimg2 -imagesize=536870912`
- 启动 `fastblock-nvmf-tgt`
  - 本地开发机未配置 hugepage，因此本轮使用：
    - `fastblock-nvmf-tgt --no-huge -s 4096 -C .vstart_nvmf/etc/fastblock/fastblock.json -S 1`
- 通过 target RPC socket 创建 fastblock bdev、TCP transport、subsystem、namespace、listener
- 在 host 侧执行：
  - `nvme discover -t tcp -a 10.211.55.29 -s 4420`
  - `nvme connect -t tcp -n nqn.2016-06.io.spdk:cnode1 -a 10.211.55.29 -s 4420`
  - `nvme list`
- 在导出的 `/dev/nvme0n1` 上执行：
  - `mkfs.ext4 -F /dev/nvme0n1`
  - `mount /dev/nvme0n1 /mnt/nvmf_test`
- 在挂载后的 ext4 文件系统上执行一轮最小 `fio`：
  - `fio --name=nvmf_smoke --directory=/mnt/nvmf_test --filename=fio_smoke.dat --size=64M --bs=4k --iodepth=4 --rw=randrw --rwmixread=50 --ioengine=libaio --direct=1 --numjobs=1 --runtime=10 --time_based=1 --group_reporting=1`

关键证据：
- initiator 发现结果：
  - `nvme discover` 返回 `subnqn: nqn.2016-06.io.spdk:cnode1`
  - listener 地址为 `10.211.55.29:4420`
- initiator 连接结果：
  - `nvme connect` 输出 `connecting to device: nvme0`
  - `nvme list` 显示：
    - `/dev/nvme0n1`
    - `SN = SPDK00000000000001`
    - `Model = SPDK_Controller1`
    - `Usage = 536.87 MB / 536.87 MB`
- target 侧 `nvmf_get_subsystems` 返回：
  - subsystem `nqn.2016-06.io.spdk:cnode1`
  - namespace `nvmf_fbdev`
  - listener `TCP 10.211.55.29:4420`
- 文件系统与 `fio` 结果：
  - `mkfs.ext4 -F /dev/nvme0n1` 成功完成
  - `mount /dev/nvme0n1 /mnt/nvmf_test` 后容量约 `488M`
  - `fio` 运行结束 `err=0`
  - `fio` 关键结果：
    - READ: `IOPS=158`, `BW=636 KiB/s`
    - WRITE: `IOPS=165`, `BW=663 KiB/s`
    - `Disk stats: nvme0n1 util=99.07%`

本轮结论：
- `NVMe-oF TCP + 内核 initiator` 的基础功能链已验证通过
- 这次已覆盖：
  - target 启动
  - fastblock bdev 导出
  - subsystem / namespace / listener 配置
  - host 发现与连接
  - `mkfs.ext4`
  - 挂载后的文件系统级 `fio` 烟测

说明：
- 本轮为了适配本地小内存开发机，`fastblock-nvmf-tgt` 使用了 `--no-huge -s 4096`
- `rpc.py` 中 `bdev_fastblock_create` 的 Python 封装会额外携带 `pool_id: null`，与当前服务端 decoder 不兼容；本轮通过直接发送最小 JSON-RPC 请求绕过该问题
- 本轮未复测：
  - nvmf 性能
  - nvmf 与成员变更/故障恢复联动

#### 5.6.2 本轮新增：NVMe-oF RDMA initiator 长稳回归
本轮新增在本地 `rdma_rxe` 开发环境下，对 `fastblock-nvmf-tgt` 的 `NVMe-oF RDMA + Linux 内核 initiator` 做了一次带空闲窗口的长时间回归，目标是验证：
- `RDMA initiator` 是否能完成 discover / connect / 枚举设备
- 在 `write-ring lease` 过期窗口之后，客户端是否会重新获取 write ring，而不是继续使用旧的 `remote_addr / rkey`
- 在“长压一轮 -> 空闲超过 lease -> 再长压一轮”的场景下，导出块设备是否仍能稳定完成 IO

测试日期：
- `2026-04-19`

测试环境：
- `3 OSD / 3 replica` 本地 RDMA dev cluster
- RDMA 设备：`rdmanic` (`rdma_rxe`)
- hugepage：`vm.nr_hugepages = 2048`
- pool: `fb`
- image: `nvmf_rdma_redeploy`
- nvmf target: `fastblock-nvmf-tgt`
- initiator: Linux 内核 `nvme-rdma`

测试步骤：
- 使用全新的本地状态目录重新部署：
  - `FASTBLOCK_VSTART_STATE_ROOT=.vstart_nvmf_rdma_redeploy ./vstart.sh -m dev -c 3 -r 3 -C 1 -n rdmanic`
- 确认集群状态：
  - `3 up / 3 in`
  - `8 active`
- 创建测试 image：
  - `fastblock-client -op=createimage -poolname=fb -imagename=nvmf_rdma_redeploy -imagesize=536870912`
- 启动 `fastblock-nvmf-tgt`：
  - `fastblock-nvmf-tgt -s 2048 -C .vstart_nvmf_rdma_redeploy/etc/fastblock/fastblock.json -S 1`
- 通过 target RPC socket 创建：
  - fastblock bdev
  - RDMA transport
  - subsystem
  - namespace
  - RDMA listener `10.211.55.29:4420`
- 在 host 侧执行：
  - `nvme discover -t rdma -a 10.211.55.29 -s 4420`
  - `nvme connect -t rdma -n nqn.2016-06.io.spdk:cnode1 -a 10.211.55.29 -s 4420`
- 对导出的 `/dev/nvme0n2` 执行一笔 `4KiB` 预热写
- 执行第 1 轮 `fio`：
  - `runtime=120s`
  - `bs=4k`
  - `iodepth=8`
  - `randrw`
- 空闲 `60s`
- 执行第 2 轮 `fio`：
  - 同样 `runtime=120s`
  - 同样 `bs=4k`
  - 同样 `iodepth=8`
  - 同样 `randrw`

关键证据：
- initiator 发现与连接：
  - `nvme discover` 返回 `subnqn: nqn.2016-06.io.spdk:cnode1`
  - `nvme connect` 输出 `connecting to device: nvme1`
  - `nvme list` 显示 `/dev/nvme0n2`
  - `SN = SPDK00000000000001`
  - `Model = SPDK_Controller1`
- target 侧 `nvmf_get_subsystems` 返回：
  - subsystem `nqn.2016-06.io.spdk:cnode1`
  - namespace `nvmf_fbdev`
  - listener `RDMA 10.211.55.29:4420`
- 第 1 轮 `fio` 结果：
  - `err=0`
  - READ: `IOPS=418`, `BW=1672 KiB/s`
  - WRITE: `IOPS=415`, `BW=1662 KiB/s`
- 空闲 `60s` 后，target 侧日志出现：
  - `write ring lease expired or is expiring ..., reacquiring`
- 第 2 轮 `fio` 结果：
  - `err=0`
  - READ: `IOPS=395`, `BW=1583 KiB/s`
  - WRITE: `IOPS=393`, `BW=1573 KiB/s`
- 两轮 `fio` 期间，本次 `dmesg` 未新增：
  - `nvme ... timeout`
  - `starting error recovery`
  - `rdma connection establishment failed`
  这类新的 `NVMe-oF RDMA` 故障

本轮结论：
- `NVMe-oF RDMA + Linux 内核 initiator` 的 discover / connect / 长时间块设备级 IO 已验证通过
- 这次额外确认了：
  - `write-ring lease` 过期后，客户端会重新获取 write ring
  - 不会在空闲窗口后继续使用旧的 `remote_addr / rkey`
  - “`120s fio -> idle 60s -> 120s fio`” 场景下，RDMA initiator 未再复现此前的 timeout / reconnect 故障

边界说明：
- 本轮验证的是 `NVMe-oF RDMA` 协议链与 `write-ring lease` 修复后的局部长稳回归
- 这不等同于：
  - 全系统长稳压测
  - 成员变更 / 故障恢复联动下的协议层长稳
  - 发布级长期稳定性签收

### 5.7 raft recovery / snapshot 证据
本轮新增完成了基于本地 RDMA dev cluster 的 snapshot 恢复专项验证，目标是验证：
- 当 follower 落后到 leader 已经 trim 掉旧日志后
- 系统是否会从普通 raft log recovery 切换到 `snapshot_check + installsnapshot`
- follower 是否能完成对象安装、推进 raft 索引并重新追平

测试方法：
- 使用 `3 OSD / 3 replica` 本地 RDMA dev cluster
- 选定 `PG 1.0`
- 下线一个 follower
- 使用 [snapshot_pg_stress.cc](./../src/test/snapshot_pg_stress.cc) 对 `PG 1.0` 的固定 object 持续写入
- 为了快速触发 trim，自测时临时下调了 raft log trim 阈值
- 拉回 follower
- 再打一笔新的 `PG 1.0` 写入触发 recovery

本轮同时确认并固化了本地 dev 环境下需要的消息参数：
- 小内存开发机场景下：
  - `msg_server_data_memory_pool_capacity = 2048`
  - `msg_client_data_memory_pool_capacity = 2048`
- 为避免 payload snapshot 被 RPC timeout 打断：
  - `msg_server_rpc_timeout_us = 600000000`
  - `msg_client_rpc_timeout_us = 600000000`

关键运行证据：
- leader 侧日志已出现：
  - `do_recovery`
  - `recovery by snapshot`
  - `send snapshot_check_request`
  - `send_snapshot`
  - `send installsnapshot_request`
  - `recvd installsnapshot_response`
- follower 侧日志已出现：
  - `recvd snapshot_check_request`
  - `snapshot_check ready`
  - `recvd installsnapshot_request ... object_size 1`
  - `installsnapshot write_fn done=0`
  - `recvd installsnapshot_request ... done=1 object_size 0`
  - `set _last_applied_idx, _current_idx, _commit_idx to ...`

最终结果：
- follower 对 payload snapshot 的第一批对象安装成功
- 随后收到收尾批次并推进：
  - `_last_applied_idx`
  - `_current_idx`
  - `_commit_idx`
- leader 收到 `installsnapshot_response success: 1`
- leader 日志出现 `snapshot is end.`
- 随后 follower 能继续接收并 apply 新的普通日志
- 集群最终恢复到：
  - `3 up / 3 in`
  - `8 active`

本轮结论：
- `raft recovery / snapshot` 的基础闭环已经验证通过
- 之前阻塞该场景的主要问题有两类：
  - recovery 期间 heartbeat 干扰与 `E_INVAL` 处理不当
  - 小内存 dev 配置下 `msg_*_data_memory_pool_capacity` 太小，导致 payload installsnapshot 无法发送
- 这两类问题已经分别通过：
  - [raft_server.cc](./../src/raft/raft_server.cc)
  - [vstart.sh](./../vstart.sh)
 进行修正和固化

仍未覆盖的 snapshot 范围：
- 完全“从 0 开始”的空节点 bootstrap
- snapshot 与成员变更联动
- snapshot 过程中的复杂故障
- 长时间稳定性

### 5.8 snapshot 复杂场景补测结果
在基础 snapshot 闭环通过之后，又补了三条更接近系统级使用方式的场景。

#### 5.8.1 场景一：snapshot 恢复过程中的 leader 切换
测试目标：
- 验证旧 leader 已经开始 `snapshot` 恢复后，如果 leader 故障，系统是否能由新 leader 接手恢复并重新收敛。

测试方法：
- `3 OSD / 3 replica` 本地 RDMA dev cluster
- `PG 1.0` 初始 leader 为 `osd2`
- 下线 follower `osd1`
- 对 `PG 1.0` 的固定 object 连续写入，逼出 snapshot 恢复条件
- 拉回 `osd1`
- 在旧 leader 已经开始发送 `installsnapshot` 后，强制 `kill -9` 当前 leader

关键证据：
- 旧 leader 日志出现：
  - `recovery by snapshot to node 1`
  - `send installsnapshot_request to node 1`
  - `recvd installsnapshot_response ... success: 1`
- 说明旧 leader 在被杀之前，snapshot 恢复已经真实开始并至少完成过一轮 install/response 往返

最终结果：
- leader 故障后，集群没有直接崩溃
- 但也没有重新收敛到健康状态，而是停在：
  - `2 up / 2 in`
  - `8 undersize`

结论：
- “snapshot 恢复过程中的 leader 切换”当前未通过
- 系统可以顶住旧 leader 被打掉，但新 leader 没有把这条恢复链重新收口到 `active`

#### 5.8.2 场景二：snapshot 恢复后的对象内容读校验
测试目标：
- 验证 payload snapshot 恢复后，不仅索引推进正确，而且对象内容也能正确读回。

测试方法：
- `3 OSD / 3 replica` 本地 RDMA dev cluster
- 选择 `PG 1.0`
- 下线 follower `osd2`
- 对固定 object `snap_obj_case2_verify` 连续写入
- 拉回 `osd2`
- 再打一笔 trigger 写入触发 recovery
- 随后对 `snap_obj_case2_verify` 执行读校验

关键证据：
- follower `osd2` 日志出现：
  - `recvd installsnapshot_request ... object_size 1`
  - `installsnapshot write_fn done=1`
  - `set _last_applied_idx, _current_idx, _commit_idx to 41`
- 随后客户端读校验输出：
  - `snapshot_pg_stress read verify success, object=snap_obj_case2_verify size=4096`

最终结果：
- follower 完成 payload snapshot 安装并推进索引
- 集群恢复为：
  - `3 up / 3 in`
  - `8 active`
- 原对象内容能够正确读回并通过校验

结论：
- “snapshot 恢复后的对象内容读校验”已通过

#### 5.8.3 场景三：成员变更后的新成员掉队，能否在新成员集合下通过 snapshot 追平
测试目标：
- 验证 `change_nodes` 完成后，如果新成员掉队，系统是否会在“变更后的成员集合”内通过 snapshot 把它追平，而不是提前退化成 monitor remap。

测试方法：
- 使用 `4 OSD / 3 replica` 本地 RDMA dev cluster
- 初始 `PG 1.0` 成员集合为 `[1,2,4]`
- 通过 `change_nodes` 将 `PG 1.0` 切换到新成员集合 `[3,4,2]`
- 随后强制下线新成员 `osd3`
- 对变更后的 `PG 1.0` 固定 object `snap_obj_case3_link` 连续写入 `2000` 次，尝试把 `osd3` 拉到只能靠 snapshot 追平的落后窗口
- 再拉回 `osd3`，观察 `pgmap` 和 snapshot 关键日志

关键证据：
- `raft_membership` 输出：
  - `leader of the pg 1.0 is 2`
  - `change membership in the pg 1.0 result: 0`
- monitor 日志出现：
  - `Received PgMemberChangeFinishRequest, pg 1 . 0`
  - `Finalize pg member change, pg 1 . 0 old osdList [1 2 4] new osdList [3 4 2]`
- 变更完成后，`getpgmap` 显示：
  - `1.0 active [3,4,2]`
- 下线 `osd3` 后，monitor 日志出现：
  - `osd 3 from up to down`
  - 随后 `osd 3 from in to out`
- 同时 `PG 1.0` 从变更后的成员集合退化为：
  - `1.0 active [2,1,4]`
  - `osd3` 拉回后进一步进入 `1.0 remapped [2,1,4] -> [2,1,3]`
- 本轮没有抓到任何：
  - `recovery by snapshot`
  - `send snapshot_check_request`
  - `send installsnapshot_request`
  - `recvd installsnapshot_request`
  这类成员变更联动 snapshot 的关键日志

最终结果：
- `change_nodes` 本身成功，`PG 1.0` 确实切换到了目标成员集合 `[3,4,2]`
- 但当新成员 `osd3` 掉队后，系统优先进入 monitor 的 `down -> out -> remap` 路径
- `osd3` 拉回后，`PG 1.0` 没有在变更后的成员集合内通过 snapshot 追平，而是退化成新的 remap 过程

结论：
- “成员变更与 snapshot/recovery 联动”当前未通过
- 当前失败模式不是 `change_nodes` 提交失败，而是：
  - 成员变更后的新成员一旦掉队，系统会优先进入 monitor remap/auto-out
  - 没有观察到在新成员集合内完成 snapshot catch-up 的闭环

## 6. raft 成员变更专项结果摘录
本节是系统总报告的一部分，直接纳入成员变更专项验证结论，不要求读者再去看单独文档才能理解结论。

### 6.1 问题背景
最初的成员变更专项测试中，直接执行 `change_nodes` 失败，返回：
- `change membership in the pg 1.0 result: -144`

错误码解释：
- `-144 = RAFT_ERR_NOT_FOUND_PG`

关键现象：
- 新成员本地还没有目标 PG
- leader 已经开始向新成员发送 catch-up `append_entries`
- 新成员直接返回 `not find pg 1.0`

这说明当时成员变更主流程缺少一条关键联动：
- 在新成员上创建 PG
- 再进行 catch-up
- 再完成最终配置切换

### 6.2 修复点
针对成员变更这条链，本轮已经修掉以下问题：
- 在 raft 成员变更 catch-up 阶段，先对新成员自动执行 `create_pg`，再发送第一批 `append_entries`
- follower 收到配置变更 entry 后，立即更新本地成员视图，避免已被移出的旧成员继续对该 PG 参与选举
- monitor 侧修复 `ProcessPgMemberChangeFinish()` 的锁释放问题，避免成员变更完成后卡住后续 `GetClusterMapRequest`
- monitor 侧调整成员变更完成后的最终落盘逻辑，不再只依赖 `PgRemapped`；只要请求成功且最终成员集合与当前 `pgmap` 不一致，就按最终成员集合更新 `pgmap`
- OSD monitor client 增加 cluster-map 响应超时保护，避免 monitor 响应链路异常时长时间卡死

### 6.3 端到端验证场景
在 `4 OSD / 3 replica` 本地 dev cluster 中，选取 `PG 1.0` 做成员变更。

最终验证场景里：
- 变更前 `PG 1.0` 成员：`[1,4,2]`
- 目标成员集合：`[2,3,4]`

执行命令：
```bash
./build/src/test/raft_membership   --no-huge -s 1024   -m 0x10   -C .vstart/etc/fastblock/fastblock.json   -I 1   -o 10.211.55.29   -t 9818   -P 1   -G 0   -N change   -O '2:10.211.55.29:9087,3:10.211.55.29:9232,4:10.211.55.29:9965'
```

结果：
- `change membership in the pg 1.0 result: 0`
- monitor 收到 `PgMemberChangeFinishRequest, pg 1.0`
- 变更后再次执行 `getpgmap`，`PG 1.0` 变为：
  - `[2,4,3]`

说明：
- `[2,4,3]` 与目标成员集合 `[2,3,4]` 等价
- 说明 monitor 侧 `pgmap` 已正确收敛到新的成员集合
- 这次验证是在“新成员未预创建 PG”的前提下直接完成的

### 6.4 变更后系统状态与基础 IO 回归
成员变更完成后再次检查：
- `status` 仍显示 `4 osds: 4 up, 4 in`
- `8 active`
- `getpgmap` 仍可继续返回

随后继续执行基础写读校验：
```bash
BLOCK_BENCH_CPU_MASK=0x10 ./scripts/run-dev-write-read-verify.sh
```

结果：
- 写阶段完成 64 次 IO
- 读校验阶段完成 64 次 IO
- 脚本最终输出 `Write-read-verify completed`

结论：
- 成员变更完成后，集群基本状态保持正常
- monitor 不再出现之前那种响应卡住和误判 down 的问题
- 变更后基础 IO 仍可成功完成，说明支持面内的成员变更与基础数据面可以串起来

### 6.5 成员变更测试范围说明
当前系统级自测只把 `change_nodes` 视为对外支持、且需要验证的成员变更入口。

说明：
- `add_node` / `remove_node` 在代码层面存在，但不属于当前对外支持面的系统级能力。
- 因此系统级总报告不再把它们当作必须通过的测试项，也不以它们的行为定义对外口径。
- 当前系统级可验证、可对外表述的成员变更路径，应以 `change_nodes` 为准。

### 6.6 当前口径
当前可以明确说：
- raft 成员变更的基础 `change_nodes` 端到端路径已经验证通过
- 无预创建 PG 场景已打通
- monitor `pgmap` 最终成员落盘已验证
- 成员变更完成后基础 IO 回归已验证

但当前还不能说：
- 成员变更复杂场景已经全部签收

仍建议继续补测：
- 成员变更过程中的 leader 切换
- 成员变更后的长时间稳定性
- 与 snapshot/recovery 的联动场景

### 6.7 leader 切换中的成员变更结果
本轮新增补测了“`change_nodes` 过程中 leader 故障”场景，结果为失败。

测试环境：
- `4 OSD / 3 replica` 本地 RDMA dev cluster
- `PG 1.0` 初始成员为 `[1,4,3]`
- 日志确认 `osd3` 为 `PG 1.0` 当前 leader

测试动作：
- 向 leader `osd3` 发起 `change_nodes`，目标成员集合为 `[2,3,4]`
- `raft_membership` 日志显示：
  - `leader of the pg 1.0 is 3`
  - `change membership in the pg 1.0 result: 0`
- 在请求返回成功后约 `0.5s` 强制 `kill -9 osd3`

实际结果：
- monitor 收到 `PgMemberChangeFinishRequest, pg 1.0`
- `getpgmap` 显示 `PG 1.0` 的成员集合已切到 `[2,3,4]`
- 但集群没有收敛回健康状态，而是退化到：
  - `2 up / 3 in`
  - `PG 1.0` 长时间保持 `down`
- `getosdmap` 最终显示：
  - `osd2 down/out`
  - `osd3 down/out`
  - 仅 `osd1`、`osd4` 保持 `up/in`

额外证据：
- `raft_membership` 在 leader 被打掉后持续出现
  - `RDMA_CM_EVENT_DISCONNECTED`
  - `cant find the connection with dispatch id ...`
  最终超时退出
- monitor 日志可见：
  - `Received PgMemberChangeFinishRequest, pg 1.0`
  - 随后又出现新的 `LeaderBeElectedRequest, pg 1.0`
- 内核 `dmesg` 记录到 `osd2` 对应的 `reactor_1` 发生 segfault：
  - 崩溃地址再次落在 `google::protobuf::internal::TypeDefinedMapFieldBase<unsigned int, msg::ShardCore>::SpaceUsedExcludingSelfNoLockImpl(...)`

阶段结论：
- “基础 `change_nodes` 路径”已通过
- “leader 切换中的成员变更”当前未通过
- 这不是 monitor `pgmap` 未更新的问题，而是 leader 故障后系统稳定性问题，且伴随额外 OSD 崩溃

## 7. 当前可以使用的系统级结论口径
建议使用下面这组口径：
- fastblock 当前已经完成并验证了基础控制面、基础数据面、基础故障恢复、基础 raft 运行路径、基础 raft 成员变更路径、基础 raft recovery / snapshot 路径以及主要对外协议能力。
- 当前版本已经可以作为“开发阶段系统级可工作版本”进行汇报。
- 但若要给出“完整签收”或“发布级”结论，仍需补充 snapshot 复杂场景、成员变更复杂场景、长稳与升级回归。

不建议使用下面这类口径：
- “raft 全部完成”
- “分布式一致性能力全部签收”
- “系统已完成全量自测”

## 8. 后续补测优先级
建议按下面顺序继续补：

### P0
- 修复成员变更过程中 leader 切换失败问题
- leader 切换场景修复后的基础 IO 回归
- 修复 snapshot 过程中的 leader 切换未收敛问题
- 补测 snapshot 与成员变更联动

### P1
- 故障恢复与成员变更联动场景
- vhost / nvmf 在成员变更后的回归读写

### P2
- 长稳压测
- 升级 / 回滚
- 自动化回归矩阵

## 9. 最终判断
当前 fastblock 已经具备“系统级基础自测框架 + 多测试域证据 + 关键缺口明确”的状态。

换句话说：
- 现在已经不是“零散功能点证明”阶段
- 已经进入“系统级自测逐域收敛”阶段
- 下一步工作重点不是再证明某一个孤立功能点，而是按本报告的大纲把剩余关键测试域补齐

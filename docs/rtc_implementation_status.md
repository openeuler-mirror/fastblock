# RTC 实现状态（主路径交付）

对照 `docs/complete_rtc_design.md` Rev 3.2。

| PR | 状态 | 说明 |
|----|------|------|
| **PR-1** Client sticky home | **已完成** | `pg_home_shard.h` + `fb_client.h`；测试 `pg_home_shard_test` |
| **PR-2** local-first process+raft | **已完成** | `rtc_local_first.h`；`osd_service::process` + `raft_service` 全入口；测试 `rtc_local_first_test` |
| **PR-3** get_leader home-safe | **已完成** | RDMA `process_get_leader` hop 到 home；`resolve_pg_leader` raw 阻塞桥 |
| **PR-4** COW shard table + disable create_pg | **已完成** | `atomic<shared_ptr>` COW + publisher 串行；`OSD_ERR_CREATE_PG_DISABLED`；测试 `pg_shard_table_cow_test` |
| **PR-5** write ring | **延后** | 不主导；不 gate 完整 RTC |
| **PR-6** coreindex | **部分完成** | mon `create_pg` 解析 `coreindex`；client seed 优先 mon coreindex；完整 mon RO snapshot 可后续加强 |
| **PR-7** strict / proto | **未做** | 可选 |
| **PR-8** heartbeat mutex | **未做** | 可选 |
| **PR-9** kfastblock docs | **未做** | 可选 |
| **PR-10** 集成门槛 | **单元回归已覆盖** | 全量 fio hop_rate 需集群环境 |

## 回归测试

```bash
# 无需 SPDK 的纯逻辑回归
g++ -std=c++20 -I src/include -o /tmp/pg_home_shard_test src/test/pg_home_shard_test.cc && /tmp/pg_home_shard_test
g++ -std=c++20 -o /tmp/rtc_local_first_test src/test/rtc_local_first_test.cc && /tmp/rtc_local_first_test
g++ -std=c++20 -o /tmp/pg_shard_table_cow_test src/test/pg_shard_table_cow_test.cc && /tmp/pg_shard_table_cow_test

# cmake 构建后
ctest -R 'pg_home_shard|rtc_local_first|pg_shard_table_cow'
```

## 未覆盖的关键回归

- raft install snapshot 大幅落后 follower：必须自动化覆盖 `next_idx < log_get_base_index()` 后触发 `_recovery_by_snapshot()` 的场景。详细复现步骤、验收不变量和风险点见 [raft_recovery_and_snapshot.md](./raft_recovery_and_snapshot.md#3-关键回归测试缺口大幅落后-follower-触发-install-snapshot)。

## 环境变量

| 变量 | 含义 |
|------|------|
| `FASTBLOCK_RTC_LOCAL_PROCESS=0` | 关闭 local-first，强制 `invoke_on`（回滚 OSD 行为） |

## 主路径 RTC 验收口径

1. Client 数据端口 = sticky `home_shard` 对应 `sharded_ports`（map 刷新不漂移）。
2. 正确 home port 上 `process` / raft 同 SPDK 线程零 hop（local_hits 增加）。
3. get_leader 不在非 home 线程读 raft。
4. `_shard_table` 多核读无 data race（COW）。
5. `process_create_pg` 返回禁用错误。

# 1 raft recovery
因为leader处理客户端请求的速度可能没有客户端发起请求的速度快，为了加速leader处理，会合并处理客户端请求。一个客户端请求在raft中就是一条raft entry（raft_entry_t类），raft_log中有一个entry cache队列（里面其实是一个有序map）以raft entry的形式存储客户端的请求。leader会依次从这个entry cache队列中取出一批raft entry处理，这一批中的第一个entry的index用_first_idx标记，这一批中的最后一个entry的index用_current_idx标记，当这一批处理完后（_commit_idx等于_current_idx时），再处理下一批。
leader在处理raft entry时需要把raft entry发送给不处于recovery状态且不落后与leader（既node的next_idx等于_first_idx）的follower，落后于leader的raft entry需要通过recovery来从leader获取，这个recovery一般都是leader从entry cache中获取，会很快。

在raft_server_t::raft_process_appendentries_reply中处理follower发送的append entry response时，检查到follower落后于leader或正处于recovery但还没有结束recovery时，需要启动recovery，recovery是通过dispatch_recovery开始的，recovery的结束条件是此node的match idx等于_current_idx。
recovery又分三种情况：
* raft log中数据不足以恢复，通过snapshot恢复
```c++
    raft_index_t next_idx = node->raft_node_get_next_idx();
    //raft_get_log()->log_get_base_index()获取raft log中第一个entry的index
    if (next_idx < raft_get_log()->log_get_base_index()){
        _recovery_by_snapshot(node);
        return;
    }  
```
* 从entry cache中恢复
```c++
    //first_log_in_cache()获取entry cache中第一个entry的index
    auto first_idx_cache = raft_get_log()->first_log_in_cache();
    if(next_idx >= first_idx_cache){
        std::vector<std::shared_ptr<raft_entry_t>> entries;
        long entry_num = std::min(recovery_max_entry_num, raft_get_current_idx() - next_idx + 1);
        raft_get_log()->log_get_from_idx(next_idx, entry_num, entries);
        msg_appendentries_t*ae = create_appendentries(node);
        for(auto entry : entries){
            auto entry_ptr = ae->add_entries();
            *entry_ptr = *entry;
        }
        if(_client.send_appendentries(this, node->raft_node_get_id(), ae) != err::E_SUCCESS){
            node->raft_set_suppress_heartbeats(false);
        }
        return;
    }
```
* 从raft log的磁盘中恢复
```c++
    auto end_idx = std::min(raft_get_current_idx(), first_idx_cache - 1);
    long entry_num = std::min(recovery_max_entry_num, end_idx - next_idx + 1);

    raft_get_log()->disk_read(
      next_idx, 
      next_idx + entry_num - 1, 
      [this, node_id = node->raft_node_get_id(), send_entries = std::move(send_recovery_entries), next_idx]
      (std::vector<raft_entry_t>&& entries, int rberrno){
        assert(rberrno == 0);
        send_entries(std::move(entries));
      }); 
```

# 2 raft snapshot
raft log是存储在磁盘中的，raft log在不断增长，但磁盘空间毕竟有限，因此需要定期清理raft log，导致磁盘中的raft log是有限的，当磁盘中的raft log不足以recovery时，就需要通过raft snapshot来recovery。
生产数据面是 **AOS**（`osd_stm::attach_aos`）。Leader 建快照不再对每个 object 做 blob snapshot：`object_recovery::recovery_create` 调 `state_machine::pin_recovery_view` → `pg_store::create_raft_snapshot`（O(1) pin，`view_at(snap_index)` 读 head）。ckpt 后 versions 为空时只装 head（见 `docs/aos_unified_log_design.md`）。
通过快照recovery是在raft_server_t::_recovery_by_snapshot中进行，pin 期间用 `_snapshot_in_progress` 暂停 apply，pin 完成后恢复 apply，再走下面的 check/install 协议。
快照recovery步骤：
- **创建raft snapshot**
调用object_recovery::recovery_create：列出 live object 名并 pin recovery view。
raft有两个成员_snapshot_index和_snapshot_term，分别记录创建快照时状态机已经apply的最后一条entry的index和term。
- **检查对象快照**
取n个（默认为10）对象的快照信息发送给follower，follower收到后检测自己是否有这n个对象且对象信息与leader的一致（这里的一致是指对象内容一致），如果有且一致，则告诉leader自己已经有一致的对象，否则leader自己的对象不一致。
类snapshot_check_request封装快照检测请求，类snapshot_check_response封装快照检查结果
```c++ protobuf
message snapshot_check_request {
    //leader node id
    int32  node_id = 1;

    uint64 pool_id = 2;
    uint64 pg_id = 3;

    /** currentTerm, to force other leader/candidate to step down */
    int64 term = 4;
    repeated bytes object_names = 5;
}

message object_check_info {
    bytes obj_name = 1;
    bool exist = 2;
    bytes data_hash = 3;
}

message snapshot_check_response {
    //The node who sent us this message
    int32  node_id = 1;

    /** currentTerm, to force other leader/candidate to step down */
    int64 term = 2;

    /** lease expiration time */
    int64 lease = 3;
    int32 success = 4;

    repeated object_check_info objects = 5;
}
```
- **发送对象快照**
Leader收到follower的响应后，向follower发送信息不一致的对象的快照，follower收到后从快照中提取数据写入对应的对象中，然后给leader回复结果。
给follower发送对象快照是通过installsnapshot_request类和installsnapshot_response。
```c++ protobuf
message object_data_info {
    bytes obj_name = 1;
    bytes data = 2;
}

message installsnapshot_request {
    //leader node id
    int32  node_id = 1;
    uint64 pool_id = 2;
    uint64 pg_id = 3;

    /** currentTerm, to force other leader/candidate to step down */
    int64 term = 4;
    /** Index of the last entry represented by this snapshot */
    int64 last_idx = 5;
    /** Term of the last entry represented by this snapshot */
    int64 last_term = 6;
    
    /* true if this is the last chunk */
    bool  done = 7;
    repeated object_data_info objects = 8;
}

message installsnapshot_response {
    //The node who sent us this message
    int32  node_id = 1;

    /** currentTerm, to force other leader/candidate to step down */
    int64 term = 2;

    /** lease expiration time */
    int64 lease = 3;

    int32 success = 4;
}
```
leader给follower发送对象快照信息时，还需要发送创建快照时记录的_snapshot_index和_snapshot_term，且还需要有一个是否是此快照最后一批对象快照的标志，当follower收到最后一批对象快照信息并写入对象后，修改raft的_last_applied_idx（之前的都已经apply）为快照信息中的last_idx，修改raft的_current_idx分别为快照信息中的last_idx，修改raft的_commit_idx为快照信息中的last_idx，修改raft_log中的_next_idx为快照信息中的last_idx + 1。
follower收到leader发送来的快照信息时，需要确保等follower的_last_applied_idx等于_commit_idx（即当前log都已经apply）后才处理。
恢复完快照后，删除快照。

# 3 关键回归测试缺口：大幅落后 follower 触发 install snapshot

记录日期：2026-08-11。

当前代码已经实现 install snapshot 路径。大幅落后 follower 的触发条件在 `raft_server_t::do_recovery()`：

```c++
if (next_idx < raft_get_log()->log_get_base_index()){
    _recovery_by_snapshot(node);
    return;
}
```

这个 case 必须作为 raft 的关键回归场景单独覆盖。原因是普通 append entries recovery 只验证日志仍在 leader cache 或 disk log 中的情况，不能证明 follower 所需日志已经被 leader trim 后仍能恢复。

## 3.1 目标场景

建议自动化测试按下面步骤构造：

1. 启动 4 OSD、8 PG、replica 3 的开发集群。
2. 选定一个目标 PG，确认 leader 与 follower 分布。
3. 停掉一个 follower，或阻断该 follower 的 append entries。
4. 对目标 PG 持续写入，写入量要超过 raft log trim 阈值。
5. 等待 leader 的 disk log trim 生效，使该 follower 的 `next_idx` 小于 leader 的 `log_get_base_index()`。
6. 恢复 follower。
7. 验证 leader 自动进入 `_recovery_by_snapshot()`，并出现 `snapshot_check`、`installsnapshot_request`、`installsnapshot_response` 流程。
8. 验证 PG 最终恢复 active，业务 read-verify 通过，过程中无 assert、core dump、无限 recovery 或卡住。

## 3.2 验收不变量

- 大幅落后时必须自动触发 install snapshot，不能继续尝试读取已经 trim 的日志。
- follower 收到最后一批 snapshot 后，`last_applied_idx`、`current_idx`、`commit_idx` 必须推进到 snapshot 的 `last_idx`。
- follower 的 raft log next index 必须被设置为 `last_idx + 1`。
- leader 后续必须从 `snapshot_index + 1` 继续给该 follower 追加日志。
- snapshot recovery 结束后，leader 必须恢复 apply，清理临时 snapshot 资源。
- 数据校验必须通过，不能只验证 raft 状态。

## 3.3 当前风险点

- `_snapshot_index` 对应 term 的查询如果碰到日志 trim 边界，可能失败或得到不一致状态。
- follower 处理 `snapshot_check` 前要求 `commit_idx == last_applied_idx`，如果 apply 卡住会导致 snapshot recovery 卡住。
- snapshot object recovery 的 `_obr` 生命周期、snapshot cleanup 与 leader stop/restart 交织时仍缺少压力验证。
- 集群回归脚本：`scripts/run-raft-install-snapshot-regression.sh`（停 follower → 写超 trim 阈值 → 重启 → 等 `_recovery_by_snapshot` / `snapshot_check` / `installsnapshot` → read-verify）。
- trim 阈值：`FASTBLOCK_RAFT_LOG_TRIM_TRIGGER_ENTRIES` / `FASTBLOCK_RAFT_LOG_TRIM_KEEP_ENTRIES`（默认仍 1000/1000）。脚本用 128/64。
- 进程内单测：`disk_log_trim_test` 断言 apply 200 后 `next_idx=1 < base=137`。
- 本环境无 hugepage / RDMA rxe 时跑不了 4 OSD 集群；有盘的机器上 `FASTBLOCK_VSTART_SPDK_ARGS=--no-huge ./scripts/run-raft-install-snapshot-regression.sh`。

# 4 AOS install snapshot 路径

网络协议仍是 §2 的 `snapshot_check` / `installsnapshot_request`（按 object 名 + 4MiB chunk）。盘上实现：

| 角色 | 入口 | 行为 |
|------|------|------|
| Leader | `object_recovery` → `osd_stm::pin_recovery_view` / `read_recovery_object` | O(1) pin；按名 `view_at` 读 |
| Follower | `begin_install_snapshot` → `write_install_object` → `finish_install_snapshot` | `snapshot_ingest` staging epoch：begin → apply_chunk → seal → activate → `retarget_aos_log` |
| 收尾 | `set_index_after_snapshot` + 持久化 lapply | follower `last_applied` / `current` / `commit` 推到 `last_idx` |

不再走 blob `object_store::recovery_create`。旧 `aos_snapshot_session` 双路径草稿已废弃，以 STM 虚函数为准。

空 live set：`snapshot_check` 回 0 个 object 时 leader 直接发 `done` 的 `installsnapshot`，不再 `read_snapshot(count=0)` 卡住。错误路径必须 `unpin` 再释放 `_obr`，避免 pin 泄漏。

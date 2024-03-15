# 1 raft recoery
因为leader处理客户端请求的速度可能没有客户端发起请求的速度快，为了加速leader处理，会合并处理客户端请求。一个客户端请求在raft中就是一条raft entry（raft_entry_t类），raft_log中有一个entry cache队列（里面其实是一个有序map）以raft entry的形式存储客户端的请求。leader会依次从这个entry cache队列中取出一批raft entry处理，这一批中的第一个entry的index用_first_idx标记，这一批中的最后一个entry的index用_current_idx标记，当这一批处理完后（_commit_idx等于_current_idx时），再处理下一批。
leader在处理raft entry时需要把raft entry发送给不处于recovery状态且不落后与leader（既node的next_idx等于_first_idx）的follower，落后于leader的raft entry需要通过recovery来从leader获取，这个recovery一般都是leader从entry cache中获取，会很快。

在raft_server_t::raft_process_appendentries_reply中处理follower发送的append entry response时，检查到follower落后于leader或正处于recovery但还没有结束recovery时，需要启动recovery，recovery是通过dispatch_recovery开始的，recovery的结束条件是次node的match idx等于_current_idx。
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
        msg_appendentries_t*ae = create_appendentries(node.get());
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
数据是以一个个对象进行存储的，底层使用的是spdk blobstore，一个对象对应一个blob，raft snapshot底层使用的是spdk blob snapshot，对raft建快照时，对raft下所有的对象创建spdk blob snapshot。
通过快照recovery是在raft_server_t::_recovery_by_snapshot中进行，因需要对对象做快照，需要暂停raft状态机apply，通过参数_snapshot_in_progress控制，快照recovery完成后恢复raft状态机apply。
快照recovery步骤：
- **创建raft snapshot**
调用object_recovery::recovery_create对此raft下的所有对象创建快照。
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

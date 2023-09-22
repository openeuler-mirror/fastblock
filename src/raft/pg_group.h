#ifndef PG_GROUP_H_
#define PG_GROUP_H_

#include <map>
#include <memory>

#include "raft/raft.h"
#include "spdk/env.h"
#include "utils/utils.h"
#include "base/core_sharded.h"

std::string pg_id_to_name(uint64_t pool_id, uint64_t pg_id);

class shard_manager
{
public:
    struct node_heartbeat
    {
        node_heartbeat(
            raft_node_id_t t,
            heartbeat_request *req)
            : target(t), request(req) {}

        raft_node_id_t target;
        heartbeat_request *request;
    };

    shard_manager(uint32_t shard_id, pg_group_t *group)
        : _shard_id(shard_id), _group(group) {}

    void add_pg(std::string &name, std::shared_ptr<raft_server_t> pg)
    {
        _pgs[name] = pg;
    }

    void delete_pg(std::string &name)
    {
        _pgs.erase(std::move(name));
    }

    std::shared_ptr<raft_server_t> get_pg(std::string &name)
    {
        if (_pgs.find(name) == _pgs.end())
            return nullptr;
        return _pgs[name];
    }

    void start();

    void stop()
    {
        spdk_poller_unregister(&_heartbeat_timer);
    }

    uint32_t get_shard_id()
    {
        return _shard_id;
    }

    void dispatch_heartbeats();
    std::vector<node_heartbeat> get_heartbeat_requests();

private:
    uint32_t _shard_id; // cpu shard id
    pg_group_t *_group;

    // 记录此cpu核上的所有pg
    std::map<std::string, std::shared_ptr<raft_server_t>> _pgs;
    struct spdk_poller *_heartbeat_timer{};
};

class pg_group_t
{
public:
    pg_group_t(int current_node_id)
        : _shard_cores(get_shard_cores()), _current_node_id(current_node_id), _client(), _shard(core_sharded::get_core_sharded())
    {
        uint32_t i = 0;
        auto shard_num = _shard_cores.size();
        for (i = 0; i < shard_num; i++)
        {
            _shard_mg.push_back(shard_manager(i, this));
        }
    }

    void create_connect(int node_id, auto &&...args)
    {
        _client.create_connect(node_id, std::forward<decltype(args)>(args)...);
    }

    auto &get_raft_client_proto() noexcept
    {
        return _client;
    }

    void remove_connect(int node_id)
    {
        _client.remove_connect(node_id);
    }

    int create_pg(std::shared_ptr<state_machine> sm_ptr, uint32_t shard_id, uint64_t pool_id, uint64_t pg_id,
                  std::vector<osd_info_t> &&osds, disk_log *log);

    void delete_pg(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id);

    std::shared_ptr<raft_server_t> get_pg(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id)
    {
        auto name = pg_id_to_name(pool_id, pg_id);
        return _shard_mg[shard_id].get_pg(name);
    }

    std::shared_ptr<raft_server_t> get_pg(uint32_t shard_id, std::string &name)
    {
        return _shard_mg[shard_id].get_pg(name);
    }

    int get_current_node_id()
    {
        return _current_node_id;
    }

    void start(context *complete);

#ifdef KVSTORE
    void add_kvstore(kv_store *kv)
    {
        _kvs.push_back(kv);
    }
#endif

    void stop()
    {
#ifdef KVSTORE
        _kv->stop([](void *, int) {}, nullptr);
        delete _kv;
        _kv = nullptr;
#endif
        stop_shard_manager();
    }

    void start_shard_manager()
    {
        uint32_t i = 0;
        auto shard_num = _shard_mg.size();
        for (i = 0; i < shard_num; i++)
        {
            _shard.invoke_on(
                i,
                [this, shard_id = i]()
                {
                    _shard_mg[shard_id].start();
                });
        }
    }

    void stop_shard_manager()
    {
        uint32_t i = 0;
        auto shard_num = _shard_mg.size();
        for (i = 0; i < shard_num; i++)
        {
            _shard.invoke_on(
                i,
                [this, shard_id = i]()
                {
                    _shard_mg[shard_id].stop();
                });
        }
    }

private:
    int _pg_add(uint32_t shard_id, std::shared_ptr<raft_server_t> raft, uint64_t pool_id, uint64_t pg_id)
    {
        auto name = pg_id_to_name(pool_id, pg_id);
        _shard_mg[shard_id].add_pg(name, raft);
        return 0;
    }

    int _pg_remove(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id)
    {
        auto name = pg_id_to_name(pool_id, pg_id);
        auto raft = _shard_mg[shard_id].get_pg(name);
        raft->raft_destroy();
        _shard_mg[shard_id].delete_pg(name);
        return 0;
    }

    // 所有的pg按核区分保持在_core_mg中
    std::vector<uint32_t> _shard_cores;
    std::vector<shard_manager> _shard_mg;
    int _current_node_id;
    raft_client_protocol _client;
#ifdef KVSTORE
    std::vector<kv_store *> _kvs;
#endif
    core_sharded &_shard;
};

#endif
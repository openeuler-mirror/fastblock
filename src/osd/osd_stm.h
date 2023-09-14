#ifndef OSD_SM_H
#define OSD_SM_H
#include <string>

#include "raft/state_machine.h"
#include "localstore/object_store.h"
#include "rpc/osd_msg.pb.h"

enum class operation_type : uint16_t {
  NONE = 0,
  READ,
  WRITE,
  DELETE
};

// Allows READ-READ, or WRITE-WRITE.
// But exclusive on different op_type, like READ-WRITE or WRITE-READ.
// Thus, two or more WRITE can be replicated without blocking.
//       Two or more READ can be applied without blocking.
// But READ-WRITE-READ or WRITE-READ-WRITE will be applied sequencially.
// One op shouldn't unlock until everything done, and exception should be handled properly between lock/unlock.
template<typename op_type>
class op_type_excl_lock {
public:
    op_type_excl_lock() 
    : _lock_type(op_type::NONE)
    , _runners(0) { 
        _waiters.clear(); 
    };

    op_type_excl_lock(op_type_excl_lock&& o) = default;
    op_type_excl_lock& operator=(op_type_excl_lock&&) = default;

    // Move-only.
    op_type_excl_lock(const op_type_excl_lock&) = delete;
    void operator=(const op_type_excl_lock&) = delete;

    void lock(const op_type type, context *complete) {
        SPDK_DEBUGLOG(osd, "enter lock type %u\n", (uint32_t)type);
    
        if (try_lock(type)) {
            SPDK_DEBUGLOG(osd, "got lock type %u\n", (uint32_t)type);
            _runners++;
            complete->complete(0);
            return;
        }
    
        SPDK_DEBUGLOG(osd, "wait lock type %u\n", (uint32_t)type);
    
        // Wait.
        _waiters.emplace_back(type, complete);
    }

    void unlock(const op_type type) {
        SPDK_DEBUGLOG(osd, "enter unlock type %u\n", (uint32_t)type);
    
        _runners--;
        
        if (0 == _runners) {
            _lock_type = op_type::NONE;
        }
    
        // Wake waiters if any.
        wake();
    }

    // holders includes all the running and waiting workers.
    uint64_t holders() const {
        return _runners + _waiters.size();
    }

private:
    // TODO: range lock
    op_type   _lock_type;   // Current lock type. Each type is exclusive. NONE is compatible with either type.
    uint64_t  _runners;
  
    struct waiter {
        waiter(op_type _type, context *_complete) 
        : complete(_complete)
        , type(_type) {}

        context *complete;
        op_type type;
    };
  
    std::list<waiter> _waiters;   // FIFO. A list for all the (non-running) waiters.
  
  
    static inline bool is_none(const op_type type) {
        return type == op_type::NONE;
    }
  
    static inline bool is_compatible_type(const op_type type1, const op_type type2) {
        if (is_none(type1) || is_none(type2)) {
            return true;
        }
    
        return type1 == type2;
    }
  
    bool try_lock(const op_type type) noexcept {
        SPDK_DEBUGLOG(osd, "enter try_lock type %u, current type %u\n", (uint32_t)type, (uint32_t)_lock_type);
    
        // If running type is the same, and there is no waiters, just return true.
        if (is_none(_lock_type)   // The first try to get log.
                || (is_compatible_type(type, _lock_type) && _waiters.empty()) // Someone else got the lock, with the same op type.
                ) {
            if (is_none(_lock_type)) {
                // The first one, update _lock_type to our type.
                _lock_type = type;
            }
    
            return true;
        }
    
        return false;
    }
  
    void wake() {
        SPDK_DEBUGLOG(osd, "enter lock wake, current type %u\n", (uint32_t)_lock_type);
    
        // Try to wake front waiters.
        while (!_waiters.empty()) {
            auto& w = _waiters.front();
            
            if (try_lock(w.type)) {
                _runners++;
                w.complete->complete(0);
                _waiters.pop_front();
            } else {
                SPDK_DEBUGLOG(osd, "lock wake->break type %u, current type %u\n", (uint32_t)w.type, (uint32_t)_lock_type);
                break;
            }
        }
    
        SPDK_DEBUGLOG(osd, "leave lock wake, current type %u, runners %lu, _waiters size %lu\n", 
                (uint32_t)_lock_type, _runners, _waiters.size());
    }
};

template<typename lock_type>
class lock_manager {
public:
    lock_manager(const bool disable_flag = false)
    : _disable_flag(disable_flag) {
        SPDK_INFOLOG(osd, "lock_manager initialized to %d\n", _disable_flag);
    }
  
    void lock(const std::string& key, const operation_type type, context *complete){
        if (_disable_flag) {
            complete->complete(0);
            return;
        }
    
        SPDK_DEBUGLOG(osd, "lock object: %s, type: %u, all locks cnt before %lu\n", 
            key.c_str(), (uint32_t)type, _lock_map.size());
    
        // Add mutex for the object if not exist.
        if (!_lock_map.contains(key)) {
            _lock_map.emplace(key, std::make_unique<lock_type>());
        }
        auto iter = _lock_map.find(key);
        auto lock_ptr = iter->second.get();
        lock_ptr->lock(type, complete);            
    }

    auto unlock(const std::string& key, const operation_type type) {
        if (_disable_flag) {
            return;
        }
    
        SPDK_DEBUGLOG(osd, "unlock object: %s, type: %u, all locks cnt before %lu\n", 
            key.c_str(), (uint32_t)type, _lock_map.size());
    
        // Add mutex for the object if not exist.
        if (!_lock_map.contains(key)) {
            return;
        }
    
        auto it = _lock_map.find(key);
        auto lock_ptr = it->second.get();
        lock_ptr->unlock(type);
    
        // Remove mutex if no other waiter.
        auto iter = _lock_map.find(key);
        if (iter == _lock_map.end()) {
            SPDK_DEBUGLOG(osd, "no lock found! done with mutex, key: %s, all mutex cnt: %lu\n", 
                key.c_str(), _lock_map.size());
            return;
        }
    
        SPDK_DEBUGLOG(osd, "done with lock, object: %s, all mutex cnt: %lu, file waiters: %lu\n", 
            key.c_str(), _lock_map.size(), iter->second->holders());
    
        // Remove it from the list if no other tasks on it, otherwise the list will be too big.
        if (0 == iter->second->holders()) {
            _lock_map.erase(key);
    
            SPDK_DEBUGLOG(osd, "no waiter on %s, erase. mutex cnt now %lu\n", key.c_str(), _lock_map.size());
        }
    }

private:
    absl::flat_hash_map<std::string, std::unique_ptr<lock_type>> _lock_map;
    const bool _disable_flag;
};

class osd_stm : public state_machine {
public:
    osd_stm();

    void apply(std::shared_ptr<raft_entry_t> entry, context *complete) override;

    void write_obj(const std::string& obj_name, uint64_t offset, const std::string& data, context *complete);
    void delete_obj(const std::string& obj_name, context *complete);

    void write_and_wait(const osd::write_request* request, osd::write_reply* response, google::protobuf::Closure* done);
    void read_and_wait(const osd::read_request* request, osd::read_reply* response, google::protobuf::Closure* done);
    void delete_and_wait(const osd::delete_request* request, osd::delete_reply* response, google::protobuf::Closure* done);

    auto unlock(const std::string& key, const operation_type type){
        return _object_rw_lock.unlock(key, type);
    }
private:
    object_store _store;
    lock_manager<op_type_excl_lock<operation_type>>   _object_rw_lock;
};

#endif
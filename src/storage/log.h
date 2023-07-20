#ifndef LOG_H_
#define LOG_H_

#include "storage/pp_config.h"
#include "rpc/raft_msg.pb.h"
#include "utils/utils.h"

namespace storage {

typedef void (*log_reload_complete)(void *arg);

class log final{
public:
    class impl{
    public:
        impl(pp_config cfg) noexcept
        : _config(std::move(cfg)) {}

        virtual void reload(log* _log, log_reload_complete cb_fn) = 0;
        virtual void disk_append(std::vector<std::shared_ptr<raft_entry_t>>& entries, context* complete) = 0;
    private:
        pp_config _config;
    };

    log(std::shared_ptr<impl> ptr)
    : _impl(ptr) {}

    void reload(log* _log, log_reload_complete cb_fn){
        _impl->reload(_log, cb_fn);
    }

    void disk_append(std::vector<std::shared_ptr<raft_entry_t>>& entries, context* complete){
        _impl->disk_append(entries, complete);
    }
private:
    std::shared_ptr<impl> _impl;
};

class log_manager;
log make_disk_log(pp_config, log_manager&);
}

#endif
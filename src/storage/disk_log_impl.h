#ifndef DISK_LOG_IMPL_H_
#define DISK_LOG_IMPL_H_
#include <vector>
#include "storage/log_manager.h"

namespace storage {
class disk_log_impl final : public log::impl{
public:
    disk_log_impl(pp_config cfg, log_manager& manager)
    : log::impl(std::move(cfg))
    , _manager(manager) {}

    void reload(log* _log, log_reload_complete cb_fn) override;
    void disk_append(std::vector<std::shared_ptr<raft_entry_t>>& entries, context* complete) override;
private:
    log_manager& _manager;
};



}
#endif
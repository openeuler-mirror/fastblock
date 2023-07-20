#include "storage/disk_log_impl.h"

namespace storage {

log make_disk_log(pp_config cfg, log_manager& manager){
    auto ptr = std::make_shared<disk_log_impl>(std::move(cfg), manager);
    return log(ptr);
}

void disk_log_impl::reload(log* _log, log_reload_complete cb_fn){

    cb_fn((void *)_log);
}

void disk_log_impl::disk_append(std::vector<std::shared_ptr<raft_entry_t>>& entries, context* complete){
    (void)entries;
    complete->complete(0);
}
}
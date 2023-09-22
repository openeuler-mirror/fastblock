#include "global.h"

namespace global {
std::shared_ptr<::partition_manager> par_mgr;
std::unique_ptr<monitor::client> mon_client;
std::shared_ptr<::libblk_client> blk_client;
}

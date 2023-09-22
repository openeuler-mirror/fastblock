#pragma once

#include "client/libfblock.h"
#include "mon/client.h"
#include "osd/partition_manager.h"

namespace global {
extern std::shared_ptr<::partition_manager> par_mgr;
extern std::unique_ptr<monitor::client> mon_client;
extern std::shared_ptr<::libblk_client> blk_client;
}

#include "core_sharded.h"

std::vector<uint32_t> get_shard_cores(){
    std::vector<uint32_t> shard_cores;
    uint32_t lcore;

    SPDK_ENV_FOREACH_CORE(lcore){
        shard_cores.push_back(lcore);
    }
    return shard_cores;
}
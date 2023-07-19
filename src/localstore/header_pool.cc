#include "utils/itos.h"
#include "utils/units.h"
#include "spdk_buffer.h"

#include <string>
#include <spdk/env.h>
#include <spdk/log.h>

static thread_local struct spdk_mempool* tls_header_pool;

static constexpr uint32_t header_pool_size = 512;
static constexpr uint32_t header_mem_size = 4_KB;

struct spdk_mempool* header_pool() {
    return tls_header_pool;
}

void header_pool_init() {
    uint32_t lcore = spdk_env_get_current_core();
    uint32_t sockid = spdk_env_get_socket_id(lcore);
    std::string name = "header_mempool" + itos(lcore);
    tls_header_pool = spdk_mempool_create(name.c_str(),
				    header_pool_size,
				    header_mem_size,
				    header_pool_size / (2 * spdk_env_get_core_count()),
				    sockid);
    if (!tls_header_pool) {
		SPDK_ERRLOG("create header pool name:%s lcore:%u sockid:%u failed!\n", 
                    name.c_str(), lcore, sockid);
		return;
	}
}

void header_pool_fini() {
    if (spdk_mempool_count(tls_header_pool) != header_pool_size) {
        SPDK_ERRLOG("header buffer pool count is %zu but should be %u\n",
                spdk_mempool_count(tls_header_pool),
                header_pool_size);
    }
    spdk_mempool_free(tls_header_pool);
}

spdk_buffer header_pool_get() {
    char* c = (char*)spdk_mempool_get(tls_header_pool);
    return spdk_buffer(c, header_mem_size);
}

void header_pool_put(spdk_buffer& sbuf) {
    char* c = sbuf.get_buf();
    spdk_mempool_put(tls_header_pool, c);
}


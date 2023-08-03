#include "utils/itos.h"
#include "spdk_buffer.h"
#include "buffer_pool.h"

#include <string>
#include <spdk/env.h>
#include <spdk/log.h>

static struct spdk_mempool* tls_buffer_pool;

struct spdk_mempool* buffer_pool() {
    return tls_buffer_pool;
}

void buffer_pool_init() {
    uint32_t lcore = spdk_env_get_current_core();
    uint32_t sockid = spdk_env_get_socket_id(lcore);
    std::string name = "buffer_mempool_" + itos(lcore);
    tls_buffer_pool = spdk_mempool_create(name.c_str(),
				    buffer_pool_size,
				    buffer_size,
				    buffer_pool_size / (2 * spdk_env_get_core_count()),
				    sockid);
    SPDK_NOTICELOG("create buffer pool name:%s lcore:%u sockid:%u\n", 
                    name.c_str(), lcore, sockid);
    if (!tls_buffer_pool) {
		SPDK_ERRLOG("create buffer pool name:%s lcore:%u sockid:%u failed!\n", 
                    name.c_str(), lcore, sockid);
		return;
	}
}

void buffer_pool_fini() {
    if (spdk_mempool_count(tls_buffer_pool) != buffer_pool_size) {
        SPDK_ERRLOG("buffer bufferfer pool count is %zu but should be %u\n",
                spdk_mempool_count(tls_buffer_pool),
                buffer_pool_size);
    }
    spdk_mempool_free(tls_buffer_pool);
}

spdk_buffer buffer_pool_get() {
    char* c = (char*)spdk_mempool_get(tls_buffer_pool);
    return spdk_buffer(c, buffer_size);
}

void buffer_pool_put(spdk_buffer& sbuf) {
    char* c = sbuf.get_buf();
    spdk_mempool_put(tls_buffer_pool, c);
}


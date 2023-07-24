#pragma once

#include "utils/units.h"

#include <spdk/env.h>

/**
 * 现在内存都用的这个pool，都是4K的内存块。
 */
static constexpr uint32_t buffer_memory = 512_MB;
static constexpr uint32_t buffer_size = 4_KB;
static constexpr uint32_t buffer_pool_size = buffer_memory / buffer_size;

class spdk_buffer;

struct spdk_mempool* buffer_pool();

void buffer_pool_init();

void buffer_pool_fini();

spdk_buffer buffer_pool_get();

void buffer_pool_put(spdk_buffer& sbuf);
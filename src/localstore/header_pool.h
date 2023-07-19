#pragma once

#include "spdk_buffer.h"

#include <spdk/env.h>

struct spdk_mempool* header_pool();

void header_pool_init();

void header_pool_fini();


spdk_buffer header_pool_get();

void header_pool_put(spdk_buffer& sbuf);
#ifndef KFASTBLOCK_BUFFER_H
#define KFASTBLOCK_BUFFER_H

#include <linux/atomic.h>
#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>

/**
 * struct kfastblock_object_buffer_pool - Per-pool object buffer cache.
 * @lock:            Protects free_list and cached counter.
 * @free_list:       List of free cached buffers.
 * @cached:          Current number of cached buffers.
 * @max_cached:      Maximum number of buffers to cache.
 * @chunk_bytes:     Size of each buffer chunk in bytes.
 * @cache_hits:      Total cache hit count.
 * @cache_misses:    Total cache miss count.
 * @cache_returns:   Total buffers returned to pool.
 * @cache_evictions: Total buffers evicted from pool.
 * @direct_allocs:   Total direct allocations bypassing cache.
 */
struct kfastblock_object_buffer_pool {
	struct mutex lock;
	struct list_head free_list;
	u32 cached;
	u32 max_cached;
	u32 chunk_bytes;
	atomic64_t cache_hits;
	atomic64_t cache_misses;
	atomic64_t cache_returns;
	atomic64_t cache_evictions;
	atomic64_t direct_allocs;
};

/* Lifecycle */
void kfastblock_buffer_pool_init(struct kfastblock_object_buffer_pool *pool,
				 u32 chunk_bytes,
				 u32 max_cached);
void kfastblock_buffer_pool_set_limit(struct kfastblock_object_buffer_pool *pool,
				      u32 max_cached);
void kfastblock_buffer_pool_cleanup(struct kfastblock_object_buffer_pool *pool);

/* Allocation / deallocation */
void *kfastblock_buffer_pool_alloc(struct kfastblock_object_buffer_pool *pool,
				   size_t size,
				   gfp_t gfp,
				   bool zero);
void kfastblock_buffer_pool_free(struct kfastblock_object_buffer_pool *pool,
				 void *buf);

/* Statistics accessors */
u32 kfastblock_buffer_pool_cached(struct kfastblock_object_buffer_pool *pool);
u32 kfastblock_buffer_pool_limit(const struct kfastblock_object_buffer_pool *pool);
u32 kfastblock_buffer_pool_chunk_bytes(const struct kfastblock_object_buffer_pool *pool);
u64 kfastblock_buffer_pool_hits(const struct kfastblock_object_buffer_pool *pool);
u64 kfastblock_buffer_pool_misses(const struct kfastblock_object_buffer_pool *pool);
u64 kfastblock_buffer_pool_returns(const struct kfastblock_object_buffer_pool *pool);
u64 kfastblock_buffer_pool_evictions(const struct kfastblock_object_buffer_pool *pool);
u64 kfastblock_buffer_pool_direct_allocs(const struct kfastblock_object_buffer_pool *pool);

#endif

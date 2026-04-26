#include <linux/slab.h>
#include <linux/string.h>

#include "kfastblock/buffer.h"

struct kfastblock_object_buffer_entry {
	struct list_head node;
	size_t capacity;
	bool pooled;
};

static struct kfastblock_object_buffer_entry *
kfastblock_buffer_pool_to_entry(void *buf)
{
	if (!buf)
		return NULL;
	return ((struct kfastblock_object_buffer_entry *)buf) - 1;
}

static void *kfastblock_buffer_pool_direct_alloc(size_t size, gfp_t gfp, bool zero)
{
	if (!size)
		return NULL;
	if (zero)
		return kvzalloc(size, gfp);
	return kvmalloc(size, gfp);
}

static void kfastblock_buffer_pool_direct_free(void *buf)
{
	kvfree(buf);
}

void kfastblock_buffer_pool_init(struct kfastblock_object_buffer_pool *pool,
				 u32 chunk_bytes,
				 u32 max_cached)
{
	if (!pool)
		return;

	mutex_init(&pool->lock);
	INIT_LIST_HEAD(&pool->free_list);
	pool->cached = 0;
	pool->max_cached = max_cached ? max_cached : 1;
	pool->chunk_bytes = chunk_bytes;
	atomic64_set(&pool->cache_hits, 0);
	atomic64_set(&pool->cache_misses, 0);
	atomic64_set(&pool->cache_returns, 0);
	atomic64_set(&pool->cache_evictions, 0);
	atomic64_set(&pool->direct_allocs, 0);
}

void kfastblock_buffer_pool_set_limit(struct kfastblock_object_buffer_pool *pool,
				      u32 max_cached)
{
	if (!pool)
		return;

	mutex_lock(&pool->lock);
	pool->max_cached = max_cached ? max_cached : 1;
	while (pool->cached > pool->max_cached &&
	       !list_empty(&pool->free_list)) {
		struct kfastblock_object_buffer_entry *entry =
			list_first_entry(&pool->free_list,
					 struct kfastblock_object_buffer_entry,
					 node);

		list_del(&entry->node);
		pool->cached--;
		atomic64_inc(&pool->cache_evictions);
		kfastblock_buffer_pool_direct_free(entry);
	}
	mutex_unlock(&pool->lock);
}

void kfastblock_buffer_pool_cleanup(struct kfastblock_object_buffer_pool *pool)
{
	if (!pool)
		return;

	mutex_lock(&pool->lock);
	while (!list_empty(&pool->free_list)) {
		struct kfastblock_object_buffer_entry *entry =
			list_first_entry(&pool->free_list,
					 struct kfastblock_object_buffer_entry,
					 node);

		list_del(&entry->node);
		pool->cached--;
		kfastblock_buffer_pool_direct_free(entry);
	}
	mutex_unlock(&pool->lock);
}

void *kfastblock_buffer_pool_alloc(struct kfastblock_object_buffer_pool *pool,
				   size_t size,
				   gfp_t gfp,
				   bool zero)
{
	struct kfastblock_object_buffer_entry *entry = NULL;
	size_t alloc_size = size;
	bool pooled = false;

	if (!size)
		return NULL;
	if (pool && pool->chunk_bytes && size <= pool->chunk_bytes) {
		mutex_lock(&pool->lock);
		if (!list_empty(&pool->free_list)) {
			entry = list_first_entry(&pool->free_list,
						 struct kfastblock_object_buffer_entry,
						 node);
			list_del(&entry->node);
			pool->cached--;
		}
		mutex_unlock(&pool->lock);
		if (entry) {
			if (zero)
				memset(entry + 1, 0, size);
			atomic64_inc(&pool->cache_hits);
			return entry + 1;
		}
		alloc_size = pool->chunk_bytes;
		pooled = true;
		atomic64_inc(&pool->cache_misses);
	} else if (pool) {
		atomic64_inc(&pool->direct_allocs);
	}

	entry = kfastblock_buffer_pool_direct_alloc(sizeof(*entry) + alloc_size,
						    gfp,
						    zero);
	if (!entry)
		return NULL;

	INIT_LIST_HEAD(&entry->node);
	entry->capacity = alloc_size;
	entry->pooled = pooled;
	return entry + 1;
}

void kfastblock_buffer_pool_free(struct kfastblock_object_buffer_pool *pool,
				 void *buf)
{
	struct kfastblock_object_buffer_entry *entry;

	if (!buf)
		return;

	entry = kfastblock_buffer_pool_to_entry(buf);
	if (!pool || !entry->pooled || entry->capacity != pool->chunk_bytes) {
		kfastblock_buffer_pool_direct_free(entry);
		return;
	}

	mutex_lock(&pool->lock);
	if (pool->cached >= pool->max_cached) {
		mutex_unlock(&pool->lock);
		atomic64_inc(&pool->cache_evictions);
		kfastblock_buffer_pool_direct_free(entry);
		return;
	}
	list_add(&entry->node, &pool->free_list);
	pool->cached++;
	mutex_unlock(&pool->lock);
	atomic64_inc(&pool->cache_returns);
}

u32 kfastblock_buffer_pool_cached(struct kfastblock_object_buffer_pool *pool)
{
	u32 cached = 0;

	if (!pool)
		return 0;

	mutex_lock(&pool->lock);
	cached = pool->cached;
	mutex_unlock(&pool->lock);
	return cached;
}

u32 kfastblock_buffer_pool_limit(const struct kfastblock_object_buffer_pool *pool)
{
	if (!pool)
		return 0;
	return pool->max_cached;
}

u32 kfastblock_buffer_pool_chunk_bytes(const struct kfastblock_object_buffer_pool *pool)
{
	if (!pool)
		return 0;
	return pool->chunk_bytes;
}

u64 kfastblock_buffer_pool_hits(const struct kfastblock_object_buffer_pool *pool)
{
	if (!pool)
		return 0;
	return atomic64_read(&pool->cache_hits);
}

u64 kfastblock_buffer_pool_misses(const struct kfastblock_object_buffer_pool *pool)
{
	if (!pool)
		return 0;
	return atomic64_read(&pool->cache_misses);
}

u64 kfastblock_buffer_pool_returns(const struct kfastblock_object_buffer_pool *pool)
{
	if (!pool)
		return 0;
	return atomic64_read(&pool->cache_returns);
}

u64 kfastblock_buffer_pool_evictions(const struct kfastblock_object_buffer_pool *pool)
{
	if (!pool)
		return 0;
	return atomic64_read(&pool->cache_evictions);
}

u64 kfastblock_buffer_pool_direct_allocs(const struct kfastblock_object_buffer_pool *pool)
{
	if (!pool)
		return 0;
	return atomic64_read(&pool->direct_allocs);
}

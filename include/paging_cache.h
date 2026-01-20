#pragma once

#include <stddef.h>
#include <stdbool.h>

// Simple in-memory paging cache.
// Keyed by an arbitrary UTF-8 key string (caller-provided).
// Stores an owned byte buffer plus total_bytes and paging metadata.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	char *data;        // owned; NUL-terminated for convenience
	size_t len;        // bytes excluding NUL
	size_t total_bytes;
	bool truncated;
	bool has_next_start;
	size_t next_start;
} aicli_paging_cache_value_t;

typedef struct aicli_paging_cache aicli_paging_cache_t;

// Creates a cache of up to max_entries. If max_entries==0, defaults to 64.
aicli_paging_cache_t *aicli_paging_cache_create(size_t max_entries);

void aicli_paging_cache_destroy(aicli_paging_cache_t *c);

// Returns true and fills out_value with pointers owned by cache (do not free)
// if key is found. Returns false if not found.
bool aicli_paging_cache_get(const aicli_paging_cache_t *c, const char *key,
                           aicli_paging_cache_value_t *out_value);

// Stores a copy of value in cache (deep-copies bytes). May evict LRU.
// Returns true on success.
bool aicli_paging_cache_put(aicli_paging_cache_t *c, const char *key,
                           const aicli_paging_cache_value_t *value);

#ifdef __cplusplus
}
#endif

#include "paging_cache.h"

#include <stdlib.h>
#include <string.h>

typedef struct aicli_paging_cache_entry {
	char *key;
	aicli_paging_cache_value_t v;
	struct aicli_paging_cache_entry *prev;
	struct aicli_paging_cache_entry *next;
} aicli_paging_cache_entry_t;

struct aicli_paging_cache {
	size_t max_entries;
	size_t entry_count;
	aicli_paging_cache_entry_t *head; // MRU
	aicli_paging_cache_entry_t *tail; // LRU
};

static void entry_free(aicli_paging_cache_entry_t *e)
{
	if (!e)
		return;
	free(e->key);
	free(e->v.data);
	free(e);
}

static void detach(aicli_paging_cache_t *c, aicli_paging_cache_entry_t *e)
{
	if (!c || !e)
		return;
	if (e->prev)
		e->prev->next = e->next;
	else
		c->head = e->next;
	if (e->next)
		e->next->prev = e->prev;
	else
		c->tail = e->prev;
	e->prev = NULL;
	e->next = NULL;
}

static void attach_front(aicli_paging_cache_t *c, aicli_paging_cache_entry_t *e)
{
	if (!c || !e)
		return;
	e->prev = NULL;
	e->next = c->head;
	if (c->head)
		c->head->prev = e;
	c->head = e;
	if (!c->tail)
		c->tail = e;
}

static aicli_paging_cache_entry_t *find_entry(aicli_paging_cache_t *c, const char *key)
{
	if (!c || !key)
		return NULL;
	for (aicli_paging_cache_entry_t *e = c->head; e; e = e->next) {
		if (e->key && strcmp(e->key, key) == 0)
			return e;
	}
	return NULL;
}

aicli_paging_cache_t *aicli_paging_cache_create(size_t max_entries)
{
	if (max_entries == 0)
		max_entries = 64;
	aicli_paging_cache_t *c = (aicli_paging_cache_t *)calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->max_entries = max_entries;
	return c;
}

void aicli_paging_cache_destroy(aicli_paging_cache_t *c)
{
	if (!c)
		return;
	aicli_paging_cache_entry_t *e = c->head;
	while (e) {
		aicli_paging_cache_entry_t *n = e->next;
		entry_free(e);
		e = n;
	}
	free(c);
}

bool aicli_paging_cache_get(const aicli_paging_cache_t *c0, const char *key,
			   aicli_paging_cache_value_t *out_value)
{
	if (out_value)
		memset(out_value, 0, sizeof(*out_value));
	if (!c0 || !key || !key[0])
		return false;
	// Cast away const to update LRU order.
	aicli_paging_cache_t *c = (aicli_paging_cache_t *)c0;
	aicli_paging_cache_entry_t *e = find_entry(c, key);
	if (!e)
		return false;
	// Move to front.
	detach(c, e);
	attach_front(c, e);
	if (out_value)
		*out_value = e->v;
	return true;
}

static bool value_deep_copy(const aicli_paging_cache_value_t *src, aicli_paging_cache_value_t *dst)
{
	if (!dst)
		return false;
	memset(dst, 0, sizeof(*dst));
	if (!src)
		return true;
	if (src->data && src->len) {
		char *p = (char *)malloc(src->len + 1);
		if (!p)
			return false;
		memcpy(p, src->data, src->len);
		p[src->len] = '\0';
		dst->data = p;
		dst->len = src->len;
	}
	dst->total_bytes = src->total_bytes;
	dst->truncated = src->truncated;
	dst->has_next_start = src->has_next_start;
	dst->next_start = src->next_start;
	return true;
}

bool aicli_paging_cache_put(aicli_paging_cache_t *c, const char *key,
			   const aicli_paging_cache_value_t *value)
{
	if (!c || !key || !key[0])
		return false;

	aicli_paging_cache_entry_t *e = find_entry(c, key);
	if (e) {
		// Update existing.
	free(e->v.data);
	if (!value_deep_copy(value, &e->v)) {
		memset(&e->v, 0, sizeof(e->v));
		return false;
	}
		detach(c, e);
		attach_front(c, e);
		return true;
	}

	// Evict if needed.
	while (c->entry_count >= c->max_entries && c->tail) {
		aicli_paging_cache_entry_t *victim = c->tail;
		detach(c, victim);
		entry_free(victim);
		c->entry_count--;
	}

	e = (aicli_paging_cache_entry_t *)calloc(1, sizeof(*e));
	if (!e)
		return false;
	e->key = strdup(key);
	if (!e->key) {
		entry_free(e);
		return false;
	}
	if (!value_deep_copy(value, &e->v)) {
		entry_free(e);
		return false;
	}
	attach_front(c, e);
	c->entry_count++;
	return true;
}

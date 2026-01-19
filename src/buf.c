#include "buf.h"

#include <stdlib.h>
#include <string.h>

static bool ensure_cap(aicli_buf_t *b, size_t need) {
	if (need <= b->cap) return true;
	size_t new_cap = b->cap ? b->cap : 256;
	while (new_cap < need) {
		new_cap *= 2;
		if (new_cap < b->cap) return false;
	}
	char *p = (char *)realloc(b->data, new_cap);
	if (!p) return false;
	b->data = p;
	b->cap = new_cap;
	return true;
}

bool aicli_buf_init(aicli_buf_t *b, size_t initial_cap) {
	if (!b) return false;
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
	if (initial_cap == 0) initial_cap = 256;
	b->data = (char *)malloc(initial_cap);
	if (!b->data) return false;
	b->cap = initial_cap;
	return true;
}

void aicli_buf_free(aicli_buf_t *b) {
	if (!b) return;
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

bool aicli_buf_append(aicli_buf_t *b, const void *data, size_t n) {
	if (!b || (!data && n > 0)) return false;
	if (!ensure_cap(b, b->len + n)) return false;
	memcpy(b->data + b->len, data, n);
	b->len += n;
	return true;
}

bool aicli_buf_append_str(aicli_buf_t *b, const char *s) {
	if (!s) return true;
	return aicli_buf_append(b, s, strlen(s));
}

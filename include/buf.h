#pragma once

#include <stddef.h>
#include <stdbool.h>

typedef struct {
	char *data;
	size_t len;
	size_t cap;
} aicli_buf_t;

bool aicli_buf_init(aicli_buf_t *b, size_t initial_cap);
void aicli_buf_free(aicli_buf_t *b);
bool aicli_buf_append(aicli_buf_t *b, const void *data, size_t n);
bool aicli_buf_append_str(aicli_buf_t *b, const char *s);

#pragma once

#include <stddef.h>

typedef struct {
	int http_status;
	char *body;
	size_t body_len;
	char error[256];
} aicli_brave_response_t;

// Performs a Brave Web Search request.
//
// Returns 0 on successful HTTP request (even if status != 200).
// On transport/setup errors, returns non-zero and sets out->error.
int aicli_brave_web_search(const char *api_key, const char *query, int count,
			   const char *lang, const char *freshness,
			   aicli_brave_response_t *out);

void aicli_brave_response_free(aicli_brave_response_t *res);

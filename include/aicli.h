#pragma once

#include <stddef.h>
#include <stdbool.h>

#define AICLI_MAX_TOOL_BYTES 4096

typedef enum {
	AICLI_SEARCH_PROVIDER_GOOGLE_CSE = 0,
	AICLI_SEARCH_PROVIDER_BRAVE = 1,
} aicli_search_provider_t;

typedef struct {
	const char *openai_api_key;
	const char *openai_base_url;
	const char *model;
	aicli_search_provider_t search_provider;

	// Google Programmable Search Engine / Custom Search JSON API
	const char *google_api_key;
	const char *google_cse_cx;

	// Brave Web Search API
	const char *brave_api_key;
} aicli_config_t;

typedef struct {
	const char *path;
	const char *name;
	size_t size_bytes;
} aicli_allowed_file_t;

typedef struct {
	const char *stdout_text;
	size_t stdout_len;
	const char *stderr_text;
	int exit_code;
	size_t total_bytes;
	bool truncated;
	bool cache_hit;
	bool has_next_start;
	size_t next_start;
} aicli_tool_result_t;

typedef struct {
	const char *id;
	const char *command;
	const char *file; // optional hint
	const char *idempotency; // optional
	size_t start;
	size_t size;
} aicli_execute_request_t;

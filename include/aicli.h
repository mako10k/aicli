#pragma once

#include <stddef.h>
#include <stdbool.h>

#define AICLI_MAX_TOOL_BYTES 4096

typedef struct {
	const char *openai_api_key;
	const char *openai_base_url;
	const char *model;
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

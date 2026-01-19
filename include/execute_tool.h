#pragma once

#include <stddef.h>
#include "aicli.h"
#include "execute_dsl.h"

typedef struct {
	aicli_allowed_file_t *files;
	int file_count;
} aicli_allowlist_t;

// Normalizes a path with realpath(). Caller must free.
char *aicli_realpath_dup(const char *path);

// Gets file size in bytes (stat). Returns true on success.
bool aicli_get_file_size(const char *path, size_t *out_size);

// Executes restricted pipeline and returns paged stdout.
int aicli_execute_run(const aicli_allowlist_t *allow,
				 const aicli_execute_request_t *req,
				 aicli_tool_result_t *out);

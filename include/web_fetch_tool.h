#pragma once

#include <stddef.h>

#include "aicli.h"
#include "web_tools.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const char *url;
	// optional: comma-separated or repeated prefixes are handled by caller; tool just receives array
	const char **allowed_prefixes;
	size_t allowed_prefix_count;
	size_t start;
	size_t size;
	size_t max_body_bytes;
	long timeout_seconds;
	long connect_timeout_seconds;
	int max_redirects;
	const char *idempotency;
} aicli_web_fetch_tool_request_t;

int aicli_web_fetch_tool_run(const aicli_config_t *cfg,
                            aicli_paging_cache_t *cache,
                            const aicli_web_fetch_tool_request_t *req,
                            aicli_tool_result_t *out);

#ifdef __cplusplus
}
#endif

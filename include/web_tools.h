#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "aicli.h"
#include "paging_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	AICLI_WEB_PROVIDER_AUTO = 0,
	AICLI_WEB_PROVIDER_GOOGLE_CSE = 1,
	AICLI_WEB_PROVIDER_BRAVE = 2,
} aicli_web_provider_t;

typedef struct {
	aicli_web_provider_t provider; // auto uses cfg->search_provider
	const char *query;
	int count;                 // desired number of results (provider-specific caps)
	const char *lang;          // optional (brave) or locale/Google lr derived by caller if desired
	const char *freshness;     // optional (brave)
	bool raw_json;             // if true, prefer raw JSON output
	// Formatting knobs (for formatted mode)
	size_t max_title;
	size_t max_url;
	size_t max_snippet;
	size_t width;
	// Paging for returned output
	size_t start;
	size_t size;
	// Optional cache key component
	const char *idempotency;
} aicli_web_search_request_t;

typedef struct {
	aicli_tool_result_t tool; // stdout/stderr/exit_code/total_bytes/truncated/next_start/cache_hit
} aicli_web_search_result_t;

int aicli_web_search_run(const aicli_config_t *cfg,
                         aicli_paging_cache_t *cache,
                         const aicli_web_search_request_t *req,
                         aicli_web_search_result_t *out);


typedef struct {
	const char *url;
	// Allowed URL prefixes (read-only allowlist). If NULL/empty, fetch is forbidden.
	const char **allowed_prefixes;
	size_t allowed_prefix_count;
	// Limits
	size_t max_body_bytes; // hard cap on fetched bytes
	long timeout_seconds;
	long connect_timeout_seconds;
	int max_redirects;
	// Paging for returned output
	size_t start;
	size_t size;
	// Optional cache key component
	const char *idempotency;
} aicli_web_fetch_request_t;

typedef struct {
	aicli_tool_result_t tool;
	int http_status;
	const char *content_type; // optional, allocated into stderr_text if needed
} aicli_web_fetch_result_t;

int aicli_web_fetch_run(const aicli_config_t *cfg,
                        aicli_paging_cache_t *cache,
                        const aicli_web_fetch_request_t *req,
                        aicli_web_fetch_result_t *out);

#ifdef __cplusplus
}
#endif

#pragma once

#include "aicli.h"
#include "web_tools.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	aicli_web_provider_t provider;
	const char *query;
	int count;
	const char *lang;
	const char *freshness;
	bool raw;
	size_t start;
	size_t size;
	const char *idempotency;
} aicli_web_search_tool_request_t;

// Runs web search as a tool and returns paged bytes in out->tool.
int aicli_web_search_tool_run(const aicli_config_t *cfg,
                             aicli_paging_cache_t *cache,
                             const aicli_web_search_tool_request_t *req,
                             aicli_tool_result_t *out);

#ifdef __cplusplus
}
#endif

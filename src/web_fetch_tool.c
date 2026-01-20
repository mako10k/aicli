#include "web_fetch_tool.h"

#include <stdlib.h>
#include <string.h>

int aicli_web_fetch_tool_run(const aicli_config_t *cfg,
                            aicli_paging_cache_t *cache,
                            const aicli_web_fetch_tool_request_t *req,
                            aicli_tool_result_t *out)
{
	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));
	if (!req || !req->url || !req->url[0]) {
		out->stderr_text = "invalid_request";
		out->exit_code = 2;
		return 0;
	}

	aicli_web_fetch_request_t r = {0};
	r.url = req->url;
	r.allowed_prefixes = req->allowed_prefixes;
	r.allowed_prefix_count = req->allowed_prefix_count;
	r.max_body_bytes = req->max_body_bytes ? req->max_body_bytes : (1024 * 1024);
	r.timeout_seconds = req->timeout_seconds ? req->timeout_seconds : 15L;
	r.connect_timeout_seconds = req->connect_timeout_seconds ? req->connect_timeout_seconds : 10L;
	r.max_redirects = req->max_redirects;
	r.start = req->start;
	r.size = req->size;
	r.idempotency = req->idempotency;

	aicli_web_fetch_result_t res;
	int rc = aicli_web_fetch_run(cfg, cache, &r, &res);
	if (rc != 0)
		return rc;

	// Free optional allocations from lower layers.
	// Note: tool strings (stdout/stderr) are passed through to the caller;
	// the caller owns freeing them.
	if (res.content_type)
		free((void *)res.content_type);

	*out = res.tool;
	// avoid accidental reuse
	res.tool.stderr_text = NULL;
	res.tool.stdout_text = NULL;
	return 0;
}

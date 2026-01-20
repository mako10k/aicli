#include "web_search_tool.h"

#include <string.h>

int aicli_web_search_tool_run(const aicli_config_t *cfg,
                             aicli_paging_cache_t *cache,
                             const aicli_web_search_tool_request_t *req,
                             aicli_tool_result_t *out)
{
	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));
	if (!cfg || !req || !req->query || !req->query[0]) {
		out->stderr_text = "invalid_request";
		out->exit_code = 2;
		return 0;
	}

	aicli_web_search_request_t r = {0};
	r.provider = req->provider;
	r.query = req->query;
	r.count = req->count > 0 ? req->count : 5;
	r.lang = req->lang;
	r.freshness = req->freshness;
	r.raw_json = req->raw;
	r.max_title = 160;
	r.max_url = 500;
	r.max_snippet = 500;
	r.width = 80;
	r.start = req->start;
	r.size = req->size;
	r.idempotency = req->idempotency;

	aicli_web_search_result_t res;
	int rc = aicli_web_search_run(cfg, cache, &r, &res);
	if (rc != 0)
		return rc;
	*out = res.tool;
	return 0;
}

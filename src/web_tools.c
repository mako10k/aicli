#include "web_tools.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "brave_search.h"
#include "buf.h"
#include "google_search.h"

static const char *safe_str(const char *s) { return s ? s : ""; }

static void tool_init(aicli_tool_result_t *t)
{
	if (!t)
		return;
	memset(t, 0, sizeof(*t));
}

static bool str_starts_with(const char *s, const char *prefix)
{
	if (!s || !prefix)
		return false;
	size_t n = strlen(prefix);
	return strncmp(s, prefix, n) == 0;
}

static bool url_is_allowed(const aicli_web_fetch_request_t *req)
{
	if (!req || !req->url || !req->url[0])
		return false;
	if (!req->allowed_prefixes || req->allowed_prefix_count == 0)
		return false;
	for (size_t i = 0; i < req->allowed_prefix_count; i++) {
		const char *p = req->allowed_prefixes[i];
		if (p && p[0] && str_starts_with(req->url, p))
			return true;
	}
	return false;
}

static char *dup_cstr(const char *s)
{
	if (!s)
		return NULL;
	size_t n = strlen(s);
	char *p = (char *)malloc(n + 1);
	if (!p)
		return NULL;
	memcpy(p, s, n + 1);
	return p;
}

static char *dup_suggest_prefix_from_url(const char *url)
{
	if (!url || !url[0])
		return NULL;
	const char *p = strstr(url, "://");
	if (!p)
		return NULL;
	const char *scheme = url;
	size_t scheme_len = (size_t)(p - url);
	if (scheme_len == 0 || scheme_len > 16)
		return NULL;
	p += 3;
	// Reject userinfo if present (avoid suggesting credentials-bearing prefixes).
	const char *at = strchr(p, '@');
	const char *slash = strchr(p, '/');
	if (at && (!slash || at < slash))
		return NULL;
	// Host ends at /, ?, #, or end.
	const char *end = p;
	while (*end && *end != '/' && *end != '?' && *end != '#')
		end++;
	if (end == p)
		return NULL;
	// Drop port if present.
	const char *host_end = end;
	for (const char *q = p; q < end; q++) {
		if (*q == ':') {
			host_end = q;
			break;
		}
	}
	if (host_end == p)
		return NULL;

	size_t host_len = (size_t)(host_end - p);
	if (host_len == 0 || host_len > 255)
		return NULL;

	// Build "scheme://host/"
	size_t out_len = scheme_len + 3 + host_len + 1;
	char *out = (char *)malloc(out_len + 1);
	if (!out)
		return NULL;
	memcpy(out, scheme, scheme_len);
	memcpy(out + scheme_len, "://", 3);
	memcpy(out + scheme_len + 3, p, host_len);
	out[out_len - 1] = '/';
	out[out_len] = '\0';
	return out;
}

static bool debug_allowlist_enabled(void)
{
	const char *v = getenv("AICLI_DEBUG_WEB_FETCH_ALLOWLIST");
	return v && v[0] && strcmp(v, "0") != 0;
}

static char *dup_url_not_allowed_debug(const aicli_web_fetch_request_t *req)
{
	const char *u = (req && req->url) ? req->url : NULL;
	char *suggest = dup_suggest_prefix_from_url(u);
	char hint[512];
	if (suggest && suggest[0]) (void)snprintf(hint, sizeof(hint),
	                                         "Try: export AICLI_WEB_FETCH_PREFIXES='%s,https://example.com/,https://docs.example.com/'",
	                                         suggest);
	else (void)snprintf(hint, sizeof(hint),
		            "Try: export AICLI_WEB_FETCH_PREFIXES='https://example.com/,https://docs.example.com/'");

	if (!req)
	{
		char msg[900];
		(void)snprintf(msg, sizeof(msg),
		              "url_not_allowed: URL does not match AICLI_WEB_FETCH_PREFIXES. %s. "
		              "Hint for tool-using models: call cli_help(topic=\"web fetch\") to show the exact CLI/env help text.",
		              hint);
		free(suggest);
		return dup_cstr(msg);
	}
	if (!debug_allowlist_enabled())
	{
		char msg[900];
		(void)snprintf(msg, sizeof(msg),
		              "url_not_allowed: URL does not match AICLI_WEB_FETCH_PREFIXES. %s. "
		              "Hint for tool-using models: call cli_help(topic=\"web fetch\") to show the exact CLI/env help text.",
		              hint);
		free(suggest);
		return dup_cstr(msg);
	}

	aicli_buf_t b;
	if (!aicli_buf_init(&b, 256))
	{
		char msg[900];
		(void)snprintf(msg, sizeof(msg),
		              "url_not_allowed: URL does not match AICLI_WEB_FETCH_PREFIXES. %s. "
		              "Hint for tool-using models: call cli_help(topic=\"web fetch\") to show the exact CLI/env help text.",
		              hint);
		free(suggest);
		return dup_cstr(msg);
	}
	bool ok = true;
	ok = ok && aicli_buf_append_str(&b, "url_not_allowed: URL does not match AICLI_WEB_FETCH_PREFIXES; allowed_prefixes=");
	ok = ok && aicli_buf_append_str(&b, "[");
	size_t shown = 0;
	for (size_t i = 0; i < req->allowed_prefix_count; i++) {
		const char *p = req->allowed_prefixes[i];
		if (!p || !p[0])
			continue;
		if (shown >= 8)
			break;
		if (shown > 0)
			ok = ok && aicli_buf_append_str(&b, ", ");
		ok = ok && aicli_buf_append_str(&b, "\"");
		ok = ok && aicli_buf_append_str(&b, p);
		ok = ok && aicli_buf_append_str(&b, "\"");
		shown++;
	}
	if (req->allowed_prefix_count > shown)
		ok = ok && aicli_buf_append_str(&b, ", ...");
	ok = ok && aicli_buf_append_str(&b, "]");
	ok = ok && aicli_buf_append(&b, "\0", 1);
	if (!ok) {
		aicli_buf_free(&b);
		char fallback[900];
		(void)snprintf(fallback, sizeof(fallback),
		              "url_not_allowed: URL does not match AICLI_WEB_FETCH_PREFIXES. %s. "
		              "Hint for tool-using models: call cli_help(topic=\"web fetch\") to show the exact CLI/env help text.",
		              hint);
		free(suggest);
		return dup_cstr(fallback);
	}
	free(suggest);
	return b.data;
}

static char *make_cache_key2(const char *prefix,
                            const char *idem,
                            const char *a,
                            const char *b,
                            size_t start,
                            size_t size)
{
	char tmp[64];
	snprintf(tmp, sizeof(tmp), "%zu:%zu", start, size);

	aicli_buf_t buf;
	if (!aicli_buf_init(&buf, 256))
		return NULL;
	bool ok = true;
	ok = ok && aicli_buf_append_str(&buf, prefix);
	ok = ok && aicli_buf_append_str(&buf, "|");
	ok = ok && aicli_buf_append_str(&buf, safe_str(idem));
	ok = ok && aicli_buf_append_str(&buf, "|");
	ok = ok && aicli_buf_append_str(&buf, safe_str(a));
	ok = ok && aicli_buf_append_str(&buf, "|");
	ok = ok && aicli_buf_append_str(&buf, safe_str(b));
	ok = ok && aicli_buf_append_str(&buf, "|");
	ok = ok && aicli_buf_append_str(&buf, tmp);
	ok = ok && aicli_buf_append(&buf, "\0", 1);
	if (!ok) {
		aicli_buf_free(&buf);
		return NULL;
	}
	return buf.data;
}

static void apply_paging_from_owned_bytes(const char *data, size_t total, size_t start, size_t size,
                                         aicli_tool_result_t *out)
{
	// Mirror execute paging behavior.
	if (!out)
		return;
	if (start > total)
		start = total;
	size_t remain = total - start;
	size_t n = (remain < size) ? remain : size;

	char *buf = (char *)malloc(n + 1);
	if (!buf) {
		out->stderr_text = "oom";
		out->exit_code = 1;
		return;
	}
	if (n)
		memcpy(buf, data + start, n);
	buf[n] = '\0';

	out->stdout_text = buf;
	out->stdout_len = n;
	out->exit_code = 0;
	out->total_bytes = total;
	out->truncated = (start + n) < total;
	out->has_next_start = out->truncated;
	out->next_start = start + n;
}

int aicli_web_search_run(const aicli_config_t *cfg,
                         aicli_paging_cache_t *cache,
                         const aicli_web_search_request_t *req,
                         aicli_web_search_result_t *out)
{
	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));
	tool_init(&out->tool);
	if (!cfg || !req || !req->query || !req->query[0]) {
		out->tool.stderr_text = "invalid_request";
		out->tool.exit_code = 2;
		return 0;
	}

	size_t size = req->size ? req->size : AICLI_MAX_TOOL_BYTES;
	if (size > AICLI_MAX_TOOL_BYTES)
		size = AICLI_MAX_TOOL_BYTES;

	int provider = (int)req->provider;
	if (provider == (int)AICLI_WEB_PROVIDER_AUTO)
		provider = (cfg->search_provider == AICLI_SEARCH_PROVIDER_BRAVE)
		               ? (int)AICLI_WEB_PROVIDER_BRAVE
		               : (int)AICLI_WEB_PROVIDER_GOOGLE_CSE;

	char provbuf[32];
	snprintf(provbuf, sizeof(provbuf), "prov_%d", provider);
	char cntbuf[32];
	snprintf(cntbuf, sizeof(cntbuf), "count_%d", req->count);

	char *key = make_cache_key2("web_search", req->idempotency, provbuf, req->query, req->start, size);
	if (key && cache) {
		aicli_paging_cache_value_t cv;
		if (aicli_paging_cache_get(cache, key, &cv)) {
			out->tool.cache_hit = true;
			apply_paging_from_owned_bytes(cv.data, cv.total_bytes, req->start, size, &out->tool);
			free(key);
			return 0;
		}
	}

	// Build an output string (either formatted summary or raw JSON)
	char *full = NULL;
	size_t full_len = 0;

	if (provider == (int)AICLI_WEB_PROVIDER_GOOGLE_CSE) {
		if (!cfg->google_api_key || !cfg->google_api_key[0] || !cfg->google_cse_cx || !cfg->google_cse_cx[0]) {
			out->tool.stderr_text =
			    "google_cse is not configured. Set GOOGLE_API_KEY and GOOGLE_CSE_CX, or use AICLI_SEARCH_PROVIDER=brave with BRAVE_API_KEY. "
			    "Hint for tool-using models: call cli_help(topic=\"web search\") to show the exact CLI/env help text.";
			out->tool.exit_code = 2;
			free(key);
			return 0;
		}
		aicli_google_response_t res;
		int rc = aicli_google_cse_search(cfg->google_api_key, cfg->google_cse_cx,
		                                 req->query, req->count, NULL, &res);
		if (rc != 0) {
			if (res.error[0]) {
				out->tool.stderr_text = dup_cstr(res.error);
			} else {
				out->tool.stderr_text = "google_cse search failed: check GOOGLE_API_KEY/GOOGLE_CSE_CX";
			}
			out->tool.exit_code = 2;
			aicli_google_response_free(&res);
			free(key);
			return 0;
		}
		if (res.http_status != 200 || !res.body) {
			out->tool.stderr_text = "google_http_error";
			out->tool.exit_code = 1;
			aicli_google_response_free(&res);
			free(key);
			return 0;
		}

		// For tools, default to returning raw JSON (compact) since formatting in CLI is best-effort.
		// Keep it simple and deterministic.
		full = (char *)malloc(res.body_len + 1);
		if (!full) {
			out->tool.stderr_text = "oom";
			out->tool.exit_code = 1;
			aicli_google_response_free(&res);
			free(key);
			return 0;
		}
		memcpy(full, res.body, res.body_len);
		full[res.body_len] = '\0';
		full_len = res.body_len;
		aicli_google_response_free(&res);
	} else if (provider == (int)AICLI_WEB_PROVIDER_BRAVE) {
		if (!cfg->brave_api_key || !cfg->brave_api_key[0]) {
			out->tool.stderr_text =
			    "brave is not configured. Set BRAVE_API_KEY (and optionally AICLI_SEARCH_PROVIDER=brave). "
			    "Hint for tool-using models: call cli_help(topic=\"web search\") to show the exact CLI/env help text.";
			out->tool.exit_code = 2;
			free(key);
			return 0;
		}
		aicli_brave_response_t res;
		int rc = aicli_brave_web_search(cfg->brave_api_key, req->query, req->count,
		                               req->lang, req->freshness, &res);
		if (rc != 0) {
			if (res.error[0]) {
				out->tool.stderr_text = dup_cstr(res.error);
			} else {
				out->tool.stderr_text = "brave search failed: check BRAVE_API_KEY";
			}
			out->tool.exit_code = 2;
			aicli_brave_response_free(&res);
			free(key);
			return 0;
		}
		if (res.http_status != 200 || !res.body) {
			out->tool.stderr_text = "brave_http_error";
			out->tool.exit_code = 1;
			aicli_brave_response_free(&res);
			free(key);
			return 0;
		}
		full = (char *)malloc(res.body_len + 1);
		if (!full) {
			out->tool.stderr_text = "oom";
			out->tool.exit_code = 1;
			aicli_brave_response_free(&res);
			free(key);
			return 0;
		}
		memcpy(full, res.body, res.body_len);
		full[res.body_len] = '\0';
		full_len = res.body_len;
		aicli_brave_response_free(&res);
	} else {
		out->tool.stderr_text = "unknown_provider";
		out->tool.exit_code = 2;
		free(key);
		return 0;
	}

	apply_paging_from_owned_bytes(full, full_len, req->start, size, &out->tool);

	if (cache && key) {
		aicli_paging_cache_value_t v = {
		    .data = full,
		    .len = full_len,
		    .total_bytes = full_len,
		    .truncated = (req->start + size) < full_len,
		    .has_next_start = (req->start + size) < full_len,
		    .next_start = req->start + size,
		};
		(void)aicli_paging_cache_put(cache, key, &v);
	}
	free(key);
	// Note: tool.stdout_text is its own allocation, so we can free full now.
	free(full);
	return 0;
}

typedef struct {
	aicli_buf_t b;
	size_t max_bytes;
	bool too_large;
} fetch_buf_t;

static size_t fetch_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	fetch_buf_t *fb = (fetch_buf_t *)userdata;
	size_t n = size * nmemb;
	if (!fb || n == 0)
		return n;
	if (fb->b.len + n > fb->max_bytes) {
		fb->too_large = true;
		return 0; // abort
	}
	if (!aicli_buf_append(&fb->b, ptr, n)) {
		return 0;
	}
	return n;
}

int aicli_web_fetch_run(const aicli_config_t *cfg,
                        aicli_paging_cache_t *cache,
                        const aicli_web_fetch_request_t *req,
                        aicli_web_fetch_result_t *out)
{
	(void)cfg;
	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));
	tool_init(&out->tool);
	if (!req || !req->url || !req->url[0]) {
		out->tool.stderr_text = "invalid_request";
		out->tool.exit_code = 2;
		return 0;
	}
	if (!req->allowed_prefixes || req->allowed_prefix_count == 0) {
		out->tool.stderr_text =
		    "web_fetch disabled. Set AICLI_WEB_FETCH_PREFIXES to allow URL prefixes. "
		    "Hint for tool-using models: call cli_help(topic=\"web fetch\") to show the exact CLI/env help text.";
		out->tool.exit_code = 3;
		return 0;
	}
	if (!url_is_allowed(req)) {
		out->tool.stderr_text = dup_url_not_allowed_debug(req);
		out->tool.exit_code = 3;
		return 0;
	}

	size_t size = req->size ? req->size : AICLI_MAX_TOOL_BYTES;
	if (size > AICLI_MAX_TOOL_BYTES)
		size = AICLI_MAX_TOOL_BYTES;

	char *key = make_cache_key2("web_fetch", req->idempotency, req->url, "", req->start, size);
	if (key && cache) {
		aicli_paging_cache_value_t cv;
		if (aicli_paging_cache_get(cache, key, &cv)) {
			out->tool.cache_hit = true;
			apply_paging_from_owned_bytes(cv.data, cv.total_bytes, req->start, size, &out->tool);
			free(key);
			return 0;
		}
	}

	CURL *curl = curl_easy_init();
	if (!curl) {
		out->tool.stderr_text = "curl_easy_init_failed";
		out->tool.exit_code = 2;
		free(key);
		return 0;
	}

	fetch_buf_t fb;
	memset(&fb, 0, sizeof(fb));
	fb.max_bytes = req->max_body_bytes ? req->max_body_bytes : (1024 * 1024);
	if (!aicli_buf_init(&fb.b, 8192)) {
		curl_easy_cleanup(curl);
		out->tool.stderr_text = "oom";
		out->tool.exit_code = 1;
		free(key);
		return 0;
	}

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml,application/json,text/plain,*/*");

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_URL, req->url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fb);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "aicli/0.0.0");
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, req->timeout_seconds ? req->timeout_seconds : 15L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, req->connect_timeout_seconds ? req->connect_timeout_seconds : 10L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, (req->max_redirects > 0) ? 1L : 0L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)((req->max_redirects > 0) ? req->max_redirects : 0));

	CURLcode cc = curl_easy_perform(curl);
	if (cc != CURLE_OK) {
		out->tool.stderr_text = dup_cstr(curl_easy_strerror(cc));
		out->tool.exit_code = 2;
		goto done;
	}

	{
		long status = 0;
		(void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		out->http_status = (int)status;
	}
	{
		char *ct = NULL;
		(void)curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
		out->content_type = ct ? dup_cstr(ct) : NULL;
	}

	if (fb.too_large) {
		out->tool.stderr_text = "body_too_large";
		out->tool.exit_code = 4;
		goto done;
	}

	// NUL-terminate buffer
	(void)aicli_buf_append(&fb.b, "\0", 1);
	char *full = fb.b.data;
	size_t full_len = fb.b.len - 1;
	fb.b.data = NULL;
	fb.b.len = 0;
	fb.b.cap = 0;

	apply_paging_from_owned_bytes(full, full_len, req->start, size, &out->tool);

	if (cache && key) {
		aicli_paging_cache_value_t v = {
		    .data = full,
		    .len = full_len,
		    .total_bytes = full_len,
		    .truncated = (req->start + size) < full_len,
		    .has_next_start = (req->start + size) < full_len,
		    .next_start = req->start + size,
		};
		(void)aicli_paging_cache_put(cache, key, &v);
	}
	free(full);

done:
	free(key);
	if (headers)
		curl_slist_free_all(headers);
	aicli_buf_free(&fb.b);
	curl_easy_cleanup(curl);
	return 0;
}

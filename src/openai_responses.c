#define _XOPEN_SOURCE 700

#include "openai_responses.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ctype.h>
#include <time.h>

#include <yyjson.h>

typedef struct {
	char *data;
	size_t len;
	size_t cap;
} mem_buf_t;

typedef struct {
	int retry_after_seconds; // -1 if missing/unknown
} resp_headers_t;

static void mem_buf_free(mem_buf_t *b)
{
	if (!b)
		return;
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

static int mem_buf_reserve(mem_buf_t *b, size_t want)
{
	if (want <= b->cap)
		return 1;
	size_t new_cap = b->cap ? b->cap : 4096;
	while (new_cap < want) {
		new_cap *= 2;
		if (new_cap > (size_t)(32 * 1024 * 1024))
			return 0;
	}
	char *p = realloc(b->data, new_cap);
	if (!p)
		return 0;
	b->data = p;
	b->cap = new_cap;
	return 1;
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	mem_buf_t *b = (mem_buf_t *)userdata;
	size_t n = size * nmemb;
	if (n == 0)
		return 0;
	if (!mem_buf_reserve(b, b->len + n + 1))
		return 0;
	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	b->data[b->len] = '\0';
	return n;
}

static int parse_retry_after_seconds(const char *value)
{
	if (!value)
		return -1;
	while (*value && isspace((unsigned char)*value))
		value++;
	// Try delta-seconds first.
	char *end = NULL;
	unsigned long sec = strtoul(value, &end, 10);
	if (end && end != value) {
		while (*end && isspace((unsigned char)*end))
			end++;
		if (*end == '\0') {
			if (sec > 3600)
				sec = 3600;
			return (int)sec;
		}
	}
	// HTTP-date form exists in the spec, but most APIs use delta-seconds.
	// Keep best-effort parsing strict here (unknown -> -1).
	return -1;
}

static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	resp_headers_t *h = (resp_headers_t *)userdata;
	size_t n = size * nmemb;
	if (!h || n == 0)
		return n;

	// Header line is not NUL-terminated.
	// We only care about "Retry-After:".
	const char *line = ptr;
	size_t len = n;
	// Trim CRLF
	while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
		len--;
	if (len == 0)
		return n;

	static const char k[] = "Retry-After:";
	size_t klen = sizeof(k) - 1;
	if (len < klen)
		return n;

	int match = 1;
	for (size_t i = 0; i < klen; i++) {
		if (tolower((unsigned char)line[i]) != tolower((unsigned char)k[i])) {
			match = 0;
			break;
		}
	}
	if (!match)
		return n;

	// Copy value to temp buffer for parsing.
	const char *value = line + klen;
	while (*value && isspace((unsigned char)*value))
		value++;
	char tmp[128];
	size_t vlen = (size_t)(line + len - value);
	if (vlen >= sizeof(tmp))
		vlen = sizeof(tmp) - 1;
	memcpy(tmp, value, vlen);
	tmp[vlen] = '\0';

	int sec = parse_retry_after_seconds(tmp);
	if (sec >= 0)
		h->retry_after_seconds = sec;

	return n;
}

static void sleep_seconds(double seconds)
{
	if (seconds <= 0)
		return;
	struct timespec ts;
	ts.tv_sec = (time_t)seconds;
	ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1000000000.0);
	if (ts.tv_nsec < 0)
		ts.tv_nsec = 0;
	(void)nanosleep(&ts, NULL);
}

static double backoff_seconds(unsigned attempt)
{
	// Exponential backoff with a small deterministic jitter.
	// attempt: 0,1,2,...
	static const double cap = 30.0;
	double base = 0.5;
	for (unsigned i = 0; i < attempt && base < cap; i++)
		base *= 2.0;
	if (base > cap)
		base = cap;
	// Jitter in [0.0, 0.25]
	unsigned j = (unsigned)((time(NULL) ^ (attempt * 1103515245u)) & 0xffu);
	double jitter = ((double)j / 255.0) * 0.25;
	return base + jitter;
}

static void set_err(char err[256], const char *msg)
{
	if (!err)
		return;
	if (!msg)
		msg = "unknown error";
	snprintf(err, 256, "%s", msg);
}

static char *join_url_path(const char *base_url, const char *path)
{
	if (!base_url || !base_url[0] || !path)
		return NULL;
	// Normalize: avoid double slashes.
	size_t blen = strlen(base_url);
	int base_ends = (blen > 0 && base_url[blen - 1] == '/');
	int path_starts = (path[0] == '/');

	size_t need = blen + strlen(path) + 2;
	char *out = malloc(need);
	if (!out)
		return NULL;
	if (base_ends && path_starts) {
		snprintf(out, need, "%.*s%s", (int)(blen - 1), base_url, path);
		return out;
	}
	if (!base_ends && !path_starts) {
		snprintf(out, need, "%s/%s", base_url, path);
		return out;
	}
	snprintf(out, need, "%s%s", base_url, path);
	return out;
}

static char *build_request_json(const aicli_openai_request_t *req,
			      const char *tools_json, const char *tool_choice)
{
	if (!req || !req->model || !req->model[0] || !req->input_text)
		return NULL;

	yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
	yyjson_mut_val *root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);

	yyjson_mut_obj_add_str(doc, root, "model", req->model);

	// input: [{role:"system",content:[{type:"input_text",text:"..."}]}, {role:"user",...}]
	yyjson_mut_val *input = yyjson_mut_arr(doc);
	yyjson_mut_obj_add_val(doc, root, "input", input);

	if (req->system_text && req->system_text[0]) {
		yyjson_mut_val *m = yyjson_mut_obj(doc);
		yyjson_mut_arr_add_val(input, m);
		yyjson_mut_obj_add_str(doc, m, "role", "system");
		yyjson_mut_val *content = yyjson_mut_arr(doc);
		yyjson_mut_obj_add_val(doc, m, "content", content);
		yyjson_mut_val *c0 = yyjson_mut_obj(doc);
		yyjson_mut_arr_add_val(content, c0);
		yyjson_mut_obj_add_str(doc, c0, "type", "input_text");
		yyjson_mut_obj_add_str(doc, c0, "text", req->system_text);
	}

	{
		yyjson_mut_val *m = yyjson_mut_obj(doc);
		yyjson_mut_arr_add_val(input, m);
		yyjson_mut_obj_add_str(doc, m, "role", "user");
		yyjson_mut_val *content = yyjson_mut_arr(doc);
		yyjson_mut_obj_add_val(doc, m, "content", content);
		yyjson_mut_val *c0 = yyjson_mut_obj(doc);
		yyjson_mut_arr_add_val(content, c0);
		yyjson_mut_obj_add_str(doc, c0, "type", "input_text");
		yyjson_mut_obj_add_str(doc, c0, "text", req->input_text);
	}

	if (tools_json && tools_json[0]) {
		yyjson_doc *tools_doc = yyjson_read(tools_json, strlen(tools_json), 0);
		if (tools_doc) {
			yyjson_val *tools_root = yyjson_doc_get_root(tools_doc);
			yyjson_mut_val *tools_mut = yyjson_val_mut_copy(doc, tools_root);
			if (tools_mut)
				yyjson_mut_obj_add_val(doc, root, "tools", tools_mut);
			yyjson_doc_free(tools_doc);
		}
	}

	if (tool_choice && tool_choice[0])
		yyjson_mut_obj_add_str(doc, root, "tool_choice", tool_choice);

	char *json = yyjson_mut_write(doc, 0, NULL);
	yyjson_mut_doc_free(doc);
	return json;
}

int aicli_openai_responses_post(const char *api_key, const char *base_url,
			      const aicli_openai_request_t *req,
			      const char *tools_json, const char *tool_choice,
			      aicli_openai_http_response_t *out)
{
	if (!out)
		return 2;
	memset(out, 0, sizeof(*out));
	out->retry_after_seconds = -1;

	if (!api_key || !api_key[0]) {
		set_err(out->error, "OPENAI_API_KEY is not set");
		return 2;
	}
	if (!base_url || !base_url[0])
		base_url = "https://api.openai.com/v1";

	char *url = join_url_path(base_url, "/responses");
	if (!url) {
		set_err(out->error, "failed to build url");
		return 2;
	}

	char *payload = build_request_json(req, tools_json, tool_choice);
	if (!payload) {
		free(url);
		set_err(out->error, "failed to build request json");
		return 2;
	}

	int rc = 0;

	// Retry strategy:
	// - 429: honor Retry-After if present, else backoff
	// - 503: backoff a few times
	// Other HTTP statuses are returned to caller without retry.
	const unsigned max_attempts = 4; // total attempts including first
	for (unsigned attempt = 0; attempt < max_attempts; attempt++) {
		CURL *curl = curl_easy_init();
		if (!curl) {
			set_err(out->error, "curl_easy_init failed");
			rc = 2;
			break;
		}

		mem_buf_t buf = {0};
		resp_headers_t rh = {.retry_after_seconds = -1};

		struct curl_slist *headers = NULL;
		char auth[512];
		snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
		headers = curl_slist_append(headers, auth);
		headers = curl_slist_append(headers, "Content-Type: application/json");
		headers = curl_slist_append(headers, "Accept: application/json");

		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &rh);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "aicli/0.0.0");
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

		CURLcode cc = curl_easy_perform(curl);
		if (cc != CURLE_OK) {
			set_err(out->error, curl_easy_strerror(cc));
			rc = 2;
			mem_buf_free(&buf);
			if (headers)
				curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
			break;
		}

		long status = 0;
		(void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		out->http_status = (int)status;
		out->retry_after_seconds = rh.retry_after_seconds;

		// Success or non-retryable status: move body to out and return.
		if (out->http_status != 429 && out->http_status != 503)
		{
			out->body = buf.data;
			out->body_len = buf.len;
			buf.data = NULL;
			buf.len = 0;
			buf.cap = 0;
			mem_buf_free(&buf);
			if (headers)
				curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
			rc = 0;
			break;
		}

		// Retryable status.
		int last = (attempt + 1 >= max_attempts);
		if (last) {
			out->body = buf.data;
			out->body_len = buf.len;
			buf.data = NULL;
			buf.len = 0;
			buf.cap = 0;
			mem_buf_free(&buf);
			if (headers)
				curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
			rc = 0;
			break;
		}

		// Drop current body before retry.
		mem_buf_free(&buf);
		if (headers)
			curl_slist_free_all(headers);
		curl_easy_cleanup(curl);

		double wait_s = backoff_seconds(attempt);
		if (out->http_status == 429 && out->retry_after_seconds >= 0)
			wait_s = (double)out->retry_after_seconds;
		sleep_seconds(wait_s);
		// continue
	}

	free(payload);
	free(url);
	return rc;
}

int aicli_openai_responses_post_raw_json(const char *api_key, const char *base_url,
				     const char *json_payload,
				     aicli_openai_http_response_t *out)
{
	if (!out)
		return 2;
	memset(out, 0, sizeof(*out));
	out->retry_after_seconds = -1;

	if (!api_key || !api_key[0]) {
		set_err(out->error, "OPENAI_API_KEY is not set");
		return 2;
	}
	if (!base_url || !base_url[0])
		base_url = "https://api.openai.com/v1";
	if (!json_payload || !json_payload[0]) {
		set_err(out->error, "missing json_payload");
		return 2;
	}

	char *url = join_url_path(base_url, "/responses");
	if (!url) {
		set_err(out->error, "failed to build url");
		return 2;
	}

	int rc = 0;

	const unsigned max_attempts = 4;
	for (unsigned attempt = 0; attempt < max_attempts; attempt++) {
		CURL *curl = curl_easy_init();
		if (!curl) {
			set_err(out->error, "curl_easy_init failed");
			rc = 2;
			break;
		}

		mem_buf_t buf = {0};
		resp_headers_t rh = {.retry_after_seconds = -1};

		struct curl_slist *headers = NULL;
		char auth[512];
		snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
		headers = curl_slist_append(headers, auth);
		headers = curl_slist_append(headers, "Content-Type: application/json");
		headers = curl_slist_append(headers, "Accept: application/json");

		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_payload));
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &rh);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "aicli/0.0.0");
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

		CURLcode cc = curl_easy_perform(curl);
		if (cc != CURLE_OK) {
			set_err(out->error, curl_easy_strerror(cc));
			rc = 2;
			mem_buf_free(&buf);
			if (headers)
				curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
			break;
		}

		long status = 0;
		(void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		out->http_status = (int)status;
		out->retry_after_seconds = rh.retry_after_seconds;

		if (out->http_status != 429 && out->http_status != 503) {
			out->body = buf.data;
			out->body_len = buf.len;
			buf.data = NULL;
			buf.len = 0;
			buf.cap = 0;
			mem_buf_free(&buf);
			if (headers)
				curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
			rc = 0;
			break;
		}

		int last = (attempt + 1 >= max_attempts);
		if (last) {
			out->body = buf.data;
			out->body_len = buf.len;
			buf.data = NULL;
			buf.len = 0;
			buf.cap = 0;
			mem_buf_free(&buf);
			if (headers)
				curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
			rc = 0;
			break;
		}

		mem_buf_free(&buf);
		if (headers)
			curl_slist_free_all(headers);
		curl_easy_cleanup(curl);

		double wait_s = backoff_seconds(attempt);
		if (out->http_status == 429 && out->retry_after_seconds >= 0)
			wait_s = (double)out->retry_after_seconds;
		sleep_seconds(wait_s);
	}

	free(url);
	return rc;
}

void aicli_openai_http_response_free(aicli_openai_http_response_t *res)
{
	if (!res)
		return;
	free(res->body);
	res->body = NULL;
	res->body_len = 0;
}

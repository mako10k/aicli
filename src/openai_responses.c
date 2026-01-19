#include "openai_responses.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

typedef struct {
	char *data;
	size_t len;
	size_t cap;
} mem_buf_t;

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

	CURL *curl = curl_easy_init();
	if (!curl) {
		free(payload);
		free(url);
		set_err(out->error, "curl_easy_init failed");
		return 2;
	}

	int rc = 0;
	mem_buf_t buf = {0};

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
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "aicli/0.0.0");
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

	CURLcode cc = curl_easy_perform(curl);
	if (cc != CURLE_OK) {
		set_err(out->error, curl_easy_strerror(cc));
		rc = 2;
		goto done;
	}

	long status = 0;
	(void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	out->http_status = (int)status;
	out->body = buf.data;
	out->body_len = buf.len;
	buf.data = NULL;
	buf.len = 0;
	buf.cap = 0;

done:
	mem_buf_free(&buf);
	if (headers)
		curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(payload);
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

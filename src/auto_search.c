#include "auto_search.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "openai_responses.h"

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

static void trim_in_place(char *s)
{
	if (!s)
		return;
	char *start = s;
	while (*start && isspace((unsigned char)*start))
		start++;
	if (start != s)
		memmove(s, start, strlen(start) + 1);
	size_t len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1])) {
		s[len - 1] = '\0';
		len--;
	}
}

static char *extract_output_text(const char *body, size_t body_len)
{
	if (!body || body_len == 0)
		return NULL;
	yyjson_doc *doc = yyjson_read(body, body_len, 0);
	if (!doc)
		return NULL;
	yyjson_val *root = yyjson_doc_get_root(doc);
	if (!root || !yyjson_is_obj(root)) {
		yyjson_doc_free(doc);
		return NULL;
	}
	yyjson_val *out = yyjson_obj_get(root, "output");
	if (!out || !yyjson_is_arr(out)) {
		yyjson_doc_free(doc);
		return NULL;
	}

	char *ret = NULL;
	size_t max = yyjson_arr_size(out);
	for (size_t i = 0; i < max; i++) {
		yyjson_val *item = yyjson_arr_get(out, i);
		if (!item || !yyjson_is_obj(item))
			continue;
		yyjson_val *type = yyjson_obj_get(item, "type");
		if (!type || !yyjson_is_str(type))
			continue;
		const char *t = yyjson_get_str(type);
		if (!t || strcmp(t, "output_text") != 0)
			continue;
		yyjson_val *text = yyjson_obj_get(item, "text");
		if (text && yyjson_is_str(text)) {
			ret = dup_cstr(yyjson_get_str(text));
			break;
		}
	}

	yyjson_doc_free(doc);
	return ret;
}

bool aicli_auto_search_plan(const aicli_config_t *cfg,
			   const char *user_prompt,
			   char **out_query)
{
	if (out_query)
		*out_query = NULL;
	if (!cfg || !cfg->openai_api_key || !cfg->openai_api_key[0])
		return false;
	if (!user_prompt || !user_prompt[0])
		return false;

	const char *model = (cfg->model && cfg->model[0]) ? cfg->model : "gpt-5-mini";

	// Keep it extremely small and robust.
	const char *system =
	    "You are a query planner. Decide if web search is truly required. "
	    "Reply with ONLY valid JSON (no markdown), with fields: "
	    "{\"need_search\":true|false,\"query\":string}. "
	    "If need_search=false, query must be \"\". "
	    "Keep query <= 12 words, focused, and safe.";

	aicli_openai_request_t req = {
	    .model = model,
	    .input_text = user_prompt,
	    .system_text = system,
	};

	aicli_openai_http_response_t http = {0};
	int rc = aicli_openai_responses_post(cfg->openai_api_key, cfg->openai_base_url,
	                                   &req, NULL, "none", &http);
	if (rc != 0) {
		aicli_openai_http_response_free(&http);
		return false;
	}
	if (http.http_status != 200 || !http.body) {
		aicli_openai_http_response_free(&http);
		return false;
	}

	char *text = extract_output_text(http.body, http.body_len);
	aicli_openai_http_response_free(&http);
	if (!text)
		return false;
	trim_in_place(text);

	// The model should have returned a JSON object as plain text.
	yyjson_doc *jdoc = yyjson_read(text, strlen(text), 0);
	if (!jdoc) {
		free(text);
		return false;
	}
	yyjson_val *root = yyjson_doc_get_root(jdoc);
	if (!root || !yyjson_is_obj(root)) {
		yyjson_doc_free(jdoc);
		free(text);
		return false;
	}

	bool need = false;
	const char *q = NULL;
	yyjson_val *v;
	v = yyjson_obj_get(root, "need_search");
	if (v && yyjson_is_bool(v))
		need = yyjson_get_bool(v);
	v = yyjson_obj_get(root, "query");
	if (v && yyjson_is_str(v))
		q = yyjson_get_str(v);

	bool ok = false;
	if (need && q && q[0]) {
		char *dq = dup_cstr(q);
		if (dq) {
			trim_in_place(dq);
			if (dq[0]) {
				if (out_query)
					*out_query = dq;
				else
					free(dq);
				ok = true;
			} else {
				free(dq);
			}
		}
	}

	yyjson_doc_free(jdoc);
	free(text);
	return ok;
}

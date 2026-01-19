#include "openai_tool_loop.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "openai_responses.h"
#include "threadpool.h"

static const char *safe_str(const char *s) { return s ? s : ""; }

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

static char *build_execute_tool_json(void)
{
	// JSON array of tool definitions for Responses API.
	// Keep schema minimal and aligned with docs/design.md.
	yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
	yyjson_mut_val *arr = yyjson_mut_arr(doc);
	yyjson_mut_doc_set_root(doc, arr);

	yyjson_mut_val *tool = yyjson_mut_obj(doc);
	yyjson_mut_arr_add_val(arr, tool);
	yyjson_mut_obj_add_str(doc, tool, "type", "function");
		yyjson_mut_obj_add_str(doc, tool, "name", "execute");

		yyjson_mut_val *fn = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_val(doc, tool, "function", fn);
		yyjson_mut_obj_add_str(doc, fn, "description",
	                      "Read-only restricted file access via a safe DSL."
	                      " Use for reading allowlisted local files.");

	yyjson_mut_val *params = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_val(doc, fn, "parameters", params);
	yyjson_mut_obj_add_str(doc, params, "type", "object");

	yyjson_mut_val *props = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_val(doc, params, "properties", props);

	yyjson_mut_val *p_command = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_command, "type", "string");
	yyjson_mut_obj_add_str(doc, p_command, "description",
	                      "Restricted pipeline DSL command (e.g. 'cat <FILE>').");
	yyjson_mut_obj_add_val(doc, props, "command", p_command);

	yyjson_mut_val *p_file = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_file, "type", "string");
	yyjson_mut_obj_add_str(doc, p_file, "description", "Optional primary file hint.");
	yyjson_mut_obj_add_val(doc, props, "file", p_file);

	yyjson_mut_val *p_start = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_start, "type", "integer");
	yyjson_mut_obj_add_str(doc, p_start, "minimum", "0");
	yyjson_mut_obj_add_str(doc, p_start, "description", "Byte offset for paging.");
	yyjson_mut_obj_add_val(doc, props, "start", p_start);

	yyjson_mut_val *p_size = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_size, "type", "integer");
	yyjson_mut_obj_add_str(doc, p_size, "minimum", "1");
	yyjson_mut_obj_add_str(doc, p_size, "maximum", "4096");
	yyjson_mut_obj_add_str(doc, p_size, "description", "Max bytes to return (<=4096).");
	yyjson_mut_obj_add_val(doc, props, "size", p_size);

	yyjson_mut_val *required = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_str(doc, required, "command");
	yyjson_mut_obj_add_val(doc, params, "required", required);

	char *json = yyjson_mut_write(doc, 0, NULL);
	yyjson_mut_doc_free(doc);
	return json;
}

typedef struct {
	const aicli_allowlist_t *allow;
	aicli_execute_request_t req;
	aicli_tool_result_t res;
	bool done;
} exec_job_t;

static void exec_job_main(void *arg)
{
	exec_job_t *j = (exec_job_t *)arg;
	(void)aicli_execute_run(j->allow, &j->req, &j->res);
	j->done = true;
}

static int parse_execute_arguments(yyjson_val *args, aicli_execute_request_t *out)
{
	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));
	if (!args || !yyjson_is_obj(args))
		return 1;

	yyjson_val *v;
	v = yyjson_obj_get(args, "command");
	if (!v || !yyjson_is_str(v))
		return 2;
	out->command = yyjson_get_str(v);

	v = yyjson_obj_get(args, "file");
	if (v && yyjson_is_str(v))
		out->file = yyjson_get_str(v);

	v = yyjson_obj_get(args, "id");
	if (v && yyjson_is_str(v))
		out->id = yyjson_get_str(v);

	v = yyjson_obj_get(args, "idempotency");
	if (v && yyjson_is_str(v))
		out->idempotency = yyjson_get_str(v);

	v = yyjson_obj_get(args, "start");
	if (v && yyjson_is_int(v))
		out->start = (size_t)yyjson_get_int(v);

	v = yyjson_obj_get(args, "size");
	if (v && yyjson_is_int(v))
		out->size = (size_t)yyjson_get_int(v);

	return 0;
}

static yyjson_val *find_output_array(yyjson_val *root)
{
	if (!root || !yyjson_is_obj(root))
		return NULL;
	yyjson_val *out = yyjson_obj_get(root, "output");
	if (out && yyjson_is_arr(out))
		return out;
	return NULL;
}

static const char *extract_response_id(yyjson_val *root)
{
	if (!root || !yyjson_is_obj(root))
		return NULL;
	yyjson_val *id = yyjson_obj_get(root, "id");
	if (!id || !yyjson_is_str(id))
		return NULL;
	return yyjson_get_str(id);
}

static char *extract_first_output_text(yyjson_val *root)
{
	yyjson_val *out = find_output_array(root);
	if (!out)
		return NULL;

	size_t idx, max = yyjson_arr_size(out);
	for (idx = 0; idx < max; idx++) {
		yyjson_val *item = yyjson_arr_get(out, idx);
		if (!item || !yyjson_is_obj(item))
			continue;

		// Newer Responses shape:
		// output[]: {type:"message", content:[{type:"output_text", text:"..."}, ...]}
		yyjson_val *content = yyjson_obj_get(item, "content");
		if (content && yyjson_is_arr(content)) {
			size_t ci, cmax = yyjson_arr_size(content);
			for (ci = 0; ci < cmax; ci++) {
				yyjson_val *citem = yyjson_arr_get(content, ci);
				if (!citem || !yyjson_is_obj(citem))
					continue;
				yyjson_val *ctype = yyjson_obj_get(citem, "type");
				if (!ctype || !yyjson_is_str(ctype))
					continue;
				const char *ct = yyjson_get_str(ctype);
				if (!ct || strcmp(ct, "output_text") != 0)
					continue;
				yyjson_val *text = yyjson_obj_get(citem, "text");
				if (text && yyjson_is_str(text))
					return dup_cstr(yyjson_get_str(text));
			}
		}

		// Backward-compatible fallback:
		// output[] item itself is {type:"output_text", text:"..."}
		yyjson_val *type = yyjson_obj_get(item, "type");
		if (type && yyjson_is_str(type)) {
			const char *t = yyjson_get_str(type);
			if (t && strcmp(t, "output_text") == 0) {
				yyjson_val *text = yyjson_obj_get(item, "text");
				if (text && yyjson_is_str(text))
					return dup_cstr(yyjson_get_str(text));
			}
		}
	}
	return NULL;
}

static int collect_execute_calls(yyjson_val *root,
                                exec_job_t *jobs,
                                const char **call_ids,
                                size_t cap,
                                size_t *out_count)
{
	if (out_count)
		*out_count = 0;
	if (!root || !jobs || !call_ids || cap == 0)
		return 2;

	yyjson_val *out = find_output_array(root);
	if (!out)
		return 0;

	size_t idx, max = yyjson_arr_size(out);
	size_t n = 0;
	for (idx = 0; idx < max && n < cap; idx++) {
		yyjson_val *item = yyjson_arr_get(out, idx);
		if (!item || !yyjson_is_obj(item))
			continue;
		yyjson_val *type = yyjson_obj_get(item, "type");
		if (!type || !yyjson_is_str(type))
			continue;
		const char *t = yyjson_get_str(type);
		if (!t || strcmp(t, "function_call") != 0)
			continue;

		yyjson_val *name = yyjson_obj_get(item, "name");
		if (!name || !yyjson_is_str(name))
			continue;
		if (strcmp(yyjson_get_str(name), "execute") != 0)
			continue;

		yyjson_val *call_id = yyjson_obj_get(item, "call_id");
		if (!call_id || !yyjson_is_str(call_id))
			continue;

		yyjson_val *args = yyjson_obj_get(item, "arguments");
		if (!args || !yyjson_is_obj(args))
			continue;

		jobs[n].req = (aicli_execute_request_t){0};
		jobs[n].allow = NULL;
		jobs[n].done = false;
		memset(&jobs[n].res, 0, sizeof(jobs[n].res));

		if (parse_execute_arguments(args, &jobs[n].req) != 0)
			continue;
		call_ids[n] = yyjson_get_str(call_id);
		n++;
	}

	if (out_count)
		*out_count = n;
	return 0;
}

static char *build_function_call_output_item_json(const char *call_id, const aicli_tool_result_t *r)
{
	// Build a single output item:
	// {"type":"function_call_output","call_id":"...","output":"{...json...}"}
	yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
	yyjson_mut_val *item = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, item);

	yyjson_mut_obj_add_str(doc, item, "type", "function_call_output");
	yyjson_mut_obj_add_str(doc, item, "call_id", safe_str(call_id));

	// output must be a string; embed JSON of tool result.
	yyjson_mut_doc *rd = yyjson_mut_doc_new(NULL);
	yyjson_mut_val *ro = yyjson_mut_obj(rd);
	yyjson_mut_doc_set_root(rd, ro);

	yyjson_mut_obj_add_bool(rd, ro, "ok", r && r->exit_code == 0);
	yyjson_mut_obj_add_int(rd, ro, "exit_code", r ? r->exit_code : 2);
	yyjson_mut_obj_add_str(rd, ro, "stdout_text", r && r->stdout_text ? r->stdout_text : "");
	yyjson_mut_obj_add_str(rd, ro, "stderr_text", r && r->stderr_text ? r->stderr_text : "");
	yyjson_mut_obj_add_int(rd, ro, "total_bytes", (long long)(r ? (long long)r->total_bytes : 0));
	yyjson_mut_obj_add_bool(rd, ro, "truncated", r ? r->truncated : true);
	yyjson_mut_obj_add_bool(rd, ro, "cache_hit", r ? r->cache_hit : false);
	if (r && r->has_next_start)
		yyjson_mut_obj_add_int(rd, ro, "next_start", (long long)r->next_start);
	else
		yyjson_mut_obj_add_null(rd, ro, "next_start");

	char *rjson = yyjson_mut_write(rd, 0, NULL);
	yyjson_mut_doc_free(rd);
	if (!rjson) {
		yyjson_mut_doc_free(doc);
		return NULL;
	}

	yyjson_mut_obj_add_str(doc, item, "output", rjson);
	free(rjson);

	char *json = yyjson_mut_write(doc, 0, NULL);
	yyjson_mut_doc_free(doc);
	return json;
}

static char *build_next_request_json(const char *model,
				    const char *previous_response_id,
				    const char *tools_json,
				    const char **items_json,
				    size_t item_count)
{
	if (!model || !model[0] || !previous_response_id || !previous_response_id[0])
		return NULL;
	if (!items_json || item_count == 0)
		return NULL;

	yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
	yyjson_mut_val *root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);

	yyjson_mut_obj_add_str(doc, root, "model", model);
	yyjson_mut_obj_add_str(doc, root, "previous_response_id", previous_response_id);

	// input: an array of tool output items.
	yyjson_mut_val *input = yyjson_mut_arr(doc);
	yyjson_mut_obj_add_val(doc, root, "input", input);

	for (size_t i = 0; i < item_count; i++) {
		const char *s = items_json[i];
		if (!s || !s[0])
			continue;
		yyjson_doc *idoc = yyjson_read(s, strlen(s), 0);
		if (!idoc)
			continue;
		yyjson_val *iroot = yyjson_doc_get_root(idoc);
		yyjson_mut_val *imut = yyjson_val_mut_copy(doc, iroot);
		if (imut)
			yyjson_mut_arr_add_val(input, imut);
		yyjson_doc_free(idoc);
	}

	if (tools_json && tools_json[0]) {
		yyjson_doc *tdoc = yyjson_read(tools_json, strlen(tools_json), 0);
		if (tdoc) {
			yyjson_val *troot = yyjson_doc_get_root(tdoc);
			yyjson_mut_val *tmut = yyjson_val_mut_copy(doc, troot);
			if (tmut)
				yyjson_mut_obj_add_val(doc, root, "tools", tmut);
			yyjson_doc_free(tdoc);
		}
	}

	char *json = yyjson_mut_write(doc, 0, NULL);
	yyjson_mut_doc_free(doc);
	return json;
}

int aicli_openai_run_with_tools(const aicli_config_t *cfg,
                               const aicli_allowlist_t *allow,
                               const char *user_prompt,
                               size_t max_turns,
                               size_t max_tool_calls_per_turn,
                               size_t tool_threads,
                               char **out_final_text)
{
	if (out_final_text)
		*out_final_text = NULL;
	if (!cfg || !user_prompt || !user_prompt[0])
		return 2;
	if (max_turns == 0)
		max_turns = 4;
	if (max_tool_calls_per_turn == 0)
		max_tool_calls_per_turn = 8;
	if (tool_threads == 0)
		tool_threads = 1;

	char *tools_json = build_execute_tool_json();
	if (!tools_json)
		return 2;

	const char *model = (cfg->model && cfg->model[0]) ? cfg->model : "gpt-4.1-mini";

	aicli_openai_request_t req0 = {
	    .model = model,
	    .input_text = user_prompt,
	    .system_text = NULL,
	};

	aicli_openai_http_response_t http = {0};
	int rc = aicli_openai_responses_post(cfg->openai_api_key, cfg->openai_base_url,
	                                   &req0, tools_json, NULL, &http);
	if (rc != 0) {
		free(tools_json);
		return rc;
	}
	if (http.http_status != 200 || !http.body || http.body_len == 0) {
		fprintf(stderr, "openai http_status=%d\n", http.http_status);
		if (http.body && http.body_len) {
			size_t n = http.body_len;
			if (n > 2048)
				n = 2048;
			fwrite(http.body, 1, n, stderr);
			fputc('\n', stderr);
			if (n < http.body_len)
				fprintf(stderr, "... (truncated, %zu bytes total)\n", http.body_len);
		}
		aicli_openai_http_response_free(&http);
		free(tools_json);
		return 2;
	}

	for (size_t turn = 0; turn < max_turns; turn++) {
		yyjson_doc *doc = yyjson_read(http.body, http.body_len, 0);
		if (!doc)
			break;
		yyjson_val *root = yyjson_doc_get_root(doc);

		char *final = extract_first_output_text(root);
		if (final) {
			yyjson_doc_free(doc);
			if (out_final_text)
				*out_final_text = final;
			aicli_openai_http_response_free(&http);
			free(tools_json);
			return 0;
		}

		const char *resp_id = extract_response_id(root);
		if (!resp_id) {
			yyjson_doc_free(doc);
			break;
		}

		exec_job_t *jobs = (exec_job_t *)calloc(max_tool_calls_per_turn, sizeof(exec_job_t));
		const char **call_ids = (const char **)calloc(max_tool_calls_per_turn, sizeof(char *));
		char **items_json = (char **)calloc(max_tool_calls_per_turn, sizeof(char *));
		if (!jobs || !call_ids || !items_json) {
			free(jobs);
			free(call_ids);
			free(items_json);
			yyjson_doc_free(doc);
			break;
		}

		size_t call_count = 0;
		(void)collect_execute_calls(root, jobs, call_ids, max_tool_calls_per_turn, &call_count);
		if (call_count == 0) {
			free(jobs);
			free(call_ids);
			free(items_json);
			yyjson_doc_free(doc);
			break;
		}

		aicli_threadpool_t *tp = aicli_threadpool_create(tool_threads);
		if (!tp) {
			free(jobs);
			free(call_ids);
			free(items_json);
			yyjson_doc_free(doc);
			break;
		}

		for (size_t i = 0; i < call_count; i++) {
			jobs[i].allow = allow;
			(void)aicli_threadpool_submit(tp, exec_job_main, &jobs[i]);
		}
		aicli_threadpool_drain(tp);
		aicli_threadpool_destroy(tp);

		for (size_t i = 0; i < call_count; i++) {
			items_json[i] = build_function_call_output_item_json(call_ids[i], &jobs[i].res);
		}

		char *next_payload = build_next_request_json(model, resp_id, tools_json,
		                                           (const char **)items_json, call_count);
		for (size_t i = 0; i < call_count; i++) {
			if (jobs[i].res.stdout_text)
				free((void *)jobs[i].res.stdout_text);
			free(items_json[i]);
		}
		free(items_json);
		free(jobs);
		free(call_ids);
		yyjson_doc_free(doc);

		if (!next_payload)
			break;

		aicli_openai_http_response_free(&http);
		memset(&http, 0, sizeof(http));
		rc = aicli_openai_responses_post_raw_json(cfg->openai_api_key, cfg->openai_base_url,
		                                       next_payload, &http);
		free(next_payload);
		if (rc != 0)
			break;
	}

	aicli_openai_http_response_free(&http);
	free(tools_json);
	return 2;
}

#include "openai_tool_loop.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "openai_responses.h"
#include "threadpool.h"
#include "buf.h"

#include "allowlist_list_tool.h"
#include "cli.h"
#include "paging_cache.h"
#include "web_search_tool.h"
#include "web_fetch_tool.h"

static const char *safe_str(const char *s) { return s ? s : ""; }

static int debug_level_enabled(int level) { return level > 0; }

static size_t debug_max_bytes_for_level(int level)
{
	// 1: summary, 2: normal, 3+: verbose
	if (level <= 0)
		return 0;
	if (level == 1)
		return 512;
	if (level == 2)
		return 2048;
	return 8192;
}

static void debug_print_trunc(FILE *out, const char *label, const char *s, size_t max_bytes)
{
	if (!out || !label)
		return;
	if (!s)
		s = "";
	if (max_bytes == 0) {
		fprintf(out, "%s: (suppressed)\n", label);
		return;
	}
	size_t n = strlen(s);
	if (n > max_bytes)
		n = max_bytes;
	fprintf(out, "%s (%zu bytes%s):\n", label, n, (strlen(s) > n) ? ", truncated" : "");
	fwrite(s, 1, n, out);
	if (strlen(s) > n)
		fputs("\n...\n", out);
	else
		fputc('\n', out);
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

static bool buf_append_json_hex2(aicli_buf_t *b, unsigned char v)
{
	static const char hex[] = "0123456789abcdef";
	char tmp[2];
	tmp[0] = hex[(v >> 4) & 0xF];
	tmp[1] = hex[v & 0xF];
	return aicli_buf_append(b, tmp, sizeof(tmp));
}

static bool buf_append_json_string_escaped_bytes(aicli_buf_t *b, const unsigned char *s, size_t n)
{
	// Appends JSON string content (WITHOUT surrounding quotes).
	// Escapes control chars and forces non-ASCII bytes to \u00XX to avoid UTF-8 concerns.
	for (size_t i = 0; i < n; i++) {
		unsigned char c = s[i];
		switch (c) {
		case '"':
			if (!aicli_buf_append_str(b, "\\\""))
				return false;
			break;
		case '\\':
			if (!aicli_buf_append_str(b, "\\\\"))
				return false;
			break;
		case '\b':
			if (!aicli_buf_append_str(b, "\\b"))
				return false;
			break;
		case '\f':
			if (!aicli_buf_append_str(b, "\\f"))
				return false;
			break;
		case '\n':
			if (!aicli_buf_append_str(b, "\\n"))
				return false;
			break;
		case '\r':
			if (!aicli_buf_append_str(b, "\\r"))
				return false;
			break;
		case '\t':
			if (!aicli_buf_append_str(b, "\\t"))
				return false;
			break;
		default:
			if (c < 0x20 || c >= 0x80) {
				if (!aicli_buf_append_str(b, "\\u00"))
					return false;
				if (!buf_append_json_hex2(b, c))
					return false;
			} else {
				if (!aicli_buf_append(b, &c, 1))
					return false;
			}
			break;
		}
	}
	return true;
}

static bool buf_append_json_string_escaped_cstr(aicli_buf_t *b, const char *s)
{
	if (!s)
		return true;
	return buf_append_json_string_escaped_bytes(b, (const unsigned char *)s, strlen(s));
}

static char *build_function_call_output_item_json_manual(const char *call_id, const aicli_tool_result_t *r)
{
	// Build:
	// {"type":"function_call_output","call_id":"...","output":"{...json...}"}
	// NOTE: output must be a JSON string, so the inner JSON is double-escaped.
	if (!call_id || !call_id[0])
		return NULL;

	aicli_buf_t b;
	if (!aicli_buf_init(&b, 512))
		return NULL;

	bool ok = true;
	ok = ok && aicli_buf_append_str(&b, "{\"type\":\"function_call_output\",\"call_id\":\"");
	ok = ok && buf_append_json_string_escaped_cstr(&b, call_id);
	ok = ok && aicli_buf_append_str(&b, "\",\"output\":\"");

	// Begin inner JSON (as a string):
	ok = ok && aicli_buf_append_str(&b, "{\\\"ok\\\":");
	ok = ok && aicli_buf_append_str(&b, (r && r->exit_code == 0) ? "true" : "false");

	char tmp[128];
	snprintf(tmp, sizeof(tmp), ",\\\"exit_code\\\":%d", r ? r->exit_code : 2);
	ok = ok && aicli_buf_append_str(&b, tmp);

	// stdout_text (bytes -> \u00XX-safe)
	ok = ok && aicli_buf_append_str(&b, ",\\\"stdout_text\\\":\\\"");
	if (r && r->stdout_text && r->stdout_len) {
		// Escape stdout bytes into JSON string content, then escape again for outer string.
		// We do this by emitting \u00XX etc already containing backslashes; so we must escape
		// backslash for the OUTER string: each '\\' becomes '\\\\'.
		// Simplest: escape bytes as JSON for inner, but with '\\' doubled.
		for (size_t i = 0; ok && i < r->stdout_len; i++) {
			unsigned char c = (unsigned char)r->stdout_text[i];
			switch (c) {
			case '"':
				ok = ok && aicli_buf_append_str(&b, "\\\\\\\"");
				break;
			case '\\':
				ok = ok && aicli_buf_append_str(&b, "\\\\\\\\");
				break;
			case '\b':
				ok = ok && aicli_buf_append_str(&b, "\\\\b");
				break;
			case '\f':
				ok = ok && aicli_buf_append_str(&b, "\\\\f");
				break;
			case '\n':
				ok = ok && aicli_buf_append_str(&b, "\\\\n");
				break;
			case '\r':
				ok = ok && aicli_buf_append_str(&b, "\\\\r");
				break;
			case '\t':
				ok = ok && aicli_buf_append_str(&b, "\\\\t");
				break;
			default:
				if (c < 0x20 || c >= 0x80) {
					ok = ok && aicli_buf_append_str(&b, "\\\\u00");
					ok = ok && buf_append_json_hex2(&b, c);
				} else {
					ok = ok && aicli_buf_append(&b, &c, 1);
				}
				break;
			}
		}
	}
	ok = ok && aicli_buf_append_str(&b, "\\\"");

	// stderr_text (cstr)
	ok = ok && aicli_buf_append_str(&b, ",\\\"stderr_text\\\":\\\"");
	if (r && r->stderr_text && r->stderr_text[0]) {
		// Same doubling rule as above: escape for inner, but keep backslashes doubled.
		const unsigned char *sp = (const unsigned char *)r->stderr_text;
		size_t sn = strlen(r->stderr_text);
		for (size_t i = 0; ok && i < sn; i++) {
			unsigned char c = sp[i];
			switch (c) {
			case '"':
				ok = ok && aicli_buf_append_str(&b, "\\\\\\\"");
				break;
			case '\\':
				ok = ok && aicli_buf_append_str(&b, "\\\\\\\\");
				break;
			case '\n':
				ok = ok && aicli_buf_append_str(&b, "\\\\n");
				break;
			case '\r':
				ok = ok && aicli_buf_append_str(&b, "\\\\r");
				break;
			case '\t':
				ok = ok && aicli_buf_append_str(&b, "\\\\t");
				break;
			default:
				if (c < 0x20 || c >= 0x80) {
					ok = ok && aicli_buf_append_str(&b, "\\\\u00");
					ok = ok && buf_append_json_hex2(&b, c);
				} else {
					ok = ok && aicli_buf_append(&b, &c, 1);
				}
				break;
			}
		}
	}
	ok = ok && aicli_buf_append_str(&b, "\\\"");

	snprintf(tmp, sizeof(tmp), ",\\\"total_bytes\\\":%zu", r ? r->total_bytes : (size_t)0);
	ok = ok && aicli_buf_append_str(&b, tmp);
	ok = ok && aicli_buf_append_str(&b, ",\\\"truncated\\\":");
	ok = ok && aicli_buf_append_str(&b, (r && r->truncated) ? "true" : "false");
	ok = ok && aicli_buf_append_str(&b, ",\\\"cache_hit\\\":");
	ok = ok && aicli_buf_append_str(&b, (r && r->cache_hit) ? "true" : "false");

	if (r && r->has_next_start) {
		snprintf(tmp, sizeof(tmp), ",\\\"next_start\\\":%zu", r->next_start);
		ok = ok && aicli_buf_append_str(&b, tmp);
	} else {
		ok = ok && aicli_buf_append_str(&b, ",\\\"next_start\\\":null");
	}

	ok = ok && aicli_buf_append_str(&b, "}");

	// Close outer output string and object.
	ok = ok && aicli_buf_append_str(&b, "\"}");

	if (!ok) {
		aicli_buf_free(&b);
		return NULL;
	}

	// NUL-terminate.
	if (!aicli_buf_append(&b, "\0", 1)) {
		aicli_buf_free(&b);
		return NULL;
	}
	char *out = b.data;
	// Transfer ownership.
	return out;
}

static char *build_execute_tool_json(void)
{
	// JSON array of tool definitions for Responses API.
	// Keep schema minimal and aligned with docs/design.md.
	yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
	yyjson_mut_val *arr = yyjson_mut_arr(doc);
	yyjson_mut_doc_set_root(doc, arr);

	// tools: [{...execute...}, {...list_allowed_files...}]
	yyjson_mut_val *tool = yyjson_mut_obj(doc);
	yyjson_mut_arr_add_val(arr, tool);
	yyjson_mut_obj_add_str(doc, tool, "type", "function");
	yyjson_mut_obj_add_str(doc, tool, "name", "execute");
	// Keep strict disabled unless we can satisfy all strict requirements.
	yyjson_mut_obj_add_bool(doc, tool, "strict", false);
	// Description placed on the tool object per official docs.
	yyjson_mut_obj_add_str(doc, tool, "description",
	                      "Read-only restricted file access via a safe DSL. "
	                      "Use ONLY for reading allowlisted local files. "
	                      "MUST provide 'command'. Examples: \n"
	                      "'cat README.md', 'cat README.md | head -n 80', 'sed -n 1,120p README.md'. "
	                      "Do NOT use a shell; do NOT use redirections/globs; "
	                      "keep it simple and safe.");

	yyjson_mut_val *params = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_val(doc, tool, "parameters", params);
	yyjson_mut_obj_add_str(doc, params, "type", "object");
	yyjson_mut_obj_add_bool(doc, params, "additionalProperties", false);

	yyjson_mut_val *props = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_val(doc, params, "properties", props);

	yyjson_mut_val *p_command = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_command, "type", "string");
	yyjson_mut_obj_add_str(doc, p_command, "description",
	                      "REQUIRED. Restricted pipeline DSL command, e.g. 'cat README.md' or 'head -n 80 README.md'.");
	yyjson_mut_obj_add_val(doc, props, "command", p_command);

	yyjson_mut_val *p_file = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_file, "type", "string");
	yyjson_mut_obj_add_str(doc, p_file, "description", "Optional primary file hint.");
	yyjson_mut_obj_add_val(doc, props, "file", p_file);

	yyjson_mut_val *p_start = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_start, "type", "integer");
	yyjson_mut_obj_add_int(doc, p_start, "minimum", 0);
	yyjson_mut_obj_add_str(doc, p_start, "description", "Byte offset for paging.");
	yyjson_mut_obj_add_val(doc, props, "start", p_start);

	yyjson_mut_val *p_size = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_size, "type", "integer");
	yyjson_mut_obj_add_int(doc, p_size, "minimum", 1);
	yyjson_mut_obj_add_int(doc, p_size, "maximum", 4096);
	yyjson_mut_obj_add_str(doc, p_size, "description", "Max bytes to return (<=4096).");
	yyjson_mut_obj_add_val(doc, props, "size", p_size);

	yyjson_mut_val *required = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_str(doc, required, "command");
	yyjson_mut_obj_add_val(doc, params, "required", required);

	// list_allowed_files
	yyjson_mut_val *tool2 = yyjson_mut_obj(doc);
	yyjson_mut_arr_add_val(arr, tool2);
	yyjson_mut_obj_add_str(doc, tool2, "type", "function");
	yyjson_mut_obj_add_str(doc, tool2, "name", "list_allowed_files");
	yyjson_mut_obj_add_bool(doc, tool2, "strict", false);
	yyjson_mut_obj_add_str(doc, tool2, "description",
	                      "Read-only: list allowlisted local files for the execute tool. "
	                      "Returns paths/names/sizes only (no file contents). "
	                      "Supports case-insensitive substring filtering (query) and paging (start/size). ");

	yyjson_mut_val *params2 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_val(doc, tool2, "parameters", params2);
	yyjson_mut_obj_add_str(doc, params2, "type", "object");
	yyjson_mut_obj_add_bool(doc, params2, "additionalProperties", false);

	yyjson_mut_val *props2 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_val(doc, params2, "properties", props2);

	yyjson_mut_val *p_query = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_query, "type", "string");
	yyjson_mut_obj_add_str(doc, p_query, "description", "Optional case-insensitive substring filter on full path.");
	yyjson_mut_obj_add_val(doc, props2, "query", p_query);

	yyjson_mut_val *p_start2 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_start2, "type", "integer");
	yyjson_mut_obj_add_int(doc, p_start2, "minimum", 0);
	yyjson_mut_obj_add_str(doc, p_start2, "description", "0-based start index for paging.");
	yyjson_mut_obj_add_val(doc, props2, "start", p_start2);

	yyjson_mut_val *p_size2 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_size2, "type", "integer");
	yyjson_mut_obj_add_int(doc, p_size2, "minimum", 1);
	yyjson_mut_obj_add_int(doc, p_size2, "maximum", 200);
	yyjson_mut_obj_add_str(doc, p_size2, "description", "Max items to return (<=200). Default 50.");
	yyjson_mut_obj_add_val(doc, props2, "size", p_size2);

	// web_search
	yyjson_mut_val *tool3 = yyjson_mut_obj(doc);
	yyjson_mut_arr_add_val(arr, tool3);
	yyjson_mut_obj_add_str(doc, tool3, "type", "function");
	yyjson_mut_obj_add_str(doc, tool3, "name", "web_search");
	yyjson_mut_obj_add_bool(doc, tool3, "strict", false);
	yyjson_mut_obj_add_str(doc, tool3, "description",
	                      "Web search (read-only, network). Uses configured provider (google_cse or brave). "
	                      "Supports paging via start/size (bytes of returned text/JSON). ");

	yyjson_mut_val *params3 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_val(doc, tool3, "parameters", params3);
	yyjson_mut_obj_add_str(doc, params3, "type", "object");
	yyjson_mut_obj_add_bool(doc, params3, "additionalProperties", false);
	yyjson_mut_val *props3 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_val(doc, params3, "properties", props3);

	yyjson_mut_val *p_q3 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_q3, "type", "string");
	yyjson_mut_obj_add_str(doc, p_q3, "description", "REQUIRED. Search query string.");
	yyjson_mut_obj_add_val(doc, props3, "query", p_q3);

	yyjson_mut_val *p_provider3 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_provider3, "type", "string");
	yyjson_mut_obj_add_str(doc, p_provider3, "description", "Optional provider override: auto|google_cse|brave.");
	yyjson_mut_obj_add_val(doc, props3, "provider", p_provider3);

	yyjson_mut_val *p_count3 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_count3, "type", "integer");
	yyjson_mut_obj_add_int(doc, p_count3, "minimum", 1);
	yyjson_mut_obj_add_int(doc, p_count3, "maximum", 20);
	yyjson_mut_obj_add_str(doc, p_count3, "description", "Optional max results (provider-capped).");
	yyjson_mut_obj_add_val(doc, props3, "count", p_count3);

	yyjson_mut_val *p_lang3 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_lang3, "type", "string");
	yyjson_mut_obj_add_str(doc, p_lang3, "description", "Optional language hint (brave) or locale string.");
	yyjson_mut_obj_add_val(doc, props3, "lang", p_lang3);

	yyjson_mut_val *p_fresh3 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_fresh3, "type", "string");
	yyjson_mut_obj_add_str(doc, p_fresh3, "description", "Optional freshness: day|week|month (brave).");
	yyjson_mut_obj_add_val(doc, props3, "freshness", p_fresh3);

	yyjson_mut_val *p_raw3 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_raw3, "type", "boolean");
	yyjson_mut_obj_add_str(doc, p_raw3, "description", "Optional: return raw JSON bytes when possible.");
	yyjson_mut_obj_add_val(doc, props3, "raw", p_raw3);

	yyjson_mut_val *p_start3 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_start3, "type", "integer");
	yyjson_mut_obj_add_int(doc, p_start3, "minimum", 0);
	yyjson_mut_obj_add_str(doc, p_start3, "description", "Byte offset for paging.");
	yyjson_mut_obj_add_val(doc, props3, "start", p_start3);

	yyjson_mut_val *p_size3 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_size3, "type", "integer");
	yyjson_mut_obj_add_int(doc, p_size3, "minimum", 1);
	yyjson_mut_obj_add_int(doc, p_size3, "maximum", 4096);
	yyjson_mut_obj_add_str(doc, p_size3, "description", "Max bytes to return (<=4096).");
	yyjson_mut_obj_add_val(doc, props3, "size", p_size3);

	yyjson_mut_val *p_idem3 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_idem3, "type", "string");
	yyjson_mut_obj_add_str(doc, p_idem3, "description", "Optional idempotency key for caching.");
	yyjson_mut_obj_add_val(doc, props3, "idempotency", p_idem3);

	yyjson_mut_val *req3 = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_str(doc, req3, "query");
	yyjson_mut_obj_add_val(doc, params3, "required", req3);

	// web_fetch
	yyjson_mut_val *tool4 = yyjson_mut_obj(doc);
	yyjson_mut_arr_add_val(arr, tool4);
	yyjson_mut_obj_add_str(doc, tool4, "type", "function");
	yyjson_mut_obj_add_str(doc, tool4, "name", "web_fetch");
	yyjson_mut_obj_add_bool(doc, tool4, "strict", false);
	yyjson_mut_obj_add_str(doc, tool4, "description",
	                      "Fetch a URL via HTTP GET with strict allowlisted URL prefixes. "
	                      "Supports paging via start/size. ");

	yyjson_mut_val *params4 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_val(doc, tool4, "parameters", params4);
	yyjson_mut_obj_add_str(doc, params4, "type", "object");
	yyjson_mut_obj_add_bool(doc, params4, "additionalProperties", false);
	yyjson_mut_val *props4 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_val(doc, params4, "properties", props4);

	yyjson_mut_val *p_url4 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_url4, "type", "string");
	yyjson_mut_obj_add_str(doc, p_url4, "description", "REQUIRED. URL to fetch (GET only).");
	yyjson_mut_obj_add_val(doc, props4, "url", p_url4);

	yyjson_mut_val *p_start4 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_start4, "type", "integer");
	yyjson_mut_obj_add_int(doc, p_start4, "minimum", 0);
	yyjson_mut_obj_add_str(doc, p_start4, "description", "Byte offset for paging.");
	yyjson_mut_obj_add_val(doc, props4, "start", p_start4);

	yyjson_mut_val *p_size4 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_size4, "type", "integer");
	yyjson_mut_obj_add_int(doc, p_size4, "minimum", 1);
	yyjson_mut_obj_add_int(doc, p_size4, "maximum", 4096);
	yyjson_mut_obj_add_str(doc, p_size4, "description", "Max bytes to return (<=4096).");
	yyjson_mut_obj_add_val(doc, props4, "size", p_size4);

	yyjson_mut_val *p_idem4 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_idem4, "type", "string");
	yyjson_mut_obj_add_str(doc, p_idem4, "description", "Optional idempotency key for caching.");
	yyjson_mut_obj_add_val(doc, props4, "idempotency", p_idem4);

	yyjson_mut_val *req4 = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_str(doc, req4, "url");
	yyjson_mut_obj_add_val(doc, params4, "required", req4);

	// cli_help
	yyjson_mut_val *tool5 = yyjson_mut_obj(doc);
	yyjson_mut_arr_add_val(arr, tool5);
	yyjson_mut_obj_add_str(doc, tool5, "type", "function");
	yyjson_mut_obj_add_str(doc, tool5, "name", "cli_help");
	yyjson_mut_obj_add_bool(doc, tool5, "strict", false);
	yyjson_mut_obj_add_str(doc, tool5, "description",
	                      "Read-only: return built-in aicli CLI help/usage text. "
	                      "Use this when you need to tell the user which flags or environment variables are required "
	                      "(e.g. web_search provider keys, web_fetch allowlist). "
	                      "Supports paging via start/size.");

	yyjson_mut_val *params5 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_val(doc, tool5, "parameters", params5);
	yyjson_mut_obj_add_str(doc, params5, "type", "object");
	yyjson_mut_obj_add_bool(doc, params5, "additionalProperties", false);

	yyjson_mut_val *props5 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_val(doc, params5, "properties", props5);

	yyjson_mut_val *p_topic5 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_topic5, "type", "string");
	yyjson_mut_obj_add_str(doc, p_topic5, "description",
	                      "Optional topic/subcommand, e.g. 'main', 'run', 'web', 'web search', 'web fetch', 'execute'. Defaults to 'main'.");
	yyjson_mut_obj_add_val(doc, props5, "topic", p_topic5);

	yyjson_mut_val *p_start5 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_start5, "type", "integer");
	yyjson_mut_obj_add_int(doc, p_start5, "minimum", 0);
	yyjson_mut_obj_add_str(doc, p_start5, "description", "Byte offset for paging.");
	yyjson_mut_obj_add_val(doc, props5, "start", p_start5);

	yyjson_mut_val *p_size5 = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, p_size5, "type", "integer");
	yyjson_mut_obj_add_int(doc, p_size5, "minimum", 1);
	yyjson_mut_obj_add_int(doc, p_size5, "maximum", 4096);
	yyjson_mut_obj_add_str(doc, p_size5, "description", "Max bytes to return (<=4096).");
	yyjson_mut_obj_add_val(doc, props5, "size", p_size5);

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

typedef struct {
	const aicli_allowlist_t *allow;
	aicli_list_allowed_files_request_t req;
	aicli_list_allowed_files_result_t res;
	bool done;
} list_job_t;

typedef struct {
	const aicli_config_t *cfg;
	aicli_paging_cache_t *cache;
	aicli_web_search_tool_request_t req;
	aicli_tool_result_t res;
	bool done;
} web_search_job_t;

typedef struct {
	const aicli_config_t *cfg;
	aicli_paging_cache_t *cache;
	aicli_web_fetch_tool_request_t req;
	aicli_tool_result_t res;
	bool done;
} web_fetch_job_t;

typedef struct {
	char *topic;
	size_t start;
	size_t size;
	aicli_tool_result_t res;
	bool done;
} cli_help_job_t;

static const char *cli_help_select_text(const char *topic)
{
	const char *t = topic ? topic : "";
	while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r')
		t++;
	if (t[0] == '\0' || strcmp(t, "main") == 0 || strcmp(t, "--help") == 0 || strcmp(t, "help") == 0)
		return aicli_cli_usage_string();
	// For now we return the full help; topic exists for forward compatibility.
	return aicli_cli_usage_string();
}

static void cli_help_job_main(void *arg)
{
	cli_help_job_t *j = (cli_help_job_t *)arg;
	if (!j)
		return;
	memset(&j->res, 0, sizeof(j->res));
	j->res.exit_code = 0;

	const char *text = cli_help_select_text(j->topic);
	if (!text)
		text = "";
	const size_t total = strlen(text);

	size_t start = j->start;
	if (start > total)
		start = total;
	size_t size = j->size;
	if (size == 0 || size > 4096)
		size = 4096;
	size_t n = total - start;
	if (n > size)
		n = size;

	char *out = (char *)malloc(n + 1);
	if (!out) {
		j->res.exit_code = 1;
		j->res.stderr_text = strdup("cli_help: out of memory\n");
		j->done = true;
		return;
	}
	memcpy(out, text + start, n);
	out[n] = '\0';
	j->res.stdout_text = out;
	j->res.total_bytes = total;
	j->res.truncated = (start + n) < total;
	j->res.has_next_start = j->res.truncated;
	j->res.next_start = j->res.truncated ? (start + n) : 0;
	j->res.cache_hit = true;
	j->done = true;
}

static int parse_cli_help_arguments(yyjson_val *args, char **out_topic, size_t *out_start, size_t *out_size)
{
	if (out_topic)
		*out_topic = NULL;
	if (out_start)
		*out_start = 0;
	if (out_size)
		*out_size = 0;
	if (!args)
		return 0;

	if (yyjson_is_str(args)) {
		const char *json = yyjson_get_str(args);
		if (!json)
			return 0;
		yyjson_doc *d = yyjson_read(json, strlen(json), 0);
		if (!d)
			return 0;
		yyjson_val *root = yyjson_doc_get_root(d);
		if (root)
			(void)parse_cli_help_arguments(root, out_topic, out_start, out_size);
		yyjson_doc_free(d);
		return 0;
	}
	if (!yyjson_is_obj(args))
		return 0;

	yyjson_val *topic = yyjson_obj_get(args, "topic");
	if (topic && yyjson_is_str(topic) && out_topic)
		*out_topic = dup_cstr(yyjson_get_str(topic));

	yyjson_val *start = yyjson_obj_get(args, "start");
	if (start && yyjson_is_int(start) && out_start) {
		int64_t v = yyjson_get_sint(start);
		if (v >= 0)
			*out_start = (size_t)v;
	}
	yyjson_val *size = yyjson_obj_get(args, "size");
	if (size && yyjson_is_int(size) && out_size) {
		int64_t v = yyjson_get_sint(size);
		if (v > 0)
			*out_size = (size_t)v;
	}
	return 0;
}

static void free_web_search_request_owned(aicli_web_search_tool_request_t *r)
{
	if (!r)
		return;
	free((void *)r->query);
	free((void *)r->lang);
	free((void *)r->freshness);
	free((void *)r->idempotency);
	memset(r, 0, sizeof(*r));
}

static void free_web_fetch_request_owned(aicli_web_fetch_tool_request_t *r)
{
	if (!r)
		return;
	free((void *)r->url);
	free((void *)r->idempotency);
	// allowed_prefixes are owned elsewhere
	memset(r, 0, sizeof(*r));
}

static int dup_web_search_request_strings(aicli_web_search_tool_request_t *r)
{
	if (!r)
		return 1;
	if (!r->query || !r->query[0])
		return 1;
	char *q = dup_cstr(r->query);
	if (!q)
		return 1;
	char *lang = NULL;
	char *fresh = NULL;
	char *idem = NULL;
	if (r->lang && r->lang[0]) {
		lang = dup_cstr(r->lang);
		if (!lang) {
			free(q);
			return 1;
		}
	}
	if (r->freshness && r->freshness[0]) {
		fresh = dup_cstr(r->freshness);
		if (!fresh) {
			free(q);
			free(lang);
			return 1;
		}
	}
	if (r->idempotency && r->idempotency[0]) {
		idem = dup_cstr(r->idempotency);
		if (!idem) {
			free(q);
			free(lang);
			free(fresh);
			return 1;
		}
	}
	r->query = q;
	r->lang = lang;
	r->freshness = fresh;
	r->idempotency = idem;
	return 0;
}

static int dup_web_fetch_request_strings(aicli_web_fetch_tool_request_t *r)
{
	if (!r)
		return 1;
	if (!r->url || !r->url[0])
		return 1;
	char *u = dup_cstr(r->url);
	if (!u)
		return 1;
	char *idem = NULL;
	if (r->idempotency && r->idempotency[0]) {
		idem = dup_cstr(r->idempotency);
		if (!idem) {
			free(u);
			return 1;
		}
	}
	r->url = u;
	r->idempotency = idem;
	return 0;
}

static aicli_web_provider_t parse_provider_string(const char *s)
{
	if (!s || !s[0])
		return AICLI_WEB_PROVIDER_AUTO;
	if (strcmp(s, "auto") == 0)
		return AICLI_WEB_PROVIDER_AUTO;
	if (strcmp(s, "google") == 0 || strcmp(s, "google_cse") == 0)
		return AICLI_WEB_PROVIDER_GOOGLE_CSE;
	if (strcmp(s, "brave") == 0)
		return AICLI_WEB_PROVIDER_BRAVE;
	return AICLI_WEB_PROVIDER_AUTO;
}

static int parse_web_search_arguments(yyjson_val *args, aicli_web_search_tool_request_t *out)
{
	if (!out)
		return 1;
	*out = (aicli_web_search_tool_request_t){0};
	if (!args)
		return 1;

	if (yyjson_is_str(args)) {
		const char *s = yyjson_get_str(args);
		if (!s || !s[0])
			return 1;
		yyjson_doc *doc = yyjson_read(s, strlen(s), 0);
		if (!doc)
			return 1;
		yyjson_val *root = yyjson_doc_get_root(doc);
		int rc = parse_web_search_arguments(root, out);
		yyjson_doc_free(doc);
		return rc;
	}
	if (!yyjson_is_obj(args))
		return 1;

	yyjson_val *v;
	v = yyjson_obj_get(args, "query");
	if (!v || !yyjson_is_str(v))
		return 2;
	out->query = yyjson_get_str(v);

	v = yyjson_obj_get(args, "provider");
	if (v && yyjson_is_str(v))
		out->provider = parse_provider_string(yyjson_get_str(v));

	v = yyjson_obj_get(args, "count");
	if (v && yyjson_is_int(v))
		out->count = (int)yyjson_get_int(v);

	v = yyjson_obj_get(args, "lang");
	if (v && yyjson_is_str(v))
		out->lang = yyjson_get_str(v);

	v = yyjson_obj_get(args, "freshness");
	if (v && yyjson_is_str(v))
		out->freshness = yyjson_get_str(v);

	v = yyjson_obj_get(args, "raw");
	if (v && yyjson_is_bool(v))
		out->raw = yyjson_get_bool(v);

	v = yyjson_obj_get(args, "start");
	if (v && yyjson_is_int(v))
		out->start = (size_t)yyjson_get_int(v);
	v = yyjson_obj_get(args, "size");
	if (v && yyjson_is_int(v))
		out->size = (size_t)yyjson_get_int(v);

	v = yyjson_obj_get(args, "idempotency");
	if (v && yyjson_is_str(v))
		out->idempotency = yyjson_get_str(v);

	return 0;
}

static int parse_web_fetch_arguments(yyjson_val *args, aicli_web_fetch_tool_request_t *out)
{
	if (!out)
		return 1;
	*out = (aicli_web_fetch_tool_request_t){0};
	if (!args)
		return 1;

	if (yyjson_is_str(args)) {
		const char *s = yyjson_get_str(args);
		if (!s || !s[0])
			return 1;
		yyjson_doc *doc = yyjson_read(s, strlen(s), 0);
		if (!doc)
			return 1;
		yyjson_val *root = yyjson_doc_get_root(doc);
		int rc = parse_web_fetch_arguments(root, out);
		yyjson_doc_free(doc);
		return rc;
	}
	if (!yyjson_is_obj(args))
		return 1;

	yyjson_val *v;
	v = yyjson_obj_get(args, "url");
	if (!v || !yyjson_is_str(v))
		return 2;
	out->url = yyjson_get_str(v);

	v = yyjson_obj_get(args, "start");
	if (v && yyjson_is_int(v))
		out->start = (size_t)yyjson_get_int(v);
	v = yyjson_obj_get(args, "size");
	if (v && yyjson_is_int(v))
		out->size = (size_t)yyjson_get_int(v);

	v = yyjson_obj_get(args, "idempotency");
	if (v && yyjson_is_str(v))
		out->idempotency = yyjson_get_str(v);

	return 0;
}

static void web_search_job_main(void *arg)
{
	web_search_job_t *j = (web_search_job_t *)arg;
	if (!j)
		return;
	memset(&j->res, 0, sizeof(j->res));
	(void)aicli_web_search_tool_run(j->cfg, j->cache, &j->req, &j->res);
	j->done = true;
}

static void web_fetch_job_main(void *arg)
{
	web_fetch_job_t *j = (web_fetch_job_t *)arg;
	if (!j)
		return;
	memset(&j->res, 0, sizeof(j->res));
	(void)aicli_web_fetch_tool_run(j->cfg, j->cache, &j->req, &j->res);
	j->done = true;
}

static void free_list_request_owned(aicli_list_allowed_files_request_t *r)
{
	if (!r)
		return;
	free((void *)r->query);
	memset(r, 0, sizeof(*r));
}

static int parse_list_allowed_files_arguments(yyjson_val *args, aicli_list_allowed_files_request_t *out)
{
	if (!out)
		return 1;
	*out = (aicli_list_allowed_files_request_t){0};

	// arguments can be a JSON string or an object
	yyjson_doc *adoc = NULL;
	yyjson_val *root = NULL;
	if (!args)
		return 0;

	if (yyjson_is_str(args)) {
		const char *s = yyjson_get_str(args);
		if (s && s[0])
			adoc = yyjson_read(s, strlen(s), 0);
		if (adoc)
			root = yyjson_doc_get_root(adoc);
	} else if (yyjson_is_obj(args)) {
		root = args;
	}

	if (!root)
		return 0;

	yyjson_val *q = yyjson_obj_get(root, "query");
	if (q && yyjson_is_str(q))
		out->query = yyjson_get_str(q);

	yyjson_val *st = yyjson_obj_get(root, "start");
	if (st && yyjson_is_int(st)) {
		int64_t v = yyjson_get_sint(st);
		if (v > 0)
			out->start = (size_t)v;
	}

	yyjson_val *sz = yyjson_obj_get(root, "size");
	if (sz && yyjson_is_int(sz)) {
		int64_t v = yyjson_get_sint(sz);
		if (v > 0)
			out->size = (size_t)v;
	}

	if (adoc)
		yyjson_doc_free(adoc);
	return 0;
}

static int dup_list_request_strings(aicli_list_allowed_files_request_t *r)
{
	if (!r)
		return 1;
	if (r->query)
		r->query = dup_cstr(r->query);
	return 0;
}

static void list_job_main(void *arg)
{
	list_job_t *j = (list_job_t *)arg;
	if (!j)
		return;
	j->done = false;
	memset(&j->res, 0, sizeof(j->res));

	int rc = aicli_list_allowed_files_json(j->allow, &j->req, &j->res);
	if (rc != 0) {
		aicli_list_allowed_files_result_free(&j->res);
		j->res.json = dup_cstr("{\"ok\":false,\"error\":\"internal_error\"}");
	}

	j->done = true;
}

static char *build_function_call_output_item_json_raw(const char *call_id, const char *raw_json)
{
	// Build:
	// {"type":"function_call_output","call_id":"...","output":"{...json...}"}
	// NOTE: output must be a JSON string, so raw_json is double-escaped.
	if (!call_id || !call_id[0])
		return NULL;
	if (!raw_json)
		raw_json = "";

	aicli_buf_t b;
	if (!aicli_buf_init(&b, 512))
		return NULL;

	bool ok = true;
	ok = ok && aicli_buf_append_str(&b, "{\"type\":\"function_call_output\",\"call_id\":\"");
	ok = ok && buf_append_json_string_escaped_cstr(&b, call_id);
	ok = ok && aicli_buf_append_str(&b, "\",\"output\":\"");

	// Escape raw_json as content for the outer JSON string.
	ok = ok && buf_append_json_string_escaped_cstr(&b, raw_json);

	ok = ok && aicli_buf_append_str(&b, "\"}");
	if (!ok) {
		aicli_buf_free(&b);
		return NULL;
	}
	if (!aicli_buf_append(&b, "\0", 1)) {
		aicli_buf_free(&b);
		return NULL;
	}
	return b.data;
}

static void free_execute_request_owned(aicli_execute_request_t *r)
{
	if (!r)
		return;
	free((void *)r->id);
	free((void *)r->command);
	free((void *)r->file);
	free((void *)r->idempotency);
	memset(r, 0, sizeof(*r));
}

static int dup_execute_request_strings(aicli_execute_request_t *r)
{
	if (!r)
		return 1;
	// command is required
	if (!r->command || !r->command[0])
		return 1;
	char *cmd = dup_cstr(r->command);
	if (!cmd)
		return 1;
	char *file = NULL;
	char *id = NULL;
	char *idem = NULL;
	if (r->file && r->file[0]) {
		file = dup_cstr(r->file);
		if (!file) {
			free(cmd);
			return 1;
		}
	}
	if (r->id && r->id[0]) {
		id = dup_cstr(r->id);
		if (!id) {
			free(cmd);
			free(file);
			return 1;
		}
	}
	if (r->idempotency && r->idempotency[0]) {
		idem = dup_cstr(r->idempotency);
		if (!idem) {
			free(cmd);
			free(file);
			free(id);
			return 1;
		}
	}

	r->command = cmd;
	r->file = file;
	r->id = id;
	r->idempotency = idem;
	return 0;
}

static void exec_job_main(void *arg)
{
	exec_job_t *j = (exec_job_t *)arg;
	// Optional debug: show allowlisted paths for troubleshooting.
	if (j && j->allow && j->allow->files && j->allow->file_count > 0) {
		// No cfg pointer here; keep it quiet by default.
		// Intentionally not printing unless explicitly enabled.
		const char *env = getenv("AICLI_DEBUG_FUNCTION_CALL");
		if (env && env[0] != '\0') {
			fprintf(stderr, "[debug:execute] allowlist file_count=%d\n", j->allow->file_count);
			for (int i = 0; i < j->allow->file_count; i++) {
				fprintf(stderr, "[debug:execute] allow[%d]=%s\n", i, safe_str(j->allow->files[i].path));
			}
		}
	}
	(void)aicli_execute_run(j->allow, &j->req, &j->res);
	j->done = true;
}

static int parse_execute_arguments(yyjson_val *args, aicli_execute_request_t *out)
{
	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));
	if (!args)
		return 1;

	// Responses API may encode arguments either as an object or as a JSON string.
	// Accept both shapes.
	if (yyjson_is_str(args)) {
		const char *s = yyjson_get_str(args);
		if (!s || !s[0])
			return 1;
		yyjson_doc *doc = yyjson_read(s, strlen(s), 0);
		if (!doc)
			return 1;
		yyjson_val *root = yyjson_doc_get_root(doc);
		int rc = 0;
		if (!root || !yyjson_is_obj(root)) {
			rc = 1;
		} else {
			rc = parse_execute_arguments(root, out);
		}
		yyjson_doc_free(doc);
		return rc;
	}

	if (!yyjson_is_obj(args))
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

static const char *find_first_execute_call_id(yyjson_val *root)
{
	yyjson_val *out = find_output_array(root);
	if (!out)
		return NULL;

	size_t idx, max = yyjson_arr_size(out);
	for (idx = 0; idx < max; idx++) {
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
		const char *n = yyjson_get_str(name);
		if (!n || strcmp(n, "execute") != 0)
			continue;
		yyjson_val *call_id = yyjson_obj_get(item, "call_id");
		if (call_id && yyjson_is_str(call_id))
			return yyjson_get_str(call_id);
	}
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

int aicli_openai_extract_response_id(const char *json, size_t json_len,
				      char *out_id, size_t out_cap)
{
	if (!out_id || out_cap == 0)
		return 2;
	out_id[0] = '\0';
	if (!json || json_len == 0)
		return 2;

	yyjson_doc *doc = yyjson_read(json, json_len, 0);
	if (!doc)
		return 2;
	yyjson_val *root = yyjson_doc_get_root(doc);
	const char *rid = extract_response_id(root);
	if (!rid || !rid[0]) {
		yyjson_doc_free(doc);
		return 1;
	}
	strncpy(out_id, rid, out_cap - 1);
	out_id[out_cap - 1] = '\0';
	yyjson_doc_free(doc);
	return 0;
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
		if (!args)
			continue;

		jobs[n].req = (aicli_execute_request_t){0};
		jobs[n].allow = NULL;
		jobs[n].done = false;
		memset(&jobs[n].res, 0, sizeof(jobs[n].res));

		// IMPORTANT: "arguments" is nested under the response yyjson_doc.
		// We must not keep pointers into that doc after it is freed.
		// Deep-copy the args payload into its own doc so strings remain valid
		// long enough to duplicate them for worker threads.
		yyjson_doc *adoc = NULL;
		if (yyjson_is_str(args)) {
			// Common shape: arguments is a JSON string.
			const char *s = yyjson_get_str(args);
			if (s && s[0])
				adoc = yyjson_read(s, strlen(s), 0);
		} else if (yyjson_is_obj(args)) {
			// Less common shape: arguments is an object.
			// yyjson_write() takes a doc, not a val; wrap via a temporary doc.
			yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
			if (md) {
				yyjson_mut_val *rootv = yyjson_val_mut_copy(md, args);
				if (rootv) {
					yyjson_mut_doc_set_root(md, rootv);
					char *astr = yyjson_mut_write(md, 0, NULL);
					if (astr && astr[0])
						adoc = yyjson_read(astr, strlen(astr), 0);
					free(astr);
				}
				yyjson_mut_doc_free(md);
			}
		}
		if (!adoc)
			continue;
		yyjson_val *aroot = yyjson_doc_get_root(adoc);
		if (parse_execute_arguments(aroot, &jobs[n].req) != 0) {
			yyjson_doc_free(adoc);
			continue;
		}
		if (!jobs[n].req.command || !jobs[n].req.command[0])
			continue;
		// The parsed strings point into the yyjson_doc; copy them so they stay valid
		// after we free the response document and while worker threads run.
		if (dup_execute_request_strings(&jobs[n].req) != 0) {
			yyjson_doc_free(adoc);
			free_execute_request_owned(&jobs[n].req);
			continue;
		}
		// args doc no longer needed once we've duplicated strings.
		yyjson_doc_free(adoc);
		// Keep a stable copy beyond the yyjson_doc lifetime.
		call_ids[n] = dup_cstr(yyjson_get_str(call_id));
		n++;
	}

	if (out_count)
		*out_count = n;
	return 0;
}

static void debug_warn_invalid_execute_calls(const aicli_config_t *cfg, yyjson_val *root)
{
	if (!cfg || !debug_level_enabled(cfg->debug_function_call))
		return;

	yyjson_val *out = find_output_array(root);
	if (!out)
		return;

	size_t idx, max = yyjson_arr_size(out);
	for (idx = 0; idx < max; idx++) {
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
		const char *nstr = (name && yyjson_is_str(name)) ? yyjson_get_str(name) : NULL;
		if (!nstr || strcmp(nstr, "execute") != 0)
			continue;

		yyjson_val *call_id = yyjson_obj_get(item, "call_id");
		const char *cid = (call_id && yyjson_is_str(call_id)) ? yyjson_get_str(call_id) : NULL;
		yyjson_val *args = yyjson_obj_get(item, "arguments");
		aicli_execute_request_t req;
		int prc = parse_execute_arguments(args, &req);
		if (prc != 0 || !req.command || !req.command[0]) {
			fprintf(stderr,
			        "[debug:function_call] WARN: execute call has missing/invalid arguments (need command). call_id=%s\n",
			        safe_str(cid));
		}
	}
}

static void debug_log_execute_calls_if_enabled(const aicli_config_t *cfg, yyjson_val *root)
{
	if (!cfg || !debug_level_enabled(cfg->debug_function_call))
		return;

	yyjson_val *out = find_output_array(root);
	if (!out)
		return;

	size_t max_bytes = debug_max_bytes_for_level(cfg->debug_function_call);
	if (cfg->debug_function_call == 1)
		fprintf(stderr, "[debug:function_call] scanning response for tool calls\n");

	size_t idx, max = yyjson_arr_size(out);
	for (idx = 0; idx < max; idx++) {
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
		yyjson_val *call_id = yyjson_obj_get(item, "call_id");
		const char *nstr = (name && yyjson_is_str(name)) ? yyjson_get_str(name) : NULL;
		const char *cid = (call_id && yyjson_is_str(call_id)) ? yyjson_get_str(call_id) : NULL;
		fprintf(stderr, "[debug:function_call] tool=%s call_id=%s\n", safe_str(nstr), safe_str(cid));

		if (cfg->debug_function_call >= 2) {
			// Print arguments JSON (truncated). Potentially sensitive; keep bounded.
			yyjson_val *args = yyjson_obj_get(item, "arguments");
			if (args) {
				if (yyjson_is_str(args)) {
					debug_print_trunc(stderr, "[debug:function_call] arguments", yyjson_get_str(args), max_bytes);
				} else {
					yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
					yyjson_mut_val *mc = yyjson_val_mut_copy(md, args);
					if (mc) {
						yyjson_mut_doc_set_root(md, mc);
						char *json = yyjson_mut_write(md, 0, NULL);
						if (json) {
							debug_print_trunc(stderr, "[debug:function_call] arguments", json, max_bytes);
							free(json);
						}
					}
					yyjson_mut_doc_free(md);
				}
			}
		}
	}
}

static char *build_function_call_output_item_json(const char *call_id, const aicli_tool_result_t *r)
{
	return build_function_call_output_item_json_manual(call_id, r);
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

	// Per the function calling guide, follow-ups append tool outputs directly
	// to the running input list (not wrapped as message content items).
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

static char *build_initial_request_json(const char *model,
				      const char *input_text,
				      const char *system_text,
				      const char *previous_response_id,
				      const char *tools_json,
				      const char *tool_choice)
{
	if (!model || !model[0] || !input_text || !input_text[0])
		return NULL;

	yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
	yyjson_mut_val *root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);

	yyjson_mut_obj_add_str(doc, root, "model", model);
	if (previous_response_id && previous_response_id[0])
		yyjson_mut_obj_add_str(doc, root, "previous_response_id", previous_response_id);

	// input: single text item
	yyjson_mut_val *input = yyjson_mut_arr(doc);
	yyjson_mut_obj_add_val(doc, root, "input", input);
	{
		yyjson_mut_val *msg = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, msg, "role", "user");
		yyjson_mut_val *content = yyjson_mut_arr(doc);
		yyjson_mut_obj_add_val(doc, msg, "content", content);
		yyjson_mut_val *c0 = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, c0, "type", "input_text");
		yyjson_mut_obj_add_str(doc, c0, "text", input_text);
		yyjson_mut_arr_add_val(content, c0);
		yyjson_mut_arr_add_val(input, msg);
	}

	if (system_text && system_text[0])
		yyjson_mut_obj_add_str(doc, root, "instructions", system_text);

	if (tool_choice && tool_choice[0]) {
		// Existing CLI uses tool_choice values like none/auto/required or a tool name.
		yyjson_mut_obj_add_str(doc, root, "tool_choice", tool_choice);
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
							   const char *previous_response_id,
							   size_t max_turns,
							   size_t max_tool_calls_per_turn,
							   size_t tool_threads,
			       const char *tool_choice,
						   char **out_final_text,
						   char **out_final_response_json)
{
	if (out_final_text)
		*out_final_text = NULL;
	if (out_final_response_json)
		*out_final_response_json = NULL;
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

	// Shared in-memory paging cache for tools (execute/web_search/web_fetch).
	// Kept per-run (process memory only).
	aicli_paging_cache_t *tool_cache = aicli_paging_cache_create(64);

	// URL allowlist for web_fetch (prefix-based). Default: disabled unless explicitly set.
	// Prefer env var for secrets/config.
	const char *web_fetch_prefixes_env = getenv("AICLI_WEB_FETCH_PREFIXES");
	const char *web_fetch_prefixes[32];
	size_t web_fetch_prefix_count = 0;
	char *web_fetch_prefixes_buf = NULL;
	if (web_fetch_prefixes_env && web_fetch_prefixes_env[0]) {
		web_fetch_prefixes_buf = dup_cstr(web_fetch_prefixes_env);
		if (web_fetch_prefixes_buf) {
			char *p = web_fetch_prefixes_buf;
			while (p && *p) {
				while (*p == ' ' || *p == '\t' || *p == ',')
					p++;
				if (!*p)
					break;
				if (web_fetch_prefix_count >= (sizeof(web_fetch_prefixes) / sizeof(web_fetch_prefixes[0])))
					break;
				web_fetch_prefixes[web_fetch_prefix_count++] = p;
				char *comma = strchr(p, ',');
				if (!comma)
					break;
				*comma = '\0';
				p = comma + 1;
			}
		}
	}
	if (cfg && cfg->debug_api >= 3) {
		size_t maxb = debug_max_bytes_for_level(cfg->debug_api);
		debug_print_trunc(stderr, "[debug:api] tools_json", tools_json, maxb);
	}

	const char *model = (cfg->model && cfg->model[0]) ? cfg->model : "gpt-5-mini";

	aicli_openai_http_response_t http = {0};
	if (cfg && debug_level_enabled(cfg->debug_api)) {
		fprintf(stderr, "[debug:api] POST /v1/responses model=%s tool_choice=%s tools=execute\n",
		        safe_str(model), safe_str(tool_choice));
	}
	int rc = 0;
	if (previous_response_id && previous_response_id[0]) {
		char *payload = build_initial_request_json(model, user_prompt, NULL,
		                                        previous_response_id, tools_json, tool_choice);
		if (!payload) {
			free(tools_json);
			free(web_fetch_prefixes_buf);
			aicli_paging_cache_destroy(tool_cache);
			return 2;
		}
		rc = aicli_openai_responses_post_raw_json(cfg->openai_api_key, cfg->openai_base_url,
		                                       payload, &http);
		free(payload);
	} else {
		aicli_openai_request_t req0 = {
		    .model = model,
		    .input_text = user_prompt,
		    .system_text = NULL,
		};
		rc = aicli_openai_responses_post(cfg->openai_api_key, cfg->openai_base_url,
		                               &req0, tools_json, tool_choice, &http);
	}
	if (rc != 0) {
		free(tools_json);
		free(web_fetch_prefixes_buf);
		aicli_paging_cache_destroy(tool_cache);
		return rc;
	}
	if (cfg && debug_level_enabled(cfg->debug_api))
		fprintf(stderr, "[debug:api] response http_status=%d body_len=%zu\n", http.http_status, http.body_len);
	if (cfg && cfg->debug_api >= 3 && http.body && http.body_len) {
		size_t maxb = debug_max_bytes_for_level(cfg->debug_api);
		if (maxb == 0)
			maxb = 4096;
		debug_print_trunc(stderr, "[debug:api] response body", http.body, maxb);
	}
	if (http.http_status != 200 || !http.body || http.body_len == 0) {
		fprintf(stderr, "openai http_status=%d\n", http.http_status);
		if (http.body && http.body_len) {
			size_t n = http.body_len;
			size_t maxb = debug_max_bytes_for_level(cfg ? cfg->debug_api : 0);
			if (maxb == 0)
				maxb = 2048;
			if (n > maxb)
				n = maxb;
			fwrite(http.body, 1, n, stderr);
			fputc('\n', stderr);
			if (n < http.body_len)
				fprintf(stderr, "... (truncated, %zu bytes total)\n", http.body_len);
		}
		aicli_openai_http_response_free(&http);
		free(tools_json);
		return 2;
	}

	// Fail fast on invalid tool arguments.

	for (size_t turn = 0; turn < max_turns; turn++) {
		yyjson_doc *doc = yyjson_read(http.body, http.body_len, 0);
		if (!doc)
			break;
		yyjson_val *root = yyjson_doc_get_root(doc);
		debug_log_execute_calls_if_enabled(cfg, root);
		debug_warn_invalid_execute_calls(cfg, root);

		char *final = extract_first_output_text(root);
		if (final) {
			if (out_final_response_json) {
				*out_final_response_json = dup_cstr(http.body);
			}
			yyjson_doc_free(doc);
			if (out_final_text)
				*out_final_text = final;
			aicli_openai_http_response_free(&http);
			free(tools_json);
			free(web_fetch_prefixes_buf);
			aicli_paging_cache_destroy(tool_cache);
			return 0;
		}

		const char *resp_id = extract_response_id(root);
		if (!resp_id) {
			yyjson_doc_free(doc);
			break;
		}

		// Keep the latest response JSON so callers can persist response_id (e.g. --continue=next).
		if (out_final_response_json) {
			free(*out_final_response_json);
			*out_final_response_json = dup_cstr(http.body);
		}

		exec_job_t *jobs = (exec_job_t *)calloc(max_tool_calls_per_turn, sizeof(exec_job_t));
		list_job_t *ljobs = (list_job_t *)calloc(max_tool_calls_per_turn, sizeof(list_job_t));
		web_search_job_t *sjobs = (web_search_job_t *)calloc(max_tool_calls_per_turn, sizeof(web_search_job_t));
		web_fetch_job_t *fjobs = (web_fetch_job_t *)calloc(max_tool_calls_per_turn, sizeof(web_fetch_job_t));
		cli_help_job_t *hjobs = (cli_help_job_t *)calloc(max_tool_calls_per_turn, sizeof(cli_help_job_t));
		char **call_ids = (char **)calloc(max_tool_calls_per_turn, sizeof(char *));
		char **items_json = (char **)calloc(max_tool_calls_per_turn, sizeof(char *));
		if (!jobs || !ljobs || !sjobs || !fjobs || !hjobs || !call_ids || !items_json) {
			free(jobs);
			free(ljobs);
			free(sjobs);
			free(fjobs);
			free(hjobs);
			free(call_ids);
			free(items_json);
			yyjson_doc_free(doc);
			break;
		}

		size_t exec_count = 0;
		(void)collect_execute_calls(root, jobs, (const char **)call_ids, max_tool_calls_per_turn, &exec_count);

		size_t list_count = 0;
		size_t web_search_count = 0;
		size_t web_fetch_count = 0;
		size_t cli_help_count = 0;
		{
			yyjson_val *outarr = find_output_array(root);
			if (outarr) {
				size_t idx, max = yyjson_arr_size(outarr);
				for (idx = 0; idx < max && (exec_count + list_count + web_search_count + web_fetch_count + cli_help_count) < max_tool_calls_per_turn; idx++) {
					yyjson_val *item = yyjson_arr_get(outarr, idx);
					if (!item || !yyjson_is_obj(item))
						continue;
					yyjson_val *type = yyjson_obj_get(item, "type");
					const char *t = (type && yyjson_is_str(type)) ? yyjson_get_str(type) : NULL;
					if (!t || strcmp(t, "function_call") != 0)
						continue;
					yyjson_val *name = yyjson_obj_get(item, "name");
					const char *nstr = (name && yyjson_is_str(name)) ? yyjson_get_str(name) : NULL;
					yyjson_val *call_id = yyjson_obj_get(item, "call_id");
					const char *cid = (call_id && yyjson_is_str(call_id)) ? yyjson_get_str(call_id) : NULL;
					if (!cid || !cid[0])
						continue;
					yyjson_val *args = yyjson_obj_get(item, "arguments");

					if (nstr && strcmp(nstr, "list_allowed_files") == 0) {

					ljobs[list_count].allow = allow;
					ljobs[list_count].done = false;
					ljobs[list_count].req = (aicli_list_allowed_files_request_t){0};
					(void)parse_list_allowed_files_arguments(args, &ljobs[list_count].req);
					(void)dup_list_request_strings(&ljobs[list_count].req);

						call_ids[exec_count + list_count] = dup_cstr(cid);
					list_count++;
						continue;
					}

					if (nstr && strcmp(nstr, "web_search") == 0) {
						sjobs[web_search_count].cfg = cfg;
						sjobs[web_search_count].cache = tool_cache;
						sjobs[web_search_count].done = false;
						sjobs[web_search_count].req = (aicli_web_search_tool_request_t){0};
						if (parse_web_search_arguments(args, &sjobs[web_search_count].req) == 0 &&
						    dup_web_search_request_strings(&sjobs[web_search_count].req) == 0) {
							call_ids[exec_count + list_count + web_search_count] = dup_cstr(cid);
							web_search_count++;
						} else {
							free_web_search_request_owned(&sjobs[web_search_count].req);
						}
						continue;
					}

					if (nstr && strcmp(nstr, "web_fetch") == 0) {
						fjobs[web_fetch_count].cfg = cfg;
						fjobs[web_fetch_count].cache = tool_cache;
						fjobs[web_fetch_count].done = false;
						fjobs[web_fetch_count].req = (aicli_web_fetch_tool_request_t){0};
						if (parse_web_fetch_arguments(args, &fjobs[web_fetch_count].req) == 0 &&
						    dup_web_fetch_request_strings(&fjobs[web_fetch_count].req) == 0) {
							// apply prefix allowlist from env
							fjobs[web_fetch_count].req.allowed_prefixes = web_fetch_prefixes;
							fjobs[web_fetch_count].req.allowed_prefix_count = web_fetch_prefix_count;
							fjobs[web_fetch_count].req.max_body_bytes = 1024 * 1024;
							fjobs[web_fetch_count].req.timeout_seconds = 15L;
							fjobs[web_fetch_count].req.connect_timeout_seconds = 10L;
							fjobs[web_fetch_count].req.max_redirects = 0;

							call_ids[exec_count + list_count + web_search_count + web_fetch_count] = dup_cstr(cid);
							web_fetch_count++;
						} else {
							free_web_fetch_request_owned(&fjobs[web_fetch_count].req);
						}
						continue;
					}

					if (nstr && strcmp(nstr, "cli_help") == 0) {
						hjobs[cli_help_count].done = false;
						hjobs[cli_help_count].topic = NULL;
						hjobs[cli_help_count].start = 0;
						hjobs[cli_help_count].size = 0;
						(void)parse_cli_help_arguments(args, &hjobs[cli_help_count].topic,
						                             &hjobs[cli_help_count].start,
						                             &hjobs[cli_help_count].size);
						call_ids[exec_count + list_count + web_search_count + web_fetch_count + cli_help_count] = dup_cstr(cid);
						cli_help_count++;
						continue;
					}
				}
			}
		}

		size_t call_count = exec_count + list_count + web_search_count + web_fetch_count + cli_help_count;
		if (call_count == 0) {
			const char *bad_call_id = find_first_execute_call_id(root);
			if (bad_call_id && bad_call_id[0]) {
				fprintf(stderr,
				        "openai tool call invalid: execute arguments missing required 'command' (call_id=%s)\n",
				        safe_str(bad_call_id));
			}
			free(jobs);
			free(ljobs);
			free(sjobs);
			free(fjobs);
			free(hjobs);
			for (size_t k = 0; k < max_tool_calls_per_turn; k++)
				free(call_ids[k]);
			free(call_ids);
			free(items_json);
			yyjson_doc_free(doc);
			aicli_openai_http_response_free(&http);
			free(tools_json);
			free(web_fetch_prefixes_buf);
			aicli_paging_cache_destroy(tool_cache);
			return 2;
		}

		aicli_threadpool_t *tp = aicli_threadpool_create(tool_threads);
		if (!tp) {
			free(jobs);
			free(call_ids);
			free(items_json);
			yyjson_doc_free(doc);
			break;
		}

		for (size_t i = 0; i < exec_count; i++) {
			jobs[i].allow = allow;
			(void)aicli_threadpool_submit(tp, exec_job_main, &jobs[i]);
		}
		for (size_t i = 0; i < list_count; i++) {
			(void)aicli_threadpool_submit(tp, list_job_main, &ljobs[i]);
		}
		for (size_t i = 0; i < web_search_count; i++) {
			(void)aicli_threadpool_submit(tp, web_search_job_main, &sjobs[i]);
		}
		for (size_t i = 0; i < web_fetch_count; i++) {
			(void)aicli_threadpool_submit(tp, web_fetch_job_main, &fjobs[i]);
		}
		for (size_t i = 0; i < cli_help_count; i++) {
			(void)aicli_threadpool_submit(tp, cli_help_job_main, &hjobs[i]);
		}
		aicli_threadpool_drain(tp);
		aicli_threadpool_destroy(tp);

		if (cfg && debug_level_enabled(cfg->debug_function_call) && cfg->debug_function_call >= 2) {
			size_t maxb = debug_max_bytes_for_level(cfg->debug_function_call);
			for (size_t i = 0; i < call_count; i++) {
				fprintf(stderr, "[debug:function_call] execute result call_id=%s exit_code=%d truncated=%d total_bytes=%zu\n",
				        safe_str(call_ids[i]), jobs[i].res.exit_code, jobs[i].res.truncated ? 1 : 0,
				        jobs[i].res.total_bytes);
				if (jobs[i].res.stderr_text && jobs[i].res.stderr_text[0])
					debug_print_trunc(stderr, "[debug:function_call] execute stderr", jobs[i].res.stderr_text, maxb);
				if (cfg->debug_function_call >= 3 && jobs[i].res.stdout_text && jobs[i].res.stdout_text[0])
					debug_print_trunc(stderr, "[debug:function_call] execute stdout", jobs[i].res.stdout_text, maxb);
			}
		}

		for (size_t i = 0; i < exec_count; i++) {
			items_json[i] = build_function_call_output_item_json(call_ids[i], &jobs[i].res);
		}
		for (size_t i = 0; i < list_count; i++) {
			items_json[exec_count + i] = build_function_call_output_item_json_raw(call_ids[exec_count + i],
			                                                                   ljobs[i].res.json);
		}
		for (size_t i = 0; i < web_search_count; i++) {
			items_json[exec_count + list_count + i] =
			    build_function_call_output_item_json(call_ids[exec_count + list_count + i], &sjobs[i].res);
		}
		for (size_t i = 0; i < web_fetch_count; i++) {
			items_json[exec_count + list_count + web_search_count + i] =
			    build_function_call_output_item_json(call_ids[exec_count + list_count + web_search_count + i],
			                                        &fjobs[i].res);
		}
		for (size_t i = 0; i < cli_help_count; i++) {
			items_json[exec_count + list_count + web_search_count + web_fetch_count + i] =
			    build_function_call_output_item_json(
			        call_ids[exec_count + list_count + web_search_count + web_fetch_count + i],
			        &hjobs[i].res);
		}
		for (size_t i = 0; i < call_count; i++) {
			if (!items_json[i] || !items_json[i][0]) {
				fprintf(stderr,
				        "openai tool call failed: could not serialize tool output (call_id=%s)\n",
				        safe_str(call_ids[i]));
				for (size_t k = 0; k < call_count; k++) {
					if (k < exec_count) {
						if (jobs[k].res.stdout_text)
							free((void *)jobs[k].res.stdout_text);
						free_execute_request_owned(&jobs[k].req);
					}
					if (k >= exec_count && k < (exec_count + list_count)) {
						size_t li = k - exec_count;
						aicli_list_allowed_files_result_free(&ljobs[li].res);
						free_list_request_owned(&ljobs[li].req);
					}
					free(items_json[k]);
					free(call_ids[k]);
				}
				free(items_json);
				free(jobs);
				free(ljobs);
				free(sjobs);
				free(fjobs);
				free(call_ids);
				yyjson_doc_free(doc);
				aicli_openai_http_response_free(&http);
				free(tools_json);
				free(web_fetch_prefixes_buf);
				aicli_paging_cache_destroy(tool_cache);
				return 2;
			}
		}
		if (cfg && cfg->debug_api >= 3) {
			for (size_t i = 0; i < call_count; i++) {
				if (!items_json[i] || !items_json[i][0]) {
					fprintf(stderr, "[debug:api] WARN: tool output item NULL/empty for call_id=%s\n", safe_str(call_ids[i]));
				} else {
					size_t maxb = debug_max_bytes_for_level(cfg->debug_api);
					if (maxb == 0)
						maxb = 4096;
					debug_print_trunc(stderr, "[debug:api] tool output item", items_json[i], maxb);
				}
			}
		}

		char *next_payload = build_next_request_json(model, resp_id, tools_json,
		                                           (const char **)items_json, call_count);
		for (size_t i = 0; i < exec_count; i++) {
			if (jobs[i].res.stdout_text)
				free((void *)jobs[i].res.stdout_text);
			free_execute_request_owned(&jobs[i].req);
		}
		for (size_t i = 0; i < list_count; i++) {
			aicli_list_allowed_files_result_free(&ljobs[i].res);
			free_list_request_owned(&ljobs[i].req);
		}
		for (size_t i = 0; i < web_search_count; i++) {
			if (sjobs[i].res.stdout_text)
				free((void *)sjobs[i].res.stdout_text);
			free_web_search_request_owned(&sjobs[i].req);
		}
		for (size_t i = 0; i < web_fetch_count; i++) {
			if (fjobs[i].res.stdout_text)
				free((void *)fjobs[i].res.stdout_text);
			free_web_fetch_request_owned(&fjobs[i].req);
		}
		for (size_t i = 0; i < cli_help_count; i++) {
			if (hjobs[i].res.stdout_text)
				free((void *)hjobs[i].res.stdout_text);
			free(hjobs[i].topic);
		}
		for (size_t i = 0; i < call_count; i++) {
			free(items_json[i]);
			free(call_ids[i]);
		}
		free(items_json);
		free(jobs);
		free(call_ids);
		free(hjobs);
		free(ljobs);
		free(sjobs);
		free(fjobs);
		yyjson_doc_free(doc);

		if (!next_payload)
			break;

		aicli_openai_http_response_free(&http);
		memset(&http, 0, sizeof(http));
		if (cfg && cfg->debug_api >= 3) {
			size_t maxb = debug_max_bytes_for_level(cfg->debug_api);
			if (maxb == 0)
				maxb = 4096;
			debug_print_trunc(stderr, "[debug:api] follow-up payload", next_payload, maxb);
		}
		rc = aicli_openai_responses_post_raw_json(cfg->openai_api_key, cfg->openai_base_url,
		                                       next_payload, &http);
		free(next_payload);
		if (rc != 0) {
			if (cfg && debug_level_enabled(cfg->debug_api)) {
				fprintf(stderr, "[debug:api] follow-up request failed rc=%d http_status=%d body_len=%zu\n",
				        rc, http.http_status, http.body_len);
				if (http.body && http.body_len) {
					size_t maxb = debug_max_bytes_for_level(cfg->debug_api);
					if (maxb == 0)
						maxb = 2048;
					debug_print_trunc(stderr, "[debug:api] follow-up body", http.body, maxb);
				}
			}
			break;
		}
		if (cfg && debug_level_enabled(cfg->debug_api))
			fprintf(stderr, "[debug:api] follow-up response http_status=%d body_len=%zu\n", http.http_status, http.body_len);
		if (http.http_status != 200 || !http.body || http.body_len == 0) {
			fprintf(stderr, "openai http_status=%d\n", http.http_status);
			if (http.body && http.body_len) {
				size_t n = http.body_len;
				size_t maxb = debug_max_bytes_for_level(cfg ? cfg->debug_api : 0);
				if (maxb == 0)
					maxb = 2048;
				if (n > maxb)
					n = maxb;
				fwrite(http.body, 1, n, stderr);
				fputc('\n', stderr);
				if (n < http.body_len)
					fprintf(stderr, "... (truncated, %zu bytes total)\n", http.body_len);
			}
			break;
		}
	}
	aicli_openai_http_response_free(&http);
	free(tools_json);
	free(web_fetch_prefixes_buf);
	aicli_paging_cache_destroy(tool_cache);
	if (out_final_response_json) {
		free(*out_final_response_json);
		*out_final_response_json = NULL;
	}
	return 2;
}

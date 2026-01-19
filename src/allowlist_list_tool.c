#include "allowlist_list_tool.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"

static const char *safe_str(const char *s) { return s ? s : ""; }

static bool is_empty(const char *s) { return !s || !s[0]; }

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

static int contains_case_insensitive(const char *haystack, const char *needle)
{
	if (is_empty(needle))
		return 1;
	if (!haystack)
		return 0;

	size_t hn = strlen(haystack);
	size_t nn = strlen(needle);
	if (nn == 0)
		return 1;
	if (nn > hn)
		return 0;

	for (size_t i = 0; i + nn <= hn; i++) {
		size_t j = 0;
		for (; j < nn; j++) {
			unsigned char a = (unsigned char)haystack[i + j];
			unsigned char b = (unsigned char)needle[j];
			if (tolower(a) != tolower(b))
				break;
		}
		if (j == nn)
			return 1;
	}
	return 0;
}

void aicli_list_allowed_files_result_free(aicli_list_allowed_files_result_t *r)
{
	if (!r)
		return;
	free((void *)r->json);
	r->json = NULL;
}

int aicli_list_allowed_files_json(const aicli_allowlist_t *allow,
                                 const aicli_list_allowed_files_request_t *req,
                                 aicli_list_allowed_files_result_t *out)
{
	if (!out)
		return 2;
	memset(out, 0, sizeof(*out));

	const char *query = (req && req->query) ? req->query : "";
	size_t start = (req) ? req->start : 0;
	size_t size = (req && req->size) ? req->size : 50;
	if (size > 200)
		size = 200;

	int total_match = 0;
	if (allow && allow->files && allow->file_count > 0) {
		for (int i = 0; i < allow->file_count; i++) {
			if (contains_case_insensitive(allow->files[i].path, query))
				total_match++;
		}
	}

	aicli_buf_t b;
	if (!aicli_buf_init(&b, 2048))
		return 2;

	bool ok = true;
	ok = ok && aicli_buf_append_str(&b, "{\"ok\":true");

	char tmp[128];
	snprintf(tmp, sizeof(tmp), ",\"total\":%d", total_match);
	ok = ok && aicli_buf_append_str(&b, tmp);
	snprintf(tmp, sizeof(tmp), ",\"start\":%zu", start);
	ok = ok && aicli_buf_append_str(&b, tmp);
	snprintf(tmp, sizeof(tmp), ",\"size\":%zu", size);
	ok = ok && aicli_buf_append_str(&b, tmp);

	ok = ok && aicli_buf_append_str(&b, ",\"query\":\"");
	ok = ok && buf_append_json_string_escaped_cstr(&b, query);
	ok = ok && aicli_buf_append_str(&b, "\"");

	ok = ok && aicli_buf_append_str(&b, ",\"files\":[");

	size_t idx = 0;
	size_t returned = 0;
	bool first = true;
	if (allow && allow->files && allow->file_count > 0) {
		for (int i = 0; i < allow->file_count; i++) {
			const aicli_allowed_file_t *f = &allow->files[i];
			if (!contains_case_insensitive(f->path, query))
				continue;

			if (idx++ < start)
				continue;
			if (returned >= size)
				break;

			if (!first)
				ok = ok && aicli_buf_append_str(&b, ",");
			first = false;

			ok = ok && aicli_buf_append_str(&b, "{\"path\":\"");
			ok = ok && buf_append_json_string_escaped_cstr(&b, safe_str(f->path));
			ok = ok && aicli_buf_append_str(&b, "\",\"name\":\"");
			ok = ok && buf_append_json_string_escaped_cstr(&b, safe_str(f->name));
			char t2[128];
			snprintf(t2, sizeof(t2), "\",\"size_bytes\":%zu}", f->size_bytes);
			ok = ok && aicli_buf_append_str(&b, t2);

			returned++;
		}
	}

	ok = ok && aicli_buf_append_str(&b, "]");

	snprintf(tmp, sizeof(tmp), ",\"returned\":%zu", returned);
	ok = ok && aicli_buf_append_str(&b, tmp);

	bool has_next = false;
	if (start + returned < (size_t)total_match)
		has_next = true;
	ok = ok && aicli_buf_append_str(&b, ",\"has_next\":");
	ok = ok && aicli_buf_append_str(&b, has_next ? "true" : "false");
	ok = ok && aicli_buf_append_str(&b, ",\"next_start\":");
	if (has_next) {
		snprintf(tmp, sizeof(tmp), "%zu", start + returned);
		ok = ok && aicli_buf_append_str(&b, tmp);
	} else {
		ok = ok && aicli_buf_append_str(&b, "null");
	}

	ok = ok && aicli_buf_append_str(&b, "}");

	if (!ok) {
		aicli_buf_free(&b);
		return 2;
	}

	if (!aicli_buf_append(&b, "\0", 1)) {
		aicli_buf_free(&b);
		return 2;
	}

	out->json = b.data;
	return 0;
}

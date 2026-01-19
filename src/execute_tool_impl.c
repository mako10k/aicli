#include "execute_tool.h"

#include "buf.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool allowlist_contains(const aicli_allowlist_t *allow, const char *path) {
	if (!allow || !path) return false;
	for (int i = 0; i < allow->file_count; i++) {
		if (strcmp(allow->files[i].path, path) == 0) return true;
	}
	return false;
}

static int read_file_range(const char *path, size_t start, size_t max_bytes,
				   char **out_buf, size_t *out_len, size_t *out_total) {
	FILE *fp = fopen(path, "rb");
	if (!fp) return -1;

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	long total = ftell(fp);
	if (total < 0) {
		fclose(fp);
		return -1;
	}
	*out_total = (size_t)total;

	if ((long)start > total) start = (size_t)total;
	if (fseek(fp, (long)start, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}

	size_t to_read = max_bytes;
	if (start + to_read > (size_t)total) to_read = (size_t)total - start;

	char *buf = (char *)malloc(to_read + 1);
	if (!buf) {
		fclose(fp);
		return -1;
	}
	size_t n = fread(buf, 1, to_read, fp);
	buf[n] = '\0';
	fclose(fp);

	*out_buf = buf;
	*out_len = n;
	return 0;
}

static bool stage_nl(const char *in, size_t in_len, aicli_buf_t *out) {
	// Simple line numbering: "     1\t..."
	unsigned long line = 1;
	size_t i = 0;
	size_t line_start = 0;
	while (i <= in_len) {
		if (i == in_len || in[i] == '\n') {
			char prefix[32];
			int n = snprintf(prefix, sizeof(prefix), "%6lu\t", line);
			if (n < 0) return false;
			if (!aicli_buf_append(out, prefix, (size_t)n)) return false;
			if (!aicli_buf_append(out, in + line_start, i - line_start)) return false;
			if (i < in_len) {
				if (!aicli_buf_append(out, "\n", 1)) return false;
			}
			line++;
			line_start = i + 1;
		}
		i++;
	}
	return true;
}

static bool stage_head(const char *in, size_t in_len, size_t nlines, aicli_buf_t *out) {
	if (nlines == 0) return true;
	size_t lines = 0;
	size_t i = 0;
	while (i < in_len) {
		if (!aicli_buf_append(out, &in[i], 1)) return false;
		if (in[i] == '\n') {
			lines++;
			if (lines >= nlines) break;
		}
		i++;
	}
	return true;
}

static bool stage_tail(const char *in, size_t in_len, size_t nlines, aicli_buf_t *out) {
	if (nlines == 0) return true;
	// Find start position of the last N lines.
	size_t lines = 0;
	for (size_t i = in_len; i > 0; i--) {
		if (in[i - 1] == '\n') {
			lines++;
			if (lines == nlines + 1) {
				// start after this newline
				size_t start = i;
				return aicli_buf_append(out, in + start, in_len - start);
			}
		}
	}
	// Not enough newlines: return whole input
	return aicli_buf_append(out, in, in_len);
}

static bool stage_wc(const char *in, size_t in_len, char mode, aicli_buf_t *out) {
	// mode: 'l' or 'c'
	unsigned long long v = 0;
	if (mode == 'c') {
		v = (unsigned long long)in_len;
	} else if (mode == 'l') {
		for (size_t i = 0; i < in_len; i++) if (in[i] == '\n') v++;
	} else {
		return false;
	}
	char buf[64];
	int n = snprintf(buf, sizeof(buf), "%llu\n", v);
	if (n < 0) return false;
	return aicli_buf_append(out, buf, (size_t)n);
}

typedef struct aicli_line_view {
	const char *s;
	size_t len;
} aicli_line_view_t;

static int cmp_line_asc(const void *a, const void *b) {
	const aicli_line_view_t *la = (const aicli_line_view_t *)a;
	const aicli_line_view_t *lb = (const aicli_line_view_t *)b;
	size_t min = la->len < lb->len ? la->len : lb->len;
	int c = memcmp(la->s, lb->s, min);
	if (c != 0) return c;
	if (la->len < lb->len) return -1;
	if (la->len > lb->len) return 1;
	return 0;
}

static int cmp_line_desc(const void *a, const void *b) {
	return -cmp_line_asc(a, b);
}

static bool stage_sort_lines(const char *in, size_t in_len, bool reverse, aicli_buf_t *out) {
	// Split into line views, sort lexicographically, join with '\n'.
	// Always emits a trailing '\n' when input has at least one line.
	if (in_len == 0) return true;

	size_t line_count = 0;
	for (size_t i = 0; i < in_len; i++) if (in[i] == '\n') line_count++;
	if (in[in_len - 1] != '\n') line_count++;
	if (line_count == 0) return true;

	aicli_line_view_t *lines = (aicli_line_view_t *)calloc(line_count, sizeof(aicli_line_view_t));
	if (!lines) return false;

	size_t idx = 0;
	size_t start = 0;
	for (size_t i = 0; i <= in_len; i++) {
		if (i == in_len || in[i] == '\n') {
			if (idx < line_count) {
				lines[idx].s = in + start;
				lines[idx].len = i - start;
				idx++;
			}
			start = i + 1;
		}
	}

	qsort(lines, line_count, sizeof(aicli_line_view_t), reverse ? cmp_line_desc : cmp_line_asc);

	for (size_t i = 0; i < line_count; i++) {
		if (lines[i].len > 0) {
			if (!aicli_buf_append(out, lines[i].s, lines[i].len)) {
				free(lines);
				return false;
			}
		}
		if (!aicli_buf_append(out, "\n", 1)) {
			free(lines);
			return false;
		}
	}

	free(lines);
	return true;
}

static bool stage_grep_fixed(const char *in, size_t in_len, const char *needle, bool with_line_numbers,
			     aicli_buf_t *out) {
	if (!needle || needle[0] == '\0') return true;
	const size_t needle_len = strlen(needle);

	unsigned long line_no = 1;
	size_t i = 0;
	size_t line_start = 0;
	while (i <= in_len) {
		if (i == in_len || in[i] == '\n') {
			size_t line_len = i - line_start;
			const char *line = in + line_start;

			bool match = false;
			if (needle_len <= line_len) {
				for (size_t off = 0; off + needle_len <= line_len; off++) {
					if (memcmp(line + off, needle, needle_len) == 0) {
						match = true;
						break;
					}
				}
			}

			if (match) {
				if (with_line_numbers) {
					char prefix[32];
					int n = snprintf(prefix, sizeof(prefix), "%lu:", line_no);
					if (n < 0) return false;
					if (!aicli_buf_append(out, prefix, (size_t)n)) return false;
				}
				if (line_len > 0) {
					if (!aicli_buf_append(out, line, line_len)) return false;
				}
				if (!aicli_buf_append(out, "\n", 1)) return false;
			}

			line_no++;
			line_start = i + 1;
		}
		i++;
	}
	return true;
}

static size_t parse_head_n(const aicli_dsl_stage_t *st, bool *ok) {
	*ok = true;
	// head -n N
	if (st->argc == 1) return 10;
	if (st->argc == 3 && strcmp(st->argv[1], "-n") == 0) {
		char *end = NULL;
		unsigned long v = strtoul(st->argv[2], &end, 10);
		if (!end || *end != '\0') { *ok = false; return 0; }
		return (size_t)v;
	}
	*ok = false;
	return 0;
}

static size_t parse_tail_n(const aicli_dsl_stage_t *st, bool *ok) {
	*ok = true;
	// tail -n N
	if (st->argc == 1) return 10;
	if (st->argc == 3 && strcmp(st->argv[1], "-n") == 0) {
		char *end = NULL;
		unsigned long v = strtoul(st->argv[2], &end, 10);
		if (!end || *end != '\0') { *ok = false; return 0; }
		return (size_t)v;
	}
	*ok = false;
	return 0;
}

static bool parse_wc_mode(const aicli_dsl_stage_t *st, char *out_mode) {
	// wc -l | wc -c
	if (st->argc != 2) return false;
	if (strcmp(st->argv[1], "-l") == 0) { *out_mode = 'l'; return true; }
	if (strcmp(st->argv[1], "-c") == 0) { *out_mode = 'c'; return true; }
	return false;
}

static bool parse_sort_reverse(const aicli_dsl_stage_t *st, bool *out_reverse) {
	// sort | sort -r
	if (st->argc == 1) {
		*out_reverse = false;
		return true;
	}
	if (st->argc == 2 && strcmp(st->argv[1], "-r") == 0) {
		*out_reverse = true;
		return true;
	}
	return false;
}

static bool parse_grep_args(const aicli_dsl_stage_t *st, const char **out_pattern, bool *out_n) {
	// grep PATTERN | grep -n PATTERN
	if (st->argc == 2) {
		*out_n = false;
		*out_pattern = st->argv[1];
		return true;
	}
	if (st->argc == 3 && strcmp(st->argv[1], "-n") == 0) {
		*out_n = true;
		*out_pattern = st->argv[2];
		return true;
	}
	return false;
}

static void apply_paging(const char *data, size_t total, size_t start, size_t size,
				 aicli_tool_result_t *out) {
	if (start > total) start = total;
	size_t remain = total - start;
	size_t n = remain < size ? remain : size;

	char *buf = (char *)malloc(n + 1);
	if (!buf) {
		out->stderr_text = "oom";
		out->exit_code = 1;
		return;
	}
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

int aicli_execute_run(const aicli_allowlist_t *allow,
				 const aicli_execute_request_t *req,
				 aicli_tool_result_t *out) {
	if (!req || !out) return -1;
	memset(out, 0, sizeof(*out));

	size_t size = req->size ? req->size : AICLI_MAX_TOOL_BYTES;
	if (size > AICLI_MAX_TOOL_BYTES) size = AICLI_MAX_TOOL_BYTES;

	aicli_dsl_pipeline_t pipe;
	aicli_dsl_status_t st = aicli_dsl_parse_pipeline(req->command, &pipe);
	if (st != AICLI_DSL_OK) {
		out->stderr_text = aicli_dsl_status_str(st);
		out->exit_code = 2;
		return 0;
	}

	// MVP2: support `cat <FILE> | nl | head -n N` (nl/head optional)
	if (pipe.stage_count < 1 || pipe.stages[0].kind != AICLI_CMD_CAT || pipe.stages[0].argc != 2) {
		out->stderr_text = "mvp_requires: cat <FILE>";
		out->exit_code = 2;
		return 0;
	}

	const char *path = pipe.stages[0].argv[1];
	char *rp = aicli_realpath_dup(path);
	if (!rp) {
		out->stderr_text = "invalid_path";
		out->exit_code = 2;
		return 0;
	}
	if (!allowlist_contains(allow, rp)) {
		free(rp);
		out->stderr_text = "file_not_allowed";
		out->exit_code = 3;
		return 0;
	}

	// Step 1: read whole file (bounded) into memory for now.
	// Limit: avoid reading huge files in MVP.
	size_t file_total = 0;
	char *file_buf = NULL;
	size_t file_len = 0;
	{
		size_t max_read = 1024 * 1024; // 1 MiB MVP limit
		if (read_file_range(rp, 0, max_read, &file_buf, &file_len, &file_total) != 0) {
			free(rp);
			out->stderr_text = strerror(errno);
			out->exit_code = 1;
			return 0;
		}
		free(rp);
		if (file_total > max_read) {
			free(file_buf);
			out->stderr_text = "file_too_large";
			out->exit_code = 4;
			return 0;
		}
	}

	const char *cur = file_buf;
	size_t cur_len = file_len;
	aicli_buf_t tmp1, tmp2;
	bool okbuf = aicli_buf_init(&tmp1, cur_len + 64) && aicli_buf_init(&tmp2, cur_len + 64);
	if (!okbuf) {
		free(file_buf);
		out->stderr_text = "oom";
		out->exit_code = 1;
		return 0;
	}

	for (int si = 1; si < pipe.stage_count; si++) {
		aicli_dsl_stage_t *stg = &pipe.stages[si];
		tmp1.len = 0;
		bool ok = true;
		if (stg->kind == AICLI_CMD_NL) {
			ok = stage_nl(cur, cur_len, &tmp1);
		} else if (stg->kind == AICLI_CMD_HEAD) {
			bool okn = true;
			size_t nlines = parse_head_n(stg, &okn);
			if (!okn) {
				ok = false;
			} else {
				ok = stage_head(cur, cur_len, nlines, &tmp1);
			}
		} else if (stg->kind == AICLI_CMD_TAIL) {
			bool okn = true;
			size_t nlines = parse_tail_n(stg, &okn);
			if (!okn) {
				ok = false;
			} else {
				ok = stage_tail(cur, cur_len, nlines, &tmp1);
			}
		} else if (stg->kind == AICLI_CMD_WC) {
			char mode = 0;
			if (!parse_wc_mode(stg, &mode)) {
				ok = false;
			} else {
				ok = stage_wc(cur, cur_len, mode, &tmp1);
			}
		} else if (stg->kind == AICLI_CMD_SORT) {
			bool reverse = false;
			if (!parse_sort_reverse(stg, &reverse)) {
				ok = false;
			} else {
				ok = stage_sort_lines(cur, cur_len, reverse, &tmp1);
			}
		} else if (stg->kind == AICLI_CMD_GREP) {
			const char *pattern = NULL;
			bool with_n = false;
			if (!parse_grep_args(stg, &pattern, &with_n)) {
				ok = false;
			} else {
				ok = stage_grep_fixed(cur, cur_len, pattern, with_n, &tmp1);
			}
		} else {
			ok = false;
		}

		if (!ok) {
			aicli_buf_free(&tmp1);
			aicli_buf_free(&tmp2);
			free(file_buf);
			out->stderr_text = "mvp_unsupported_stage";
			out->exit_code = 2;
			return 0;
		}

		// swap buffers: tmp1 becomes current
		tmp2.len = 0;
		if (!aicli_buf_append(&tmp2, tmp1.data, tmp1.len)) {
			aicli_buf_free(&tmp1);
			aicli_buf_free(&tmp2);
			free(file_buf);
			out->stderr_text = "oom";
			out->exit_code = 1;
			return 0;
		}
		cur = tmp2.data;
		cur_len = tmp2.len;
	}

	apply_paging(cur, cur_len, req->start, size, out);

	aicli_buf_free(&tmp1);
	aicli_buf_free(&tmp2);
	free(file_buf);
	return 0;
}

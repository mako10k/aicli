#include "execute_tool.h"

#include "buf.h"

#include <errno.h>
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

#include "execute_tool.h"

#include "buf.h"
#include "execute/allowlist.h"
#include "execute/file_reader.h"
#include "execute/paging.h"
#include "execute/pipeline_stages.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef bool (*aicli_stage_apply_fn)(const aicli_dsl_stage_t *stg, const char *in, size_t in_len,
				    aicli_buf_t *out);

typedef struct aicli_stage_dispatch {
	aicli_cmd_kind_t kind;
	aicli_stage_apply_fn apply;
} aicli_stage_dispatch_t;

static bool apply_nl(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	(void)stg;
	return aicli_stage_nl(in, in_len, out);
}

static bool apply_head(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	bool ok = true;
	size_t nlines = aicli_parse_head_n(stg, &ok);
	return ok && aicli_stage_head(in, in_len, nlines, out);
}

static bool apply_tail(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	bool ok = true;
	size_t nlines = aicli_parse_tail_n(stg, &ok);
	return ok && aicli_stage_tail(in, in_len, nlines, out);
}

static bool apply_wc(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	char mode = 0;
	if (!aicli_parse_wc_mode(stg, &mode))
		return false;
	return aicli_stage_wc(in, in_len, mode, out);
}

static bool apply_sort(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	bool reverse = false;
	if (!aicli_parse_sort_reverse(stg, &reverse))
		return false;
	return aicli_stage_sort_lines(in, in_len, reverse, out);
}

static bool apply_grep(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	const char *pattern = NULL;
	bool with_n = false;
	if (!aicli_parse_grep_args(stg, &pattern, &with_n))
		return false;
	return aicli_stage_grep_fixed(in, in_len, pattern, with_n, out);
}

static bool apply_sed(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	size_t start_addr = 0;
	size_t end_addr = 0;
	char cmd = 0;
	if (!aicli_parse_sed_args(stg, &start_addr, &end_addr, &cmd))
		return false;
	return aicli_stage_sed_n_addr(in, in_len, start_addr, end_addr, cmd, out);
}

static const aicli_stage_dispatch_t k_dispatch[] = {
	{ AICLI_CMD_NL, apply_nl },     { AICLI_CMD_HEAD, apply_head },
	{ AICLI_CMD_TAIL, apply_tail }, { AICLI_CMD_WC, apply_wc },
	{ AICLI_CMD_SORT, apply_sort }, { AICLI_CMD_GREP, apply_grep },
	{ AICLI_CMD_SED, apply_sed },
};

static aicli_stage_apply_fn dispatch_find(aicli_cmd_kind_t kind)
{
	for (size_t i = 0; i < (sizeof(k_dispatch) / sizeof(k_dispatch[0])); i++) {
		if (k_dispatch[i].kind == kind)
			return k_dispatch[i].apply;
	}
	return NULL;
}

int aicli_execute_run(const aicli_allowlist_t *allow, const aicli_execute_request_t *req,
                      aicli_tool_result_t *out)
{
	if (!req || !out)
		return -1;
	memset(out, 0, sizeof(*out));

	size_t size = req->size ? req->size : AICLI_MAX_TOOL_BYTES;
	if (size > AICLI_MAX_TOOL_BYTES)
		size = AICLI_MAX_TOOL_BYTES;

	aicli_dsl_pipeline_t pipe;
	aicli_dsl_status_t st = aicli_dsl_parse_pipeline(req->command, &pipe);
	if (st != AICLI_DSL_OK) {
		out->stderr_text = aicli_dsl_status_str(st);
		out->exit_code = 2;
		return 0;
	}

	// MVP2: support `cat <FILE> | nl | head -n N` (nl/head optional)
	if (pipe.stage_count < 1 || pipe.stages[0].kind != AICLI_CMD_CAT ||
	    pipe.stages[0].argc != 2) {
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
	if (!aicli_allowlist_contains(allow, rp)) {
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
				if (aicli_read_file_range(rp, 0, max_read, &file_buf, &file_len, &file_total) !=
					0) {
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
		aicli_stage_apply_fn apply = dispatch_find(stg->kind);
		bool ok = (apply != NULL) && apply(stg, cur, cur_len, &tmp1);

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

	aicli_apply_paging(cur, cur_len, req->start, size, out);

	aicli_buf_free(&tmp1);
	aicli_buf_free(&tmp2);
	free(file_buf);
	return 0;
}

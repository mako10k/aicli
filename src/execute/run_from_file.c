#include "execute/run_from_file.h"

#include "buf.h"
#include "execute/allowlist.h"
#include "execute/dispatch.h"
#include "execute/file_reader.h"
#include "execute/paging.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

int aicli_execute_run_pipeline_from_file(const aicli_allowlist_t *allow,
                                        const aicli_dsl_pipeline_t *pipe,
                                        const aicli_execute_request_t *req,
                                        aicli_tool_result_t *out)
{
	if (!pipe || !req || !out)
		return -1;

	// MVP2: support `cat <FILE> | ...`
	if (pipe->stage_count < 1 || pipe->stages[0].kind != AICLI_CMD_CAT || pipe->stages[0].argc != 2) {
		out->stderr_text = "mvp_requires: cat <FILE>";
		out->exit_code = 2;
		return 0;
	}

	const char *path = pipe->stages[0].argv[1];
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

	size_t size = req->size ? req->size : AICLI_MAX_TOOL_BYTES;
	if (size > AICLI_MAX_TOOL_BYTES)
		size = AICLI_MAX_TOOL_BYTES;

	// Step 1: read whole file (bounded) into memory for now.
	// Limit: avoid reading huge files in MVP.
	size_t file_total = 0;
	char *file_buf = NULL;
	size_t file_len = 0;
	{
		size_t max_read = 1024 * 1024; // 1 MiB MVP limit
		if (aicli_read_file_range(rp, 0, max_read, &file_buf, &file_len, &file_total) != 0) {
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

	for (int si = 1; si < pipe->stage_count; si++) {
		const aicli_dsl_stage_t *stg = &pipe->stages[si];
		tmp1.len = 0;
		bool ok = aicli_execute_apply_stage(stg, cur, cur_len, &tmp1);

		if (!ok) {
			aicli_buf_free(&tmp1);
			aicli_buf_free(&tmp2);
			free(file_buf);
			out->stderr_text = "mvp_unsupported_stage";
			out->exit_code = 2;
			return 0;
		}

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

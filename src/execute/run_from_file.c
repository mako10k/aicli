#include "execute/run_from_file.h"

#include "buf.h"
#include "execute/allowlist.h"
#include "execute/dispatch.h"
#include "execute/file_reader.h"
#include "execute/paging.h"

#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool stage_has_file_arg(aicli_cmd_kind_t kind)
{
	// Commands that can take a source FILE as their last argument.
	// We normalize them into: cat FILE | <cmd ...without FILE>
	switch (kind) {
	case AICLI_CMD_HEAD:
	case AICLI_CMD_TAIL:
	case AICLI_CMD_NL:
	case AICLI_CMD_SED:
		return true;
	default:
		return false;
	}
}

static bool stage_is_file_sed_n_addr(const aicli_dsl_stage_t *st)
{
	// We only support a restricted sed form via pipeline stages:
	//   sed -n '1,200p'   (script token only)
	// When used as a file-input command, models often emit:
	//   sed -n 1,200p FILE
	// Normalize that shape into: cat FILE | sed -n 1,200p
	if (!st)
		return false;
	if (st->kind != AICLI_CMD_SED)
		return false;
	if (st->argc != 4)
		return false;
	if (strcmp(st->argv[1], "-n") != 0)
		return false;
	// argv[2] is the script, argv[3] is the file
	if (!st->argv[2] || !st->argv[2][0])
		return false;
	if (!st->argv[3] || !st->argv[3][0])
		return false;
	return true;
}

static int normalize_file_input_pipeline(aicli_dsl_pipeline_t *pipe)
{
	// Ensures pipelines enter the executor as: cat FILE | ...
	// Returns 0 on success; non-zero means unsupported shape.
	if (!pipe || pipe->stage_count <= 0)
		return -1;

	// Already normalized.
	if (pipe->stages[0].kind == AICLI_CMD_CAT && pipe->stages[0].argc == 2)
		return 0;

	// Also allow a single-stage direct file read: cat FILE
	// (covered above) and reject other single-stage non-file commands.

	// Support: <cmd ... FILE> | ...  ==>  cat FILE | <cmd ...> | ...
	// Special-case: sed -n ADDR FILE (argc==4)
	bool is_sed_file_form = stage_is_file_sed_n_addr(&pipe->stages[0]);
	if (!is_sed_file_form && !stage_has_file_arg(pipe->stages[0].kind))
		return -1;
	if (pipe->stages[0].argc < 2)
		return -1;

	const char *file = NULL;
	if (is_sed_file_form)
		file = pipe->stages[0].argv[3];
	else
		file = pipe->stages[0].argv[pipe->stages[0].argc - 1];
	if (!file || file[0] == '\0')
		return -1;

	// Shift stages right by 1.
	if (pipe->stage_count >= 8)
		return -1;
	for (int i = pipe->stage_count; i > 0; i--) {
		pipe->stages[i] = pipe->stages[i - 1];
	}
	pipe->stage_count++;

	// New stage 0: cat FILE
	pipe->stages[0].kind = AICLI_CMD_CAT;
	pipe->stages[0].argc = 2;
	pipe->stages[0].argv[0] = "cat";
	pipe->stages[0].argv[1] = file;
	for (int i = 2; i < 8; i++)
		pipe->stages[0].argv[i] = NULL;

	// Remove FILE from the original command stage (now stage 1)
	if (is_sed_file_form) {
		// Convert: sed -n ADDR FILE  (argc=4)
		// into:    sed -n ADDR       (argc=3)
		pipe->stages[1].argc = 3;
		pipe->stages[1].argv[3] = NULL;
	} else {
		pipe->stages[1].argc -= 1;
		pipe->stages[1].argv[pipe->stages[1].argc] = NULL;
	}
	return 0;
}

int aicli_execute_run_pipeline_from_file(const aicli_allowlist_t *allow,
                                        const aicli_dsl_pipeline_t *pipe,
                                        const aicli_execute_request_t *req,
                                        aicli_tool_result_t *out)
{
	if (!pipe || !req || !out)
		return -1;

	aicli_dsl_pipeline_t local_pipe = *pipe;
	if (normalize_file_input_pipeline(&local_pipe) != 0) {
		out->stderr_text = "mvp_requires: cat <FILE> (or head/tail/nl/sed ... <FILE>)";
		out->exit_code = 2;
		return 0;
	}

	const char *path = local_pipe.stages[0].argv[1];
	{
		const char *dbg = getenv("AICLI_DEBUG_FUNCTION_CALL");
		if (dbg && dbg[0] != '\0')
			fprintf(stderr, "[debug:allowlist] pipeline file_arg='%s'\n", path ? path : "(null)");
	}
	char *rp = aicli_realpath_dup(path);
	if (!rp) {
		out->stderr_text = "invalid_path";
		out->exit_code = 2;
		return 0;
	}
	if (!aicli_allowlist_contains(allow, rp)) {
		const char *dbg = getenv("AICLI_DEBUG_FUNCTION_CALL");
		if (dbg && dbg[0] != '\0')
			fprintf(stderr, "[debug:allowlist] rejected realpath='%s'\n", rp);
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

	for (int si = 1; si < local_pipe.stage_count; si++) {
		const aicli_dsl_stage_t *stg = &local_pipe.stages[si];
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


		// Next stage should read the freshly-produced buffer. Swap the buffers
		// to avoid tmp1/tmp2 aliasing issues (tmp2 may point to tmp1.data).
		aicli_buf_t swap = tmp2;
		tmp2 = tmp1;
		tmp1 = swap;
		cur = tmp2.data;
		cur_len = tmp2.len;
	}

	aicli_apply_paging(cur, cur_len, req->start, size, out);

	aicli_buf_free(&tmp1);
	aicli_buf_free(&tmp2);
	free(file_buf);
	return 0;
}

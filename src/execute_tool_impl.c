#include "execute_tool.h"

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

	// MVP: only support a single-stage "cat <FILE>".
	if (pipe.stage_count != 1 || pipe.stages[0].kind != AICLI_CMD_CAT || pipe.stages[0].argc != 2) {
		out->stderr_text = "mvp_only_supports: cat <FILE>";
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

	char *buf = NULL;
	size_t len = 0;
	size_t total = 0;
	if (read_file_range(rp, req->start, size, &buf, &len, &total) != 0) {
		free(rp);
		out->stderr_text = strerror(errno);
		out->exit_code = 1;
		return 0;
	}
	free(rp);

	out->stdout_text = buf;
	out->stdout_len = len;
	out->exit_code = 0;
	out->total_bytes = total;
	out->truncated = (req->start + len) < total;
	out->has_next_start = out->truncated;
	out->next_start = req->start + len;
	return 0;
}

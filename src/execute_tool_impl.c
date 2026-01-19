#include "execute_tool.h"

#include "execute/run_from_file.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
		// Keep debug output opt-in via CLI --debug-function-call.
		const char *dbg = getenv("AICLI_DEBUG_FUNCTION_CALL");
		if (dbg && dbg[0] != '\0') {
			fprintf(stderr, "[debug:dsl] parse_status=%s command='%s'\n",
			        aicli_dsl_status_str(st), req->command ? req->command : "(null)");
		}
		out->stderr_text = aicli_dsl_status_str(st);
		out->exit_code = 2;
		return 0;
	}

	return aicli_execute_run_pipeline_from_file(allow, &pipe, req, out);
}

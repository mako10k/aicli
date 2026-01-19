#pragma once

#include "execute_tool.h"

// Executes an already-parsed pipeline that starts with `cat <FILE>`.
// On success, returns 0 and sets `out`.
// On invalid request, returns 0 with an appropriate `out->exit_code`.
// On internal parameter errors, returns -1.
int aicli_execute_run_pipeline_from_file(const aicli_allowlist_t *allow,
                                        const aicli_dsl_pipeline_t *pipe,
                                        const aicli_execute_request_t *req,
                                        aicli_tool_result_t *out);

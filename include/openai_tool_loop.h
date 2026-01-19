#pragma once

#include <stddef.h>

#include "aicli.h"
#include "execute_tool.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runs a single-turn Responses request with tool support.
//
// It will:
//  - POST /v1/responses with tools definition
//  - if the model returns one or more function_call items for "execute",
//    run them (possibly in parallel)
//  - POST /v1/responses again with function_call_output appended
//
// Returns 0 on success (even if the model response is non-200); non-zero on
// transport/setup errors.
int aicli_openai_run_with_tools(const aicli_config_t *cfg,
                               const aicli_allowlist_t *allow,
                               const char *user_prompt,
                               size_t max_turns,
                               size_t max_tool_calls_per_turn,
                               size_t tool_threads,
                               char **out_final_text);

#ifdef __cplusplus
}
#endif

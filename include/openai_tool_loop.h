#pragma once

#include <stddef.h>

#include "aicli.h"
#include "buf.h"
#include "execute_tool.h"

#ifdef __cplusplus
extern "C" {
#endif

// Extract response id (root.id) from a Responses JSON.
// Returns 0 on success; 1 if not found; 2 on parse/args error.
int aicli_openai_extract_response_id(const char *json, size_t json_len,
				      char *out_id, size_t out_cap);

// Runs a multi-turn Responses tool loop.
//
// Returns 0 on success; non-zero on transport/setup errors.
int aicli_openai_run_with_tools(const aicli_config_t *cfg,
				   const aicli_allowlist_t *allow,
				   const char *user_prompt,
				   const char *previous_response_id,
				   size_t max_turns,
				   size_t max_tool_calls_per_turn,
				   size_t tool_threads,
				   const char *tool_choice,
				   char **out_final_text,
				   char **out_final_response_json);

#ifdef __cplusplus
}
#endif

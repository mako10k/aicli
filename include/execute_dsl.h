#pragma once

#include <stddef.h>
#include <stdbool.h>

typedef enum {
	AICLI_DSL_OK = 0,
	AICLI_DSL_ERR_EMPTY,
	AICLI_DSL_ERR_PARSE,
	AICLI_DSL_ERR_FORBIDDEN,
	AICLI_DSL_ERR_TOO_MANY_STAGES,
	AICLI_DSL_ERR_TOO_MANY_ARGS,
} aicli_dsl_status_t;

typedef enum {
	AICLI_CMD_CAT,
	AICLI_CMD_NL,
	AICLI_CMD_HEAD,
	AICLI_CMD_TAIL,
	AICLI_CMD_WC,
	AICLI_CMD_SORT,
	AICLI_CMD_GREP,
	AICLI_CMD_SED,
	AICLI_CMD_UNKNOWN,
} aicli_cmd_kind_t;

typedef struct {
	aicli_cmd_kind_t kind;
	const char *argv[8];
	int argc;
} aicli_dsl_stage_t;

typedef struct {
	aicli_dsl_stage_t stages[8];
	int stage_count;
} aicli_dsl_pipeline_t;

// Parses a restricted pipeline like: "cat FILE | nl | head -n 20"
// No redirects, no subshell, no env-vars, no command substitution.
// Quotes are minimally supported for single/double quoted strings.

aicli_dsl_status_t aicli_dsl_parse_pipeline(const char *command, aicli_dsl_pipeline_t *out);

const char *aicli_dsl_status_str(aicli_dsl_status_t st);

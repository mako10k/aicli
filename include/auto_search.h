#pragma once

#include <stdbool.h>

#include "aicli.h"

// Asks the model whether web search is needed, and if so returns a query.
//
// Returns true if a query was produced (need_search=true and non-empty query).
// Returns false if no search is needed or on any error (caller should continue without search).
//
// The returned query must be freed by the caller.
bool aicli_auto_search_plan(const aicli_config_t *cfg,
			   const char *user_prompt,
			   char **out_query);

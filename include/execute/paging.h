#pragma once

#include <stddef.h>

#include "execute_tool.h"

void aicli_apply_paging(const char *data, size_t total, size_t start, size_t size,
                        aicli_tool_result_t *out);

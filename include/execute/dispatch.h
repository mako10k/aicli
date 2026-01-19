#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "buf.h"
#include "execute_dsl.h"

bool aicli_execute_apply_stage(const aicli_dsl_stage_t *stg, const char *in, size_t in_len,
                              aicli_buf_t *out);

#pragma once

#include <stdbool.h>

#include "execute_tool.h"

bool aicli_allowlist_contains(const aicli_allowlist_t *allow, const char *path);

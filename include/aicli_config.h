#pragma once

#include <stdbool.h>

#include "aicli.h"

#include "aicli_config_file.h"

bool aicli_config_load_from_env(aicli_config_t *out);

void aicli_config_free(aicli_config_t *cfg);

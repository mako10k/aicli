#pragma once

#include "aicli.h"

#include <stdbool.h>

// Configuration file name searched by default.
#define AICLI_DEFAULT_CONFIG_FILENAME ".aicli.json"

typedef struct {
	// Full path to the selected config file, or NULL if none found/used.
	char *path;
	// Directory containing the config file (used for resolving relative paths).
	char *dir;
} aicli_config_file_t;

// Finds the config file according to priority:
//  - current working directory, only if under $HOME
//  - parent directories up to $HOME (inclusive)
//  - $HOME
// Returns true if a config file was found and out is populated.
bool aicli_config_file_find(aicli_config_file_t *out);

void aicli_config_file_free(aicli_config_file_t *cf);

// Returns true if config file is safe to read:
//  - regular file
//  - owned by current user
//  - no group/other permissions (no secrets leakage)
bool aicli_config_file_is_secure(const aicli_config_file_t *cf);

// Loads config values from a JSON file and applies them onto `cfg`.
// Only known keys are applied; unknown keys are ignored.
bool aicli_config_load_from_file(aicli_config_t *cfg, const aicli_config_file_t *cf);

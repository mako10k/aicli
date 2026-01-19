#pragma once

// Small path utilities.

// Returns a newly-allocated realpath() of `path`, or NULL on failure.
// Caller must free.
char *aicli_realpath_dup(const char *path);

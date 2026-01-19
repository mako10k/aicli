#pragma once

#include <stddef.h>

// Reads up to max_bytes from path starting at start.
// - Allocates *out_buf (null-terminated) which caller must free.
// - Writes *out_len (bytes read, excluding '\0') and *out_total (file size).
// Returns 0 on success, -1 on failure (errno is set by stdio functions).
int aicli_read_file_range(const char *path, size_t start, size_t max_bytes, char **out_buf,
                          size_t *out_len, size_t *out_total);

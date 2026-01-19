#include "execute_tool.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char *aicli_realpath_dup(const char *path) {
	if (!path) return NULL;
	char *resolved = realpath(path, NULL);
	return resolved;
}

bool aicli_get_file_size(const char *path, size_t *out_size) {
	if (!path || !out_size) return false;
	struct stat st;
	if (stat(path, &st) != 0) return false;
	if (!S_ISREG(st.st_mode)) return false;
	*out_size = (size_t)st.st_size;
	return true;
}

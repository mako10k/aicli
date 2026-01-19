#include "execute/allowlist.h"

#include <string.h>

bool aicli_allowlist_contains(const aicli_allowlist_t *allow, const char *path)
{
	if (!allow || !path)
		return false;
	for (int i = 0; i < allow->file_count; i++) {
		if (strcmp(allow->files[i].path, path) == 0)
			return true;
	}
	return false;
}

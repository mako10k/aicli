#include "execute/paging.h"

#include <stdlib.h>
#include <string.h>

void aicli_apply_paging(const char *data, size_t total, size_t start, size_t size,
                        aicli_tool_result_t *out)
{
	if (start > total)
		start = total;
	size_t remain = total - start;
	size_t n = remain < size ? remain : size;

	char *buf = (char *)malloc(n + 1);
	if (!buf) {
		out->stderr_text = "oom";
		out->exit_code = 1;
		return;
	}
	memcpy(buf, data + start, n);
	buf[n] = '\0';

	out->stdout_text = buf;
	out->stdout_len = n;
	out->exit_code = 0;
	out->total_bytes = total;
	out->truncated = (start + n) < total;
	out->has_next_start = out->truncated;
	out->next_start = start + n;
}

#include "execute/file_reader.h"

#include <stdio.h>
#include <stdlib.h>

int aicli_read_file_range(const char *path, size_t start, size_t max_bytes, char **out_buf,
                          size_t *out_len, size_t *out_total)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	long total = ftell(fp);
	if (total < 0) {
		fclose(fp);
		return -1;
	}
	*out_total = (size_t)total;

	if ((long)start > total)
		start = (size_t)total;
	if (fseek(fp, (long)start, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}

	size_t to_read = max_bytes;
	if (start + to_read > (size_t)total)
		to_read = (size_t)total - start;

	char *buf = (char *)malloc(to_read + 1);
	if (!buf) {
		fclose(fp);
		return -1;
	}
	size_t n = fread(buf, 1, to_read, fp);
	buf[n] = '\0';
	fclose(fp);

	*out_buf = buf;
	*out_len = n;
	return 0;
}

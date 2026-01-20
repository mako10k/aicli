#include "execute/pipeline_stages.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void aicli_dsl_strip_double_dash(const aicli_dsl_stage_t *st, const char *argv_out[8],
				       int *argc_out)
{
	// POSIX-ish: treat "--" as end of options.
	// We don't implement option permutations; we just remove the marker.
	if (!st || !argv_out || !argc_out) {
		if (argc_out)
			*argc_out = 0;
		return;
	}
	int n = 0;
	for (int i = 0; i < st->argc && i < 8; i++) {
		const char *a = st->argv[i];
		if (a && strcmp(a, "--") == 0)
			continue;
		argv_out[n++] = a;
	}
	for (int i = n; i < 8; i++)
		argv_out[i] = NULL;
	*argc_out = n;
}

bool aicli_stage_nl(const char *in, size_t in_len, aicli_buf_t *out)
{
	// Simple line numbering: "     1\t..."
	unsigned long line = 1;
	size_t i = 0;
	size_t line_start = 0;
	while (i <= in_len) {
		if (i == in_len || in[i] == '\n') {
			char prefix[32];
			int n = snprintf(prefix, sizeof(prefix), "%6lu\t", line);
			if (n < 0)
				return false;
			if (!aicli_buf_append(out, prefix, (size_t)n))
				return false;
			if (!aicli_buf_append(out, in + line_start, i - line_start))
				return false;
			if (i < in_len) {
				if (!aicli_buf_append(out, "\n", 1))
					return false;
			}
			line++;
			line_start = i + 1;
		}
		i++;
	}
	return true;
}

bool aicli_stage_head(const char *in, size_t in_len, size_t nlines, aicli_buf_t *out)
{
	if (nlines == 0)
		return true;
	size_t lines = 0;
	size_t i = 0;
	while (i < in_len) {
		if (!aicli_buf_append(out, &in[i], 1))
			return false;
		if (in[i] == '\n') {
			lines++;
			if (lines >= nlines)
				break;
		}
		i++;
	}
	return true;
}

bool aicli_stage_tail(const char *in, size_t in_len, size_t nlines, aicli_buf_t *out)
{
	if (nlines == 0)
		return true;
	// Find start position of the last N lines.
	size_t lines = 0;
	for (size_t i = in_len; i > 0; i--) {
		if (in[i - 1] == '\n') {
			lines++;
			if (lines == nlines + 1) {
				// start after this newline
				size_t start = i;
				return aicli_buf_append(out, in + start, in_len - start);
			}
		}
	}
	// Not enough newlines: return whole input
	return aicli_buf_append(out, in, in_len);
}

bool aicli_stage_wc(const char *in, size_t in_len, char mode, aicli_buf_t *out)
{
	// mode: 'l' or 'c'
	unsigned long long v = 0;
	if (mode == 'c') {
		v = (unsigned long long)in_len;
	} else if (mode == 'l') {
		for (size_t i = 0; i < in_len; i++)
			if (in[i] == '\n')
				v++;
	} else {
		return false;
	}
	char buf[64];
	int n = snprintf(buf, sizeof(buf), "%llu\n", v);
	if (n < 0)
		return false;
	return aicli_buf_append(out, buf, (size_t)n);
}

typedef struct aicli_line_view {
	const char *s;
	size_t len;
} aicli_line_view_t;

static int cmp_line_asc(const void *a, const void *b)
{
	const aicli_line_view_t *la = (const aicli_line_view_t *)a;
	const aicli_line_view_t *lb = (const aicli_line_view_t *)b;
	size_t min = la->len < lb->len ? la->len : lb->len;
	int c = memcmp(la->s, lb->s, min);
	if (c != 0)
		return c;
	if (la->len < lb->len)
		return -1;
	if (la->len > lb->len)
		return 1;
	return 0;
}

static int cmp_line_desc(const void *a, const void *b)
{
	return -cmp_line_asc(a, b);
}

bool aicli_stage_sort_lines(const char *in, size_t in_len, bool reverse, aicli_buf_t *out)
{
	// Split into line views, sort lexicographically, join with '\n'.
	// Always emits a trailing '\n' when input has at least one line.
	if (in_len == 0)
		return true;

	size_t line_count = 0;
	for (size_t i = 0; i < in_len; i++)
		if (in[i] == '\n')
			line_count++;
	if (in[in_len - 1] != '\n')
		line_count++;
	if (line_count == 0)
		return true;

	aicli_line_view_t *lines = (aicli_line_view_t *)calloc(line_count, sizeof(aicli_line_view_t));
	if (!lines)
		return false;

	size_t idx = 0;
	size_t start = 0;
	for (size_t i = 0; i <= in_len; i++) {
		if (i == in_len || in[i] == '\n') {
			if (idx < line_count) {
				lines[idx].s = in + start;
				lines[idx].len = i - start;
				idx++;
			}
			start = i + 1;
		}
	}

	qsort(lines, line_count, sizeof(aicli_line_view_t), reverse ? cmp_line_desc : cmp_line_asc);

	for (size_t i = 0; i < line_count; i++) {
		if (lines[i].len > 0) {
			if (!aicli_buf_append(out, lines[i].s, lines[i].len)) {
				free(lines);
				return false;
			}
		}
		if (!aicli_buf_append(out, "\n", 1)) {
			free(lines);
			return false;
		}
	}

	free(lines);
	return true;
}

bool aicli_stage_grep_fixed(const char *in, size_t in_len, const char *needle, bool with_line_numbers,
                            aicli_buf_t *out)
{
	if (!needle || needle[0] == '\0')
		return true;
	const size_t needle_len = strlen(needle);

	unsigned long line_no = 1;
	size_t i = 0;
	size_t line_start = 0;
	while (i <= in_len) {
		if (i == in_len || in[i] == '\n') {
			size_t line_len = i - line_start;
			const char *line = in + line_start;

			bool match = false;
			if (needle_len <= line_len) {
				for (size_t off = 0; off + needle_len <= line_len; off++) {
					if (memcmp(line + off, needle, needle_len) == 0) {
						match = true;
						break;
					}
				}
			}

			if (match) {
				if (with_line_numbers) {
					char prefix[32];
					int n = snprintf(prefix, sizeof(prefix), "%lu:", line_no);
					if (n < 0)
						return false;
					if (!aicli_buf_append(out, prefix, (size_t)n))
						return false;
				}
				if (line_len > 0) {
					if (!aicli_buf_append(out, line, line_len))
						return false;
				}
				if (!aicli_buf_append(out, "\n", 1))
					return false;
			}

			line_no++;
			line_start = i + 1;
		}
		i++;
	}
	return true;
}

static bool parse_sed_n_script(const char *script, size_t *out_start, size_t *out_end, char *out_cmd)
{
	// Accept only:
	//  - "Np" / "Nd" (single address)
	//  - "N,Mp" / "N,Md" (range address)
	// Where N/M are 1-based integers.
	if (!script || !*script)
		return false;
	char *end = NULL;
	unsigned long v1 = strtoul(script, &end, 10);
	if (!end || end == script)
		return false;
	if (v1 == 0)
		return false;

	unsigned long v2 = v1;
	if (*end == ',') {
		end++;
		char *end2 = NULL;
		v2 = strtoul(end, &end2, 10);
		if (!end2 || end2 == end)
			return false;
		if (v2 == 0)
			return false;
		end = end2;
	}

	if (*end != 'p' && *end != 'd')
		return false;
	if (end[1] != '\0')
		return false;

	if (v1 > v2)
		return false;
	*out_start = (size_t)v1;
	*out_end = (size_t)v2;
	*out_cmd = *end;
	return true;
}

bool aicli_stage_sed_n_addr(const char *in, size_t in_len, size_t start_addr, size_t end_addr, char cmd,
                            aicli_buf_t *out)
{
	// Implements: sed -n 'Np'/'Nd' and 'N,Mp'/'N,Md'
	if (start_addr == 0 || end_addr == 0)
		return false;
	if (start_addr > end_addr)
		return false;

	unsigned long line_no = 1;
	size_t i = 0;
	size_t line_start = 0;
	while (i <= in_len) {
		if (i == in_len || in[i] == '\n') {
			size_t line_len = i - line_start;
			const char *line = in + line_start;

			bool in_range = (line_no >= start_addr && line_no <= end_addr);
			bool emit = false;
			if (cmd == 'p') {
				emit = in_range;
			} else if (cmd == 'd') {
				emit = !in_range;
			} else {
				return false;
			}

			if (emit) {
				if (line_len > 0) {
					if (!aicli_buf_append(out, line, line_len))
						return false;
				}
				if (!aicli_buf_append(out, "\n", 1))
					return false;
			}

			line_no++;
			line_start = i + 1;
		}
		i++;
	}
	return true;
}

size_t aicli_parse_head_n(const aicli_dsl_stage_t *st, bool *ok)
{
	*ok = true;
	// head -n N
	const char *a[8];
	int ac = 0;
	aicli_dsl_strip_double_dash(st, a, &ac);
	if (ac == 1)
		return 10;
	if (ac == 2 && strncmp(a[1], "-n", 2) == 0 && a[1][2] != '\0') {
		char *end = NULL;
		unsigned long v = strtoul(a[1] + 2, &end, 10);
		if (!end || *end != '\0') {
			*ok = false;
			return 0;
		}
		return (size_t)v;
	}
	if (ac == 3 && strcmp(a[1], "-n") == 0) {
		char *end = NULL;
		unsigned long v = strtoul(a[2], &end, 10);
		if (!end || *end != '\0') {
			*ok = false;
			return 0;
		}
		return (size_t)v;
	}
	*ok = false;
	return 0;
}

size_t aicli_parse_tail_n(const aicli_dsl_stage_t *st, bool *ok)
{
	*ok = true;
	// tail -n N
	const char *a[8];
	int ac = 0;
	aicli_dsl_strip_double_dash(st, a, &ac);
	if (ac == 1)
		return 10;
	if (ac == 2 && strncmp(a[1], "-n", 2) == 0 && a[1][2] != '\0') {
		char *end = NULL;
		unsigned long v = strtoul(a[1] + 2, &end, 10);
		if (!end || *end != '\0') {
			*ok = false;
			return 0;
		}
		return (size_t)v;
	}
	if (ac == 3 && strcmp(a[1], "-n") == 0) {
		char *end = NULL;
		unsigned long v = strtoul(a[2], &end, 10);
		if (!end || *end != '\0') {
			*ok = false;
			return 0;
		}
		return (size_t)v;
	}
	*ok = false;
	return 0;
}

bool aicli_parse_wc_mode(const aicli_dsl_stage_t *st, char *out_mode)
{
	// wc -l | wc -c
	const char *a[8];
	int ac = 0;
	aicli_dsl_strip_double_dash(st, a, &ac);
	if (ac != 2)
		return false;
	if (strcmp(a[1], "-l") == 0) {
		*out_mode = 'l';
		return true;
	}
	if (strcmp(a[1], "-c") == 0) {
		*out_mode = 'c';
		return true;
	}
	return false;
}

bool aicli_parse_sort_reverse(const aicli_dsl_stage_t *st, bool *out_reverse)
{
	// sort | sort -r
	const char *a[8];
	int ac = 0;
	aicli_dsl_strip_double_dash(st, a, &ac);
	if (ac == 1) {
		*out_reverse = false;
		return true;
	}
	if (ac == 2 && strcmp(a[1], "-r") == 0) {
		*out_reverse = true;
		return true;
	}
	return false;
}

bool aicli_parse_grep_args(const aicli_dsl_stage_t *st, const char **out_pattern, bool *out_n)
{
	// grep PATTERN | grep -n PATTERN
	const char *a[8];
	int ac = 0;
	aicli_dsl_strip_double_dash(st, a, &ac);
	if (ac == 2) {
		*out_n = false;
		*out_pattern = a[1];
		return true;
	}
	if (ac == 3 && strcmp(a[1], "-n") == 0) {
		*out_n = true;
		*out_pattern = a[2];
		return true;
	}
	return false;
}

bool aicli_parse_sed_args(const aicli_dsl_stage_t *st, size_t *out_start, size_t *out_end, char *out_cmd)
{
	// sed -n 'Np'/'Nd' and 'N,Mp'/'N,Md'
	const char *a[8];
	int ac = 0;
	aicli_dsl_strip_double_dash(st, a, &ac);
	if (ac != 3)
		return false;
	if (strcmp(a[1], "-n") != 0)
		return false;
	return parse_sed_n_script(a[2], out_start, out_end, out_cmd);
}

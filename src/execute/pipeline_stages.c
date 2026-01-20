#include "execute/pipeline_stages.h"

#include <stdbool.h>
#include <regex.h>
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
	// mode: 'l' (lines) or 'c' (bytes) or 'w' (words)
	unsigned long long v = 0;
	if (mode == 'c') {
		v = (unsigned long long)in_len;
	} else if (mode == 'l') {
		for (size_t i = 0; i < in_len; i++)
			if (in[i] == '\n')
				v++;
	} else if (mode == 'w') {
		// POSIX-ish word count: transitions from whitespace to non-whitespace.
		bool in_word = false;
		for (size_t i = 0; i < in_len; i++) {
			unsigned char ch = (unsigned char)in[i];
			bool ws = (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f');
			if (ws) {
				in_word = false;
			} else if (!in_word) {
				v++;
				in_word = true;
			}
		}
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

	// If caller passed -F, this function is used for fixed substring search.
	// (name kept for backward compatibility)

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

bool aicli_stage_grep_bre(const char *in, size_t in_len, const char *pattern, bool with_line_numbers,
			  aicli_buf_t *out)
{
	if (!pattern || pattern[0] == '\0')
		return true;

	regex_t rx;
	memset(&rx, 0, sizeof(rx));
	int rc = regcomp(&rx, pattern, 0);
	if (rc != 0) {
		char errbuf[256];
		regerror(rc, &rx, errbuf, sizeof(errbuf));
		aicli_buf_append(out, "grep: ", 6);
		aicli_buf_append(out, errbuf, strlen(errbuf));
		aicli_buf_append(out, "\n", 1);
		regfree(&rx);
		return false;
	}

	unsigned long line_no = 1;
	size_t i = 0;
	size_t line_start = 0;
	while (i <= in_len) {
		if (i == in_len || in[i] == '\n') {
			size_t line_len = i - line_start;
			const char *line = in + line_start;

			// NUL-terminate for regexec.
			char *z = (char *)malloc(line_len + 1);
			if (!z) {
				regfree(&rx);
				return false;
			}
			memcpy(z, line, line_len);
			z[line_len] = '\0';

			int er = regexec(&rx, z, 0, NULL, 0);
			free(z);
			bool match = (er == 0);
			if (er != 0 && er != REG_NOMATCH) {
				char errbuf[256];
				regerror(er, &rx, errbuf, sizeof(errbuf));
				aicli_buf_append(out, "grep: ", 6);
				aicli_buf_append(out, errbuf, strlen(errbuf));
				aicli_buf_append(out, "\n", 1);
				regfree(&rx);
				return false;
			}

			if (match) {
				if (with_line_numbers) {
					char prefix[32];
					int n = snprintf(prefix, sizeof(prefix), "%lu:", line_no);
					if (n < 0) {
						regfree(&rx);
						return false;
					}
					if (!aicli_buf_append(out, prefix, (size_t)n)) {
						regfree(&rx);
						return false;
					}
				}
				if (line_len > 0) {
					if (!aicli_buf_append(out, line, line_len)) {
						regfree(&rx);
						return false;
					}
				}
				if (!aicli_buf_append(out, "\n", 1)) {
					regfree(&rx);
					return false;
				}
			}

			line_no++;
			line_start = i + 1;
		}
		i++;
	}

	regfree(&rx);
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

static bool parse_sed_re_addr(const char *s, const char **out_re, size_t *out_re_len, const char **out_end)
{
	// Parse /RE/ where delimiter is fixed '/'. RE must be non-empty.
	if (!s || s[0] != '/')
		return false;
	const char *re = s + 1;
	const char *end = strchr(re, '/');
	if (!end || end == re)
		return false;
	*out_re = re;
	*out_re_len = (size_t)(end - re);
	*out_end = end + 1;
	return true;
}

static bool parse_sed_re_script(const char *script, const char **out_re1, size_t *out_re1_len,
				const char **out_re2, size_t *out_re2_len, char *out_cmd)
{
	// Accept only:
	//   /RE/p  /RE/d
	//   /RE/,/RE/p  /RE/,/RE/d
	// Delimiter is fixed to '/'. No escaping supported (safe subset).
	if (!script || !*script)
		return false;

	const char *re1 = NULL;
	size_t re1_len = 0;
	const char *p = NULL;
	if (!parse_sed_re_addr(script, &re1, &re1_len, &p))
		return false;

	const char *re2 = NULL;
	size_t re2_len = 0;
	if (p[0] == ',') {
		p++;
		if (!parse_sed_re_addr(p, &re2, &re2_len, &p))
			return false;
	}

	if (p[0] != 'p' && p[0] != 'd')
		return false;
	if (p[1] != '\0')
		return false;

	*out_re1 = re1;
	*out_re1_len = re1_len;
	*out_re2 = re2;
	*out_re2_len = re2_len;
	*out_cmd = p[0];
	return true;
}

bool aicli_parse_sed_re_args(const aicli_dsl_stage_t *st, const char **out_re1, size_t *out_re1_len,
			    const char **out_re2, size_t *out_re2_len, char *out_cmd)
{
	// sed -n '/RE/p' '/RE/d' '/RE/,/RE/p' '/RE/,/RE/d'
	const char *a[8];
	int ac = 0;
	aicli_dsl_strip_double_dash(st, a, &ac);
	if (ac != 3)
		return false;
	if (strcmp(a[1], "-n") != 0)
		return false;

	return parse_sed_re_script(a[2], out_re1, out_re1_len, out_re2, out_re2_len, out_cmd);
}

bool aicli_stage_sed_n_re_addr(const char *in, size_t in_len, const char *re1, size_t re1_len,
			      const char *re2, size_t re2_len, char cmd, aicli_buf_t *out)
{
	if (!in || !out || !re1 || re1_len == 0)
		return false;

	char *re1_z = (char *)malloc(re1_len + 1);
	if (!re1_z)
		return false;
	memcpy(re1_z, re1, re1_len);
	re1_z[re1_len] = '\0';

	char *re2_z = NULL;
	if (re2 && re2_len > 0) {
		re2_z = (char *)malloc(re2_len + 1);
		if (!re2_z) {
			free(re1_z);
			return false;
		}
		memcpy(re2_z, re2, re2_len);
		re2_z[re2_len] = '\0';
	}

	regex_t rx1;
	memset(&rx1, 0, sizeof(rx1));
	int rc = regcomp(&rx1, re1_z, 0);
	if (rc != 0) {
		free(re1_z);
		free(re2_z);
		regfree(&rx1);
		return false;
	}
	regex_t rx2;
	memset(&rx2, 0, sizeof(rx2));
	bool has_rx2 = false;
	if (re2_z) {
		rc = regcomp(&rx2, re2_z, 0);
		if (rc != 0) {
			regfree(&rx1);
			free(re1_z);
			free(re2_z);
			regfree(&rx2);
			return false;
		}
		has_rx2 = true;
	}

	bool in_range = false;
	unsigned long line_no = 1;
	size_t i = 0;
	size_t line_start = 0;
	while (i <= in_len) {
		if (i == in_len || in[i] == '\n') {
			size_t line_len = i - line_start;
			const char *line = in + line_start;

			char *z = (char *)malloc(line_len + 1);
			if (!z) {
				regfree(&rx1);
				if (has_rx2)
					regfree(&rx2);
				free(re1_z);
				free(re2_z);
				return false;
			}
			memcpy(z, line, line_len);
			z[line_len] = '\0';

			bool m1 = (regexec(&rx1, z, 0, NULL, 0) == 0);
			bool m2 = false;
			if (has_rx2)
				m2 = (regexec(&rx2, z, 0, NULL, 0) == 0);
			free(z);

			bool selected = false;
			if (!has_rx2) {
				selected = m1;
			} else {
				if (!in_range) {
					if (m1)
						in_range = true;
				}
				if (in_range) {
					selected = true;
					if (m2)
						in_range = false;
				}
			}

			bool emit = false;
			if (cmd == 'p')
				emit = selected;
			else if (cmd == 'd')
				emit = !selected;
			else {
				regfree(&rx1);
				if (has_rx2)
					regfree(&rx2);
				free(re1_z);
				free(re2_z);
				return false;
			}

			if (emit) {
				if (line_len > 0) {
					if (!aicli_buf_append(out, line, line_len)) {
						regfree(&rx1);
						if (has_rx2)
							regfree(&rx2);
						free(re1_z);
						free(re2_z);
						return false;
					}
				}
				if (!aicli_buf_append(out, "\n", 1)) {
					regfree(&rx1);
					if (has_rx2)
						regfree(&rx2);
					free(re1_z);
					free(re2_z);
					return false;
				}
			}

			line_no++;
			line_start = i + 1;
		}
		i++;
	}

	regfree(&rx1);
	if (has_rx2)
		regfree(&rx2);
	free(re1_z);
	free(re2_z);
	(void)line_no;
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
	// wc -l | wc -c | wc -w
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
	if (strcmp(a[1], "-w") == 0) {
		*out_mode = 'w';
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

bool aicli_parse_grep_args(const aicli_dsl_stage_t *st, const char **out_pattern, bool *out_n, bool *out_fixed)
{
	// grep PATTERN | grep -n PATTERN | grep -F PATTERN | grep -n -F PATTERN
	const char *a[8];
	int ac = 0;
	aicli_dsl_strip_double_dash(st, a, &ac);
	if (ac == 2) {
		*out_n = false;
		*out_fixed = false;
		*out_pattern = a[1];
		return true;
	}
	if (ac == 3 && strcmp(a[1], "-n") == 0) {
		*out_n = true;
		*out_fixed = false;
		*out_pattern = a[2];
		return true;
	}
	if (ac == 3 && strcmp(a[1], "-F") == 0) {
		*out_n = false;
		*out_fixed = true;
		*out_pattern = a[2];
		return true;
	}
	if (ac == 4) {
		bool n = false;
		bool fixed = false;
		int opt_cnt = 0;
		for (int i = 1; i <= 2; i++) {
			if (strcmp(a[i], "-n") == 0) {
				n = true;
				opt_cnt++;
				continue;
			}
			if (strcmp(a[i], "-F") == 0) {
				fixed = true;
				opt_cnt++;
				continue;
			}
		}
		if (opt_cnt == 2) {
			*out_n = n;
			*out_fixed = fixed;
			*out_pattern = a[3];
			return true;
		}
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

static bool parse_sed_subst_script(const char *script, const char **out_pat, size_t *out_pat_len,
				 const char **out_repl, size_t *out_repl_len,
				 bool *out_global, bool *out_print_on_match)
{
	// Accept only:
	//   s/RE/REPL/
	//   s/RE/REPL/g
	//   s/RE/REPL/p
	//   s/RE/REPL/gp
	// Delimiter is fixed to '/'. No backrefs supported in REPL.
	if (!script || !*script)
		return false;
	if (script[0] != 's' || script[1] != '/')
		return false;
	const char *p = script + 2;
	const char *re_end = strchr(p, '/');
	if (!re_end || re_end == p)
		return false;
	const char *repl = re_end + 1;
	const char *repl_end = strchr(repl, '/');
	if (!repl_end)
		return false;

	size_t pat_len = (size_t)(re_end - p);
	size_t repl_len = (size_t)(repl_end - repl);

	bool g = false;
	bool pr = false;
	for (const char *f = repl_end + 1; *f; f++) {
		if (*f == 'g') {
			g = true;
			continue;
		}
		if (*f == 'p') {
			pr = true;
			continue;
		}
		return false;
	}

	*out_pat = p;
	*out_pat_len = pat_len;
	*out_repl = repl;
	*out_repl_len = repl_len;
	*out_global = g;
	*out_print_on_match = pr;
	return true;
}

bool aicli_parse_sed_subst_args(const aicli_dsl_stage_t *st, const char **out_pattern,
				 const char **out_repl, bool *out_global,
				 bool *out_print_on_match)
{
	// sed -n 's/RE/REPL/[gp]'
	const char *a[8];
	int ac = 0;
	aicli_dsl_strip_double_dash(st, a, &ac);
	if (ac != 3)
		return false;
	if (strcmp(a[1], "-n") != 0)
		return false;
	const char *pat = NULL;
	size_t pat_len = 0;
	const char *rep = NULL;
	size_t rep_len = 0;
	if (!parse_sed_subst_script(a[2], &pat, &pat_len, &rep, &rep_len, out_global, out_print_on_match))
		return false;
	// Copy out NUL-terminated strings for later use in stage execution.
	char *pat_z = (char *)malloc(pat_len + 1);
	char *rep_z = (char *)malloc(rep_len + 1);
	if (!pat_z || !rep_z) {
		free(pat_z);
		free(rep_z);
		return false;
	}
	memcpy(pat_z, pat, pat_len);
	pat_z[pat_len] = '\0';
	memcpy(rep_z, rep, rep_len);
	rep_z[rep_len] = '\0';
	*out_pattern = pat_z;
	*out_repl = rep_z;
	return true;
}

bool aicli_stage_sed_n_subst(const char *in, size_t in_len, const char *pattern, const char *repl,
			     bool global, bool print_on_match, aicli_buf_t *out)
{
	if (!in || !pattern || !repl || !out)
		return false;
	const char *dbg = getenv("AICLI_DEBUG_FUNCTION_CALL");
	// `pattern` and `repl` are already NUL-terminated (allocated) by the parser.
	const char *pat = pattern;
	const char *rep = repl;
	size_t repl_len = strlen(rep);

	// Compile BRE (REG_EXTENDED not set). We need match offsets, so do NOT use REG_NOSUB.
	regex_t rx;
	memset(&rx, 0, sizeof(rx));
	int rc = regcomp(&rx, pat, 0);
	if (rc != 0) {
		char errbuf[256];
		regerror(rc, &rx, errbuf, sizeof(errbuf));
		aicli_buf_append(out, "sed: ", 5);
		aicli_buf_append(out, errbuf, strlen(errbuf));
		aicli_buf_append(out, "\n", 1);
		regfree(&rx);
		free((void *)pattern);
		free((void *)repl);
		return false;
	}


	const size_t k_max_line_len = 64 * 1024;
	const size_t k_max_out_bytes_per_line = 256 * 1024;
	const size_t k_max_subst_per_line = 4096;

	size_t i = 0;
	size_t line_start = 0;
	while (i <= in_len) {
		if (i == in_len || in[i] == '\n') {
			size_t line_len = i - line_start;
			const char *line = in + line_start;
			if (line_len > k_max_line_len) {
				regfree(&rx);
				free((void *)pattern);
				free((void *)repl);
				return false;
			}

			bool matched_any = false;
			aicli_buf_t tmp;
			if (!aicli_buf_init(&tmp, line_len + 32)) {
				regfree(&rx);
				free((void *)pattern);
				free((void *)repl);
				return false;
			}
			// Defensive: ensure empty output for this line.
			tmp.len = 0;

			size_t cursor = 0;
			size_t subst_cnt = 0;
			while (cursor <= line_len) {
				size_t rem = line_len - cursor;
				char *z = (char *)malloc(rem + 1);
				if (!z) {
					aicli_buf_free(&tmp);
					regfree(&rx);
					free((void *)pattern);
					free((void *)repl);
					return false;
				}
				memcpy(z, line + cursor, rem);
				z[rem] = '\0';

				regmatch_t m;
				m.rm_so = -1;
				m.rm_eo = -1;
				int er = regexec(&rx, z, 1, &m, 0);
				if (er == REG_NOMATCH) {
					free(z);
					break;
				}
				if (er != 0) {
					char errbuf[256];
					regerror(er, &rx, errbuf, sizeof(errbuf));
					aicli_buf_append(out, "sed: ", 5);
					aicli_buf_append(out, errbuf, strlen(errbuf));
					aicli_buf_append(out, "\n", 1);
					free(z);
					aicli_buf_free(&tmp);
					regfree(&rx);
					free((void *)pattern);
					free((void *)repl);
					return false;
				}
				if (m.rm_so < 0 || m.rm_eo < 0) {
					free(z);
					break;
				}
				size_t so = (size_t)m.rm_so;
				size_t eo = (size_t)m.rm_eo;
				if (eo < so) {
					free(z);
					break;
				}

				matched_any = true;
				subst_cnt++;
				if (subst_cnt > k_max_subst_per_line) {
					free(z);
					aicli_buf_free(&tmp);
					regfree(&rx);
					free((void *)pattern);
					free((void *)repl);
					return false;
				}

				if (so > 0) {
					if (!aicli_buf_append(&tmp, z, so)) {
						free(z);
						aicli_buf_free(&tmp);
						regfree(&rx);
						free((void *)pattern);
						free((void *)repl);
						return false;
					}
				}
				if (!aicli_buf_append(&tmp, rep, repl_len)) {
					free(z);
					aicli_buf_free(&tmp);
					regfree(&rx);
					free((void *)pattern);
					free((void *)repl);
					return false;
				}
				if (tmp.len > k_max_out_bytes_per_line) {
					free(z);
					aicli_buf_free(&tmp);
					regfree(&rx);
					free((void *)pattern);
					free((void *)repl);
					return false;
				}

				cursor += eo;
				free(z);
				if (!global)
					break;

				if (eo == 0) {
					if (cursor < line_len) {
						if (!aicli_buf_append(&tmp, line + cursor, 1)) {
							aicli_buf_free(&tmp);
							regfree(&rx);
							free((void *)pattern);
							free((void *)repl);
							return false;
						}
						cursor++;
					} else {
						break;
					}
				}
			}

			if (matched_any && cursor < line_len) {
				if (!aicli_buf_append(&tmp, line + cursor, line_len - cursor)) {
					aicli_buf_free(&tmp);
					regfree(&rx);
					free((void *)pattern);
					free((void *)repl);
					return false;
				}
			}

			if (dbg && dbg[0] != '\0') {
				fprintf(stderr,
				        "[debug:sed] pat='%s' rep='%s' line_start=%zu line_len=%zu matched_any=%d global=%d p=%d tmp_len=%zu\n",
				        pat, rep, line_start, line_len, matched_any ? 1 : 0, global ? 1 : 0,
				        print_on_match ? 1 : 0, tmp.len);
				if (matched_any) {
					fprintf(stderr, "[debug:sed] line='%.*s'\n", (int)line_len, line);
				}
			}

			if (print_on_match && matched_any) {
				if (!aicli_buf_append(out, tmp.data, tmp.len)) {
					aicli_buf_free(&tmp);
					regfree(&rx);
					free((void *)pattern);
					free((void *)repl);
					return false;
				}
				if (!aicli_buf_append(out, "\n", 1)) {
					aicli_buf_free(&tmp);
					regfree(&rx);
					free((void *)pattern);
					free((void *)repl);
					return false;
				}
			}

			aicli_buf_free(&tmp);
			line_start = i + 1;
		}
		i++;
	}

	regfree(&rx);
	free((void *)pattern);
	free((void *)repl);
	return true;
}

#include "execute_dsl.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char *aicli_strdup(const char *s)
{
	if (!s)
		return NULL;
	size_t n = strlen(s);
	char *p = (char *)malloc(n + 1);
	if (!p)
		return NULL;
	memcpy(p, s, n + 1);
	return p;
}

static bool is_forbidden_char(char c)
{
	// Block obvious shell metacharacters and redirects.
	switch (c) {
	case ';':
	case '&':
	case '>':
	case '<':
	case '$':
	case '`':
	case '\n':
	case '\r':
		return true;
	default:
		return false;
	}
}

static bool is_forbidden_char_in_quote(char c)
{
	// Inside quotes we still forbid expansion/substitution metacharacters.
	// We intentionally allow spaces and '|' as data.
	switch (c) {
	case '$':
	case '`':
	case '\n':
	case '\r':
		return true;
	default:
		return false;
	}
}

static aicli_cmd_kind_t cmd_kind_from_token(const char *tok)
{
	if (strcmp(tok, "cat") == 0)
		return AICLI_CMD_CAT;
	if (strcmp(tok, "nl") == 0)
		return AICLI_CMD_NL;
	if (strcmp(tok, "head") == 0)
		return AICLI_CMD_HEAD;
	if (strcmp(tok, "tail") == 0)
		return AICLI_CMD_TAIL;
	if (strcmp(tok, "wc") == 0)
		return AICLI_CMD_WC;
	if (strcmp(tok, "sort") == 0)
		return AICLI_CMD_SORT;
	if (strcmp(tok, "grep") == 0)
		return AICLI_CMD_GREP;
	if (strcmp(tok, "sed") == 0)
		return AICLI_CMD_SED;
	return AICLI_CMD_UNKNOWN;
}

static void skip_ws(const char **p)
{
	while (**p && isspace((unsigned char)**p))
		(*p)++;
}

static bool read_token(const char **p, char *buf, size_t buflen)
{
	skip_ws(p);
	if (!**p)
		return false;

	char quote = 0;
	size_t n = 0;

	if (**p == '\'' || **p == '"') {
		quote = **p;
		(*p)++;
		while (**p && **p != quote) {
			char ch = **p;
			if (quote == '"' && ch == '\\') {
				// Minimal backslash escapes inside quotes.
				(*p)++;
				if (!**p)
					return false;
				ch = **p;
			}
			if (is_forbidden_char_in_quote(ch))
				return false;
			if (n + 1 >= buflen)
				return false;
			buf[n++] = ch;
			(*p)++;
		}
		if (**p != quote)
			return false;
		(*p)++;
	} else {
		while (**p && !isspace((unsigned char)**p) && **p != '|') {
			char ch = **p;
			if (ch == '\\') {
				// Minimal backslash escapes outside quotes (POSIX-ish).
				(*p)++;
				if (!**p)
					return false;
				ch = **p;
			}
			if (is_forbidden_char(ch))
				return false;
			if (n + 1 >= buflen)
				return false;
			buf[n++] = ch;
			(*p)++;
		}
	}

	buf[n] = '\0';
	return n > 0;
}

const char *aicli_dsl_status_str(aicli_dsl_status_t st)
{
	switch (st) {
	case AICLI_DSL_OK:
		return "ok";
	case AICLI_DSL_ERR_EMPTY:
		return "empty";
	case AICLI_DSL_ERR_PARSE:
		return "parse_error";
	case AICLI_DSL_ERR_FORBIDDEN:
		return "forbidden";
	case AICLI_DSL_ERR_TOO_MANY_STAGES:
		return "too_many_stages";
	case AICLI_DSL_ERR_TOO_MANY_ARGS:
		return "too_many_args";
	default:
		return "unknown";
	}
}

aicli_dsl_status_t aicli_dsl_parse_pipeline(const char *command, aicli_dsl_pipeline_t *out)
{
	if (!out)
		return AICLI_DSL_ERR_PARSE;
	memset(out, 0, sizeof(*out));
	if (!command)
		return AICLI_DSL_ERR_EMPTY;

	for (const char *c = command; *c; c++) {
		// Allow pipelines separated by '|', while still blocking other metacharacters.
		if (*c == '|')
			continue;
		if (is_forbidden_char(*c))
			return AICLI_DSL_ERR_FORBIDDEN;
	}

	const char *p = command;
	char tok[256];

	while (1) {
		skip_ws(&p);
		if (!*p)
			break;
		if (out->stage_count >= 8)
			return AICLI_DSL_ERR_TOO_MANY_STAGES;

		aicli_dsl_stage_t *st = &out->stages[out->stage_count];
		memset(st, 0, sizeof(*st));

		if (!read_token(&p, tok, sizeof(tok)))
			return AICLI_DSL_ERR_PARSE;
		st->kind = cmd_kind_from_token(tok);
		if (st->kind == AICLI_CMD_UNKNOWN)
			return AICLI_DSL_ERR_FORBIDDEN;
		st->argv[st->argc++] = aicli_strdup(tok);
		if (!st->argv[st->argc - 1])
			return AICLI_DSL_ERR_PARSE;

		while (1) {
			skip_ws(&p);
			if (!*p || *p == '|')
				break;
			if (st->argc >= 8)
				return AICLI_DSL_ERR_TOO_MANY_ARGS;
			if (!read_token(&p, tok, sizeof(tok)))
				return AICLI_DSL_ERR_PARSE;
			st->argv[st->argc++] = aicli_strdup(tok);
			if (!st->argv[st->argc - 1])
				return AICLI_DSL_ERR_PARSE;
		}

		out->stage_count++;
		skip_ws(&p);
		if (*p == '|') {
			p++;
			continue;
		}
		break;
	}

	if (out->stage_count == 0)
		return AICLI_DSL_ERR_EMPTY;
	return AICLI_DSL_OK;
}

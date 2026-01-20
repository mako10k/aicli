#include "execute/dispatch.h"

#include "execute/pipeline_stages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef bool (*aicli_stage_apply_fn)(const aicli_dsl_stage_t *stg, const char *in, size_t in_len,
                                    aicli_buf_t *out);

typedef struct aicli_stage_dispatch {
	aicli_cmd_kind_t kind;
	aicli_stage_apply_fn apply;
} aicli_stage_dispatch_t;

static bool apply_nl(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	// Accept minimal compatibility flags like: nl -ba
	if (stg && stg->argc > 1) {
		if (!(stg->argc == 2 && strcmp(stg->argv[1], "-ba") == 0))
			return false;
	}
	return aicli_stage_nl(in, in_len, out);
}

static bool apply_head(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	bool ok = true;
	size_t nlines = aicli_parse_head_n(stg, &ok);
	return ok && aicli_stage_head(in, in_len, nlines, out);
}

static bool apply_tail(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	bool ok = true;
	size_t nlines = aicli_parse_tail_n(stg, &ok);
	return ok && aicli_stage_tail(in, in_len, nlines, out);
}

static bool apply_wc(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	char mode = 0;
	if (!aicli_parse_wc_mode(stg, &mode))
		return false;
	return aicli_stage_wc(in, in_len, mode, out);
}

static bool apply_sort(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	bool reverse = false;
	if (!aicli_parse_sort_reverse(stg, &reverse))
		return false;
	return aicli_stage_sort_lines(in, in_len, reverse, out);
}

static bool apply_grep(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	const char *pattern = NULL;
	bool with_n = false;
	bool fixed = false;
	bool invert = false;
	if (!aicli_parse_grep_args(stg, &pattern, &with_n, &fixed, &invert))
		return false;
	if (fixed) {
		if (invert)
			return aicli_stage_grep_fixed_invert(in, in_len, pattern, with_n, out);
		return aicli_stage_grep_fixed(in, in_len, pattern, with_n, out);
	}
	if (invert)
		return aicli_stage_grep_bre_invert(in, in_len, pattern, with_n, out);
	return aicli_stage_grep_bre(in, in_len, pattern, with_n, out);
}

static bool apply_sed(const aicli_dsl_stage_t *stg, const char *in, size_t in_len, aicli_buf_t *out)
{
	// Prefer sed -n address scripts, then substitution scripts.
	char cmd = 0;
	const char *re1 = NULL;
	size_t re1_len = 0;
	const char *re2 = NULL;
	size_t re2_len = 0;
	if (aicli_parse_sed_re_args(stg, &re1, &re1_len, &re2, &re2_len, &cmd)) {
		return aicli_stage_sed_n_re_addr(in, in_len, re1, re1_len, re2, re2_len, cmd, out);
	}

	size_t start_addr = 0;
	size_t end_addr = 0;
	cmd = 0;
	if (aicli_parse_sed_args(stg, &start_addr, &end_addr, &cmd)) {
		return aicli_stage_sed_n_addr(in, in_len, start_addr, end_addr, cmd, out);
	}

	const char *pattern = NULL;
	const char *repl = NULL;
	bool global = false;
	bool print_on_match = false;
	if (!aicli_parse_sed_subst_args(stg, &pattern, &repl, &global, &print_on_match)) {
		return false;
	}
	return aicli_stage_sed_n_subst(in, in_len, pattern, repl, global, print_on_match, out);
}

static const aicli_stage_dispatch_t k_dispatch[] = {
	{ AICLI_CMD_NL, apply_nl },        { AICLI_CMD_HEAD, apply_head },
	{ AICLI_CMD_TAIL, apply_tail },    { AICLI_CMD_WC, apply_wc },
	{ AICLI_CMD_SORT, apply_sort },    { AICLI_CMD_GREP, apply_grep },
	{ AICLI_CMD_SED, apply_sed },
};

static aicli_stage_apply_fn dispatch_find(aicli_cmd_kind_t kind)
{
	for (size_t i = 0; i < (sizeof(k_dispatch) / sizeof(k_dispatch[0])); i++) {
		if (k_dispatch[i].kind == kind)
			return k_dispatch[i].apply;
	}
	return NULL;
}

bool aicli_execute_apply_stage(const aicli_dsl_stage_t *stg, const char *in, size_t in_len,
                              aicli_buf_t *out)
{
	aicli_stage_apply_fn apply = dispatch_find(stg->kind);
	if (!apply)
		return false;
	return apply(stg, in, in_len, out);
}

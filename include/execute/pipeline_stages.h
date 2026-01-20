#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "buf.h"
#include "execute_dsl.h"

// Text stages
bool aicli_stage_nl(const char *in, size_t in_len, aicli_buf_t *out);
bool aicli_stage_head(const char *in, size_t in_len, size_t nlines, aicli_buf_t *out);
bool aicli_stage_tail(const char *in, size_t in_len, size_t nlines, aicli_buf_t *out);
bool aicli_stage_wc(const char *in, size_t in_len, char mode, aicli_buf_t *out);
bool aicli_stage_sort_lines(const char *in, size_t in_len, bool reverse, aicli_buf_t *out);
bool aicli_stage_grep_fixed(const char *in, size_t in_len, const char *needle,
                            bool with_line_numbers, aicli_buf_t *out);
bool aicli_stage_sed_n_addr(const char *in, size_t in_len, size_t start_addr, size_t end_addr,
                            char cmd, aicli_buf_t *out);

// sed -n 's/RE/REPL/[gp]'
bool aicli_stage_sed_n_subst(const char *in, size_t in_len, const char *pattern, const char *repl,
                             bool global, bool print_on_match, aicli_buf_t *out);

// Stage-arg parsing helpers
size_t aicli_parse_head_n(const aicli_dsl_stage_t *st, bool *ok);
size_t aicli_parse_tail_n(const aicli_dsl_stage_t *st, bool *ok);
bool aicli_parse_wc_mode(const aicli_dsl_stage_t *st, char *out_mode);
bool aicli_parse_sort_reverse(const aicli_dsl_stage_t *st, bool *out_reverse);
bool aicli_parse_grep_args(const aicli_dsl_stage_t *st, const char **out_pattern, bool *out_n);
bool aicli_parse_sed_args(const aicli_dsl_stage_t *st, size_t *out_start, size_t *out_end,
                          char *out_cmd);

bool aicli_parse_sed_subst_args(const aicli_dsl_stage_t *st, const char **out_pattern,
                                const char **out_repl, bool *out_global,
                                bool *out_print_on_match);

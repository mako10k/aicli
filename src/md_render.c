#include "md_render.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"

#ifdef HAVE_MD4C
#include <md4c.h>

typedef struct {
	aicli_buf_t out;
	bool use_ansi;
	unsigned heading_level;
	int strong_depth;
	int em_depth;
	int code_span_depth;
	int link_depth;
	bool in_code_block;
	int style_bold;
	int style_underline;
	int style_color;
} md_plain_ctx_t;

static char *dup_cstr_local(const char *s)
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

static int append_newline(md_plain_ctx_t *ctx)
{
	if (!ctx)
		return 1;
	if (ctx->out.len > 0) {
		char last = ctx->out.data[ctx->out.len - 1];
		if (last == '\n')
			return 0;
	}
	if (!aicli_buf_append(&ctx->out, "\n", 1))
		return 1;
	return 0;
}

static bool should_use_ansi(void)
{
	if (!isatty(STDOUT_FILENO))
		return false;
	if (getenv("NO_COLOR"))
		return false;
	const char *term = getenv("TERM");
	if (term && strcmp(term, "dumb") == 0)
		return false;
	return true;
}

static int sb_addf(char *dst, size_t cap, size_t *pos, const char *fmt, ...)
{
	if (!dst || !pos || *pos >= cap)
		return 1;
	va_list ap;
	va_start(ap, fmt);
	int w = vsnprintf(dst + *pos, cap - *pos, fmt, ap);
	va_end(ap);
	if (w < 0)
		return 1;
	size_t avail = cap - *pos;
	if ((size_t)w >= avail) {
		*pos = cap - 1;
		return 0;
	}
	*pos += (size_t)w;
	return 0;
}

static int apply_style(md_plain_ctx_t *ctx)
{
	if (!ctx || !ctx->use_ansi)
		return 0;

	int bold = (ctx->heading_level > 0) || (ctx->strong_depth > 0);
	int underline = (ctx->em_depth > 0) || (ctx->link_depth > 0);
	int color = 0;
	if (ctx->heading_level > 0)
		color = 36; // cyan
	else if (ctx->in_code_block || ctx->code_span_depth > 0)
		color = 33; // yellow
	else if (ctx->link_depth > 0)
		color = 34; // blue

	if (bold == ctx->style_bold && underline == ctx->style_underline && color == ctx->style_color)
		return 0;

	ctx->style_bold = bold;
	ctx->style_underline = underline;
	ctx->style_color = color;

	if (!aicli_buf_append(&ctx->out, "\x1b[0m", 4))
		return 1;

	if (!bold && !underline && color == 0)
		return 0;

	char seq[64];
	size_t pos = 0;
	bool first = true;

	if (sb_addf(seq, sizeof(seq), &pos, "\x1b[") != 0)
		return 1;
	if (bold) {
		if (sb_addf(seq, sizeof(seq), &pos, "%s1", first ? "" : ";") != 0)
			return 1;
		first = false;
	}
	if (underline) {
		if (sb_addf(seq, sizeof(seq), &pos, "%s4", first ? "" : ";") != 0)
			return 1;
		first = false;
	}
	if (color) {
		if (sb_addf(seq, sizeof(seq), &pos, "%s%d", first ? "" : ";", color) != 0)
			return 1;
		first = false;
	}
	if (sb_addf(seq, sizeof(seq), &pos, "m") != 0)
		return 1;

	if (!aicli_buf_append(&ctx->out, seq, pos))
		return 1;
	return 0;
}

static int on_enter_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
	md_plain_ctx_t *ctx = (md_plain_ctx_t *)userdata;
	if (!ctx)
		return 1;

	switch (type) {
	case MD_BLOCK_H: {
		if (append_newline(ctx) != 0)
			return 1;
		MD_BLOCK_H_DETAIL *d = (MD_BLOCK_H_DETAIL *)detail;
		ctx->heading_level = d ? d->level : 0;
		return apply_style(ctx);
	}
	case MD_BLOCK_CODE:
		if (append_newline(ctx) != 0)
			return 1;
		ctx->in_code_block = true;
		return apply_style(ctx);
	case MD_BLOCK_P:
	case MD_BLOCK_QUOTE:
	case MD_BLOCK_UL:
	case MD_BLOCK_OL:
	case MD_BLOCK_TABLE:
		return append_newline(ctx);
	default:
		return 0;
	}
}

static int on_leave_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
	(void)detail;
	md_plain_ctx_t *ctx = (md_plain_ctx_t *)userdata;
	if (!ctx)
		return 1;

	switch (type) {
	case MD_BLOCK_H:
		ctx->heading_level = 0;
		if (apply_style(ctx) != 0)
			return 1;
		return append_newline(ctx);
	case MD_BLOCK_CODE:
		ctx->in_code_block = false;
		if (apply_style(ctx) != 0)
			return 1;
		return append_newline(ctx);
	case MD_BLOCK_P:
	case MD_BLOCK_LI:
		return append_newline(ctx);
	default:
		return 0;
	}
}

static int on_enter_span(MD_SPANTYPE type, void *detail, void *userdata)
{
	(void)detail;
	md_plain_ctx_t *ctx = (md_plain_ctx_t *)userdata;
	if (!ctx)
		return 1;

	switch (type) {
	case MD_SPAN_STRONG:
		ctx->strong_depth++;
		return apply_style(ctx);
	case MD_SPAN_EM:
		ctx->em_depth++;
		return apply_style(ctx);
	case MD_SPAN_CODE:
		ctx->code_span_depth++;
		return apply_style(ctx);
	case MD_SPAN_A:
		ctx->link_depth++;
		return apply_style(ctx);
	default:
		return 0;
	}
}

static int on_leave_span(MD_SPANTYPE type, void *detail, void *userdata)
{
	(void)detail;
	md_plain_ctx_t *ctx = (md_plain_ctx_t *)userdata;
	if (!ctx)
		return 1;

	switch (type) {
	case MD_SPAN_STRONG:
		if (ctx->strong_depth > 0)
			ctx->strong_depth--;
		return apply_style(ctx);
	case MD_SPAN_EM:
		if (ctx->em_depth > 0)
			ctx->em_depth--;
		return apply_style(ctx);
	case MD_SPAN_CODE:
		if (ctx->code_span_depth > 0)
			ctx->code_span_depth--;
		return apply_style(ctx);
	case MD_SPAN_A:
		if (ctx->link_depth > 0)
			ctx->link_depth--;
		return apply_style(ctx);
	default:
		return 0;
	}
}

static int on_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata)
{
	md_plain_ctx_t *ctx = (md_plain_ctx_t *)userdata;
	if (!ctx)
		return 1;
	if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR)
		return append_newline(ctx);
	if (!text || size == 0)
		return 0;
	if (!aicli_buf_append(&ctx->out, text, (size_t)size))
		return 1;
	return 0;
}

char *aicli_render_markdown_text(const char *input)
{
	if (!input)
		return NULL;

	md_plain_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.use_ansi = should_use_ansi();
	if (!aicli_buf_init(&ctx.out, 256))
		return dup_cstr_local(input);

	MD_PARSER parser;
	memset(&parser, 0, sizeof(parser));
	parser.abi_version = 0;
	parser.flags = MD_DIALECT_COMMONMARK;
	parser.enter_block = on_enter_block;
	parser.leave_block = on_leave_block;
	parser.enter_span = on_enter_span;
	parser.leave_span = on_leave_span;
	parser.text = on_text;

	int rc = md_parse((const MD_CHAR *)input, (MD_SIZE)strlen(input), &parser, &ctx);
	if (rc != 0) {
		aicli_buf_free(&ctx.out);
		return dup_cstr_local(input);
	}
	if (ctx.use_ansi && (ctx.style_bold || ctx.style_underline || ctx.style_color)) {
		if (!aicli_buf_append(&ctx.out, "\x1b[0m", 4)) {
			aicli_buf_free(&ctx.out);
			return dup_cstr_local(input);
		}
	}
	if (!aicli_buf_append(&ctx.out, "\0", 1)) {
		aicli_buf_free(&ctx.out);
		return dup_cstr_local(input);
	}
	return ctx.out.data;
}

#else

static char *dup_cstr_local(const char *s)
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

char *aicli_render_markdown_text(const char *input)
{
	return dup_cstr_local(input ? input : "");
}

#endif

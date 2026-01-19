#include "cli.h"

#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aicli.h"
#include "aicli_config.h"
#include "brave_search.h"
#include "execute_tool.h"

#if HAVE_YYJSON_H
#include <yyjson.h>
#endif

static const char *skip_ws(const char *s)
{
	if (!s)
		return "";
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	return s;
}

static void fprint_wrapped(FILE *out, const char *indent, const char *text,
			   size_t max_chars, size_t wrap_col)
{
	if (!out)
		return;
	if (!indent)
		indent = "";
	if (!text)
		text = "";
	if (wrap_col < 20)
		wrap_col = 80;

	const char *s = skip_ws(text);
	size_t printed = 0;
	size_t col = 0;
	int at_line_start = 1;

	while (*s && printed < max_chars) {
		// normalize whitespace to single space
		if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
			while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
				s++;
			if (!*s)
				break;
			if (col + 1 >= wrap_col) {
				fputc('\n', out);
				col = 0;
				at_line_start = 1;
			} else {
				if (at_line_start) {
					fputs(indent, out);
					col += strlen(indent);
					at_line_start = 0;
				}
				fputc(' ', out);
				col++;
				printed++;
			}
			continue;
		}

		if (at_line_start) {
			fputs(indent, out);
			col += strlen(indent);
			at_line_start = 0;
		}
		if (col + 1 >= wrap_col) {
			fputc('\n', out);
			col = 0;
			at_line_start = 1;
			continue;
		}

		fputc(*s, out);
		s++;
		col++;
		printed++;
	}

	if (*s) {
		if (col + 3 >= wrap_col) {
			fputc('\n', out);
			fputs(indent, out);
		}
		fputs("...", out);
	}
	fputc('\n', out);
}

static size_t detect_tty_width_or_default(size_t fallback)
{
	if (fallback < 20)
		fallback = 80;
	if (!isatty(STDOUT_FILENO))
		return fallback;

	struct winsize ws;
	memset(&ws, 0, sizeof(ws));
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0)
		return fallback;
	if (ws.ws_col < 20)
		return fallback;
	return (size_t)ws.ws_col;
}

static int cmd_exec_local(int argc, char **argv)
{
	// Internal helper for MVP testing:
	// aicli _exec --file PATH --start N --size N "cat PATH"
	aicli_allowed_file_t files[16];
	int file_count = 0;
	size_t start = 0;
	size_t size = 4096;

	int i = 2;
	while (i < argc && strncmp(argv[i], "--", 2) == 0) {
		if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
			if (file_count < 16) {
				char *rp = aicli_realpath_dup(argv[i + 1]);
				if (!rp) {
					fprintf(stderr, "invalid file: %s\n", argv[i + 1]);
					return 2;
				}
				files[file_count].path = rp;
				files[file_count].name = argv[i + 1];
				(void)aicli_get_file_size(rp, &files[file_count].size_bytes);
				file_count++;
			}
			i += 2;
			continue;
		}
		if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
			start = (size_t)strtoull(argv[i + 1], NULL, 10);
			i += 2;
			continue;
		}
		if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			size = (size_t)strtoull(argv[i + 1], NULL, 10);
			i += 2;
			continue;
		}
		break;
	}

	if (i >= argc) {
		fprintf(stderr, "missing command\n");
		return 2;
	}

	aicli_allowlist_t allow = {.files = files, .file_count = file_count};
	aicli_execute_request_t req = {
	    .command = argv[i],
	    .file = NULL,
	    .idempotency = NULL,
	    .start = start,
	    .size = size,
	};

	aicli_tool_result_t res;
	aicli_execute_run(&allow, &req, &res);
	if (res.stderr_text && res.stderr_text[0])
		fprintf(stderr, "%s\n", res.stderr_text);
	if (res.stdout_text)
		fwrite(res.stdout_text, 1, res.stdout_len, stdout);
	if (res.stdout_text)
		free((void *)res.stdout_text);
	for (int fi = 0; fi < file_count; fi++) {
		free((void *)files[fi].path);
	}

	if (res.has_next_start) {
		fprintf(stderr, "\n[total_bytes=%zu next_start=%zu]\n", res.total_bytes,
		        res.next_start);
	} else {
		fprintf(stderr, "\n[total_bytes=%zu]\n", res.total_bytes);
	}
	return res.exit_code;
}

static void usage(FILE *out)
{
	fprintf(out,
	        "aicli - lightweight native OpenAI client\n\n"
	        "Usage:\n"
	        "  aicli chat <prompt>\n"
	        "  aicli web search <query> [--count N] [--lang xx] [--freshness day|week|month] [--max-title N] [--max-url N] [--max-snippet N] [--width N] [--raw]\n"
	        "  aicli run [--auto-search] [--file PATH ...] <prompt>\n");
}

static int cmd_web_search(int argc, char **argv, const aicli_config_t *cfg)
{
	if (argc < 4) {
		fprintf(stderr, "missing query\n");
		return 2;
	}
	if (!cfg || !cfg->brave_api_key || !cfg->brave_api_key[0]) {
		fprintf(stderr, "BRAVE_API_KEY is required\n");
		return 2;
	}

	const char *query = argv[3];
	int count = 5;
	const char *lang = NULL;
	const char *freshness = NULL;
	int raw_json = 0;
	size_t max_title = 160;
	size_t max_url = 500;
	size_t max_snippet = 500;
	size_t width = 0;

	for (int i = 4; i < argc; i++) {
		if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
			count = atoi(argv[i + 1]);
			i++;
			continue;
		}
		if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
			lang = argv[i + 1];
			i++;
			continue;
		}
		if (strcmp(argv[i], "--freshness") == 0 && i + 1 < argc) {
			freshness = argv[i + 1];
			i++;
			continue;
		}
		if (strcmp(argv[i], "--raw") == 0) {
			raw_json = 1;
			continue;
		}
		if (strcmp(argv[i], "--max-title") == 0 && i + 1 < argc) {
			errno = 0;
			unsigned long long v = strtoull(argv[i + 1], NULL, 10);
			if (errno != 0) {
				fprintf(stderr, "invalid --max-title\n");
				return 2;
			}
			max_title = (size_t)v;
			if (max_title < 40)
				max_title = 40;
			if (max_title > 1000)
				max_title = 1000;
			i++;
			continue;
		}
		if (strcmp(argv[i], "--max-url") == 0 && i + 1 < argc) {
			errno = 0;
			unsigned long long v = strtoull(argv[i + 1], NULL, 10);
			if (errno != 0) {
				fprintf(stderr, "invalid --max-url\n");
				return 2;
			}
			max_url = (size_t)v;
			if (max_url < 40)
				max_url = 40;
			if (max_url > 5000)
				max_url = 5000;
			i++;
			continue;
		}
		if (strcmp(argv[i], "--max-snippet") == 0 && i + 1 < argc) {
			errno = 0;
			unsigned long long v = strtoull(argv[i + 1], NULL, 10);
			if (errno != 0) {
				fprintf(stderr, "invalid --max-snippet\n");
				return 2;
			}
			max_snippet = (size_t)v;
			if (max_snippet < 40)
				max_snippet = 40;
			if (max_snippet > 5000)
				max_snippet = 5000;
			i++;
			continue;
		}
		if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
			errno = 0;
			unsigned long long v = strtoull(argv[i + 1], NULL, 10);
			if (errno != 0) {
				fprintf(stderr, "invalid --width\n");
				return 2;
			}
			width = (size_t)v;
			if (width < 40)
				width = 40;
			if (width > 200)
				width = 200;
			i++;
			continue;
		}
		fprintf(stderr, "unknown option: %s\n", argv[i]);
		return 2;
	}

	if (width == 0)
		width = detect_tty_width_or_default(80);

	aicli_brave_response_t res;
	int rc = aicli_brave_web_search(cfg->brave_api_key, query, count, lang,
				 freshness, &res);
	if (rc != 0) {
		fprintf(stderr, "brave search failed: %s\n", res.error[0] ? res.error : "unknown");
		aicli_brave_response_free(&res);
		return 2;
	}

	if (res.http_status != 200) {
		fprintf(stderr, "brave http_status=%d\n", res.http_status);
		if (res.body && res.body_len)
			fwrite(res.body, 1, res.body_len, stdout);
		fputc('\n', stdout);
		aicli_brave_response_free(&res);
		return 1;
	}

	// Heuristic: small JSON is OK to print as-is; otherwise print a compact view.
	if (raw_json || res.body_len <= 4096) {
		if (res.body && res.body_len)
			fwrite(res.body, 1, res.body_len, stdout);
		fputc('\n', stdout);
		aicli_brave_response_free(&res);
		return 0;
	}

#if HAVE_YYJSON_H
	// If yyjson is available, extract and print a compact view.
	yyjson_read_err err;
	yyjson_doc *doc = yyjson_read(res.body, res.body_len, 0, NULL, &err);
	if (!doc) {
		fprintf(stderr, "yyjson parse error: %s at %zu\n", err.msg, (size_t)err.pos);
		// Fall back to truncation below.
	} else {
		yyjson_val *root = yyjson_doc_get_root(doc);
		yyjson_val *web = yyjson_obj_get(root, "web");
		yyjson_val *results = web ? yyjson_obj_get(web, "results") : NULL;
		if (!results || !yyjson_is_arr(results)) {
			fprintf(stderr, "unexpected JSON shape: missing web.results[]\n");
		} else {
			printf("# Brave Web Search\n");
			printf("query: %s\n\n", query);

			size_t idx, max;
			yyjson_val *it;
			max = yyjson_arr_size(results);
			if (max > (size_t)count)
				max = (size_t)count;
			for (idx = 0; idx < max; idx++) {
				it = yyjson_arr_get(results, idx);
				const char *title = NULL;
				const char *url = NULL;
				const char *desc = NULL;
				yyjson_val *v;
				v = yyjson_obj_get(it, "title");
				if (v && yyjson_is_str(v))
					title = yyjson_get_str(v);
				v = yyjson_obj_get(it, "url");
				if (v && yyjson_is_str(v))
					url = yyjson_get_str(v);
				v = yyjson_obj_get(it, "description");
				if (v && yyjson_is_str(v))
					desc = yyjson_get_str(v);

				if (!title)
					title = "";
				if (!url)
					url = "";
				if (!desc)
					desc = "";

				printf("%zu) ", idx + 1);
				fprint_wrapped(stdout, "", title, max_title, width);
				fprint_wrapped(stdout, "    ", url, max_url, width);
				fprint_wrapped(stdout, "    ", desc, max_snippet, width);
				fputc('\n', stdout);
			}
			yyjson_doc_free(doc);
			aicli_brave_response_free(&res);
			return 0;
		}
		yyjson_doc_free(doc);
	}
#endif

	// Fallback without a JSON parser: print the first ~4KB.
	size_t n = res.body_len;
	if (n > 4096)
		n = 4096;
	fwrite(res.body, 1, n, stdout);
	fprintf(stdout, "\n... (truncated, %zu bytes total; add --raw for full JSON)\n",
	        res.body_len);

	aicli_brave_response_free(&res);
	return 0;
}

int aicli_cli_main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stderr);
		return 2;
	}

	if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
		usage(stdout);
		return 0;
	}

	if (strcmp(argv[1], "_exec") == 0) {
		return cmd_exec_local(argc, argv);
	}

	aicli_config_t cfg;
	if (!aicli_config_load_from_env(&cfg)) {
		// It's OK for scaffold; real implementation will require OPENAI_API_KEY for
		// chat/run.
	}

	if (strcmp(argv[1], "web") == 0) {
		if (argc >= 3 && strcmp(argv[2], "search") == 0) {
			return cmd_web_search(argc, argv, &cfg);
		}
		fprintf(stderr, "unknown web subcommand\n");
		return 2;
	}

	// Stub scaffold: other subcommands will be wired in MVP implementation.
	fprintf(stderr, "scaffold: subcommands not implemented yet (%s)\n", argv[1]);
	fprintf(stderr, "See docs/design.md for the target spec.\n");

	return 0;
}

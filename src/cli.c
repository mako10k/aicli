
#include "cli.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aicli.h"
#include "aicli_config.h"
#include "aicli_config_file.h"
#include "auto_search.h"
#include "brave_search.h"
#include "google_search.h"
#include "execute_tool.h"
#include "openai_tool_loop.h"
#include "paging_cache.h"
#include "web_search_tool.h"
#include "web_fetch_tool.h"

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

static const char *skip_json_ws(const char *s, const char *end)
{
	while (s && s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
		s++;
	return s;
}

static const char *parse_json_string_value(const char *s, const char *end,
				      char *out, size_t out_cap)
{
	// Parses a JSON string starting at '"' and decodes a small subset of escapes.
	// Returns pointer just after closing quote, or NULL on failure.
	if (!s || s >= end || *s != '"' || !out || out_cap == 0)
		return NULL;
	s++;
	size_t oi = 0;
	while (s < end) {
		unsigned char c = (unsigned char)*s++;
		if (c == '"') {
			out[oi] = '\0';
			return s;
		}
		if (c == '\\') {
			if (s >= end)
				return NULL;
			unsigned char e = (unsigned char)*s++;
			switch (e) {
			case '"':
			case '\\':
			case '/':
				c = e;
				break;
			case 'b':
				c = '\b';
				break;
			case 'f':
				c = '\f';
				break;
			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			case 'u':
				// Skip \uXXXX without full Unicode decoding (best-effort).
				if (s + 3 >= end)
					return NULL;
				s += 4;
				c = '?';
				break;
			default:
				c = '?';
				break;
			}
		}
		if (oi + 1 < out_cap)
			out[oi++] = (char)c;
	}
	return NULL;
}

static int google_cse_print_formatted_from_json(const char *json, size_t json_len,
					const char *query, int count,
					size_t max_title, size_t max_url, size_t max_snippet,
					size_t width)
{
	if (!json || json_len == 0)
		return 1;
	const char *s = json;
	const char *end = json + json_len;

	const char *items = strstr(json, "\"items\"");
	if (!items)
		return 2;
	items = strstr(items, "[");
	if (!items)
		return 2;
	s = items + 1;

	printf("# Google Custom Search\n");
	printf("query: %s\n\n", query ? query : "");

	int printed = 0;
	while (s < end && printed < count) {
		const char *obj = strstr(s, "{");
		if (!obj)
			break;
		const char *obj_end = strstr(obj, "}");
		if (!obj_end)
			break;

		char title[1024] = {0};
		char link[2048] = {0};
		char snippet[2048] = {0};

		const char *p;
		p = strstr(obj, "\"title\"");
		if (p && p < obj_end) {
			p = strchr(p, ':');
			if (p && p < obj_end) {
				p++;
				p = skip_json_ws(p, obj_end);
				if (p && p < obj_end && *p == '"')
					parse_json_string_value(p, obj_end, title, sizeof(title));
			}
		}
		p = strstr(obj, "\"link\"");
		if (p && p < obj_end) {
			p = strchr(p, ':');
			if (p && p < obj_end) {
				p++;
				p = skip_json_ws(p, obj_end);
				if (p && p < obj_end && *p == '"')
					parse_json_string_value(p, obj_end, link, sizeof(link));
			}
		}
		p = strstr(obj, "\"snippet\"");
		if (p && p < obj_end) {
			p = strchr(p, ':');
			if (p && p < obj_end) {
				p++;
				p = skip_json_ws(p, obj_end);
				if (p && p < obj_end && *p == '"')
					parse_json_string_value(p, obj_end, snippet, sizeof(snippet));
			}
		}

		if (title[0] != '\0' || link[0] != '\0' || snippet[0] != '\0') {
			printed++;
			printf("%d) ", printed);
			fprint_wrapped(stdout, "", title, max_title, width);
			fprint_wrapped(stdout, "    ", link, max_url, width);
			fprint_wrapped(stdout, "    ", snippet, max_snippet, width);
			fputc('\n', stdout);
		}

		s = obj_end + 1;
	}

	return (printed > 0) ? 0 : 3;
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
	// aicli _exec [--file PATH ...] [--file - | --stdin] [--start N] [--size N] "CMD"
	// Notes:
	//  - Multiple files: repeat --file (e.g. --file A --file B). "--file A B" is NOT supported.
	//  - stdin: default when no --file is given, or explicitly via --stdin / --file -.
	//  - CMD may use '-' to refer to stdin; it will be rewritten to a temp file path.
	aicli_allowed_file_t files[16];
	int file_count = 0;
	size_t start = 0;
	size_t size = 4096;
	bool use_stdin = false;
	char stdin_tmp_path[256];
	stdin_tmp_path[0] = '\0';

	int i = 2;
	while (i < argc && strncmp(argv[i], "--", 2) == 0) {
		if (strcmp(argv[i], "--stdin") == 0) {
			use_stdin = true;
			i++;
			continue;
		}
		if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
			if (strcmp(argv[i + 1], "-") == 0) {
				use_stdin = true;
				i += 2;
				continue;
			}
			if (file_count < 16) {
				// Resolve relative paths against the current working directory so that
				// allowlist entries match what execute() will resolve (it uses realpath()).
				char *rp = NULL;
				if (argv[i + 1][0] == '/') {
					rp = aicli_realpath_dup(argv[i + 1]);
				} else {
					char cwd[4096];
					if (!getcwd(cwd, sizeof(cwd))) {
						fprintf(stderr, "failed to get cwd for --file\n");
						return 2;
					}
					char joined[8192];
					snprintf(joined, sizeof(joined), "%s/%s", cwd, argv[i + 1]);
					rp = aicli_realpath_dup(joined);
				}
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

	// If no files were specified, default to reading stdin.
	if (file_count == 0)
		use_stdin = true;

	// If stdin is in use, materialize it as a temp file and allowlist it.
	if (use_stdin) {
		if (snprintf(stdin_tmp_path, sizeof(stdin_tmp_path), "/tmp/aicli-stdin-%ld-XXXXXX",
		             (long)getpid()) <= 0) {
			fprintf(stderr, "failed to build stdin tempfile template\n");
			return 2;
		}
		int fd = mkstemp(stdin_tmp_path);
		if (fd < 0) {
			fprintf(stderr, "failed to create stdin tempfile\n");
			return 2;
		}

		size_t total = 0;
		char buf[8192];
		while (1) {
			ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
			if (r < 0) {
				close(fd);
				unlink(stdin_tmp_path);
				fprintf(stderr, "failed to read stdin\n");
				return 2;
			}
			if (r == 0)
				break;
			total += (size_t)r;
			if (total > (1024 * 1024)) {
				close(fd);
				unlink(stdin_tmp_path);
				fprintf(stderr, "stdin_too_large\n");
				return 4;
			}
			ssize_t off = 0;
			while (off < r) {
				ssize_t w = write(fd, buf + off, (size_t)(r - off));
				if (w <= 0) {
					close(fd);
					unlink(stdin_tmp_path);
					fprintf(stderr, "failed to write stdin tempfile\n");
					return 2;
				}
				off += w;
			}
		}
		close(fd);

		char *rp = aicli_realpath_dup(stdin_tmp_path);
		if (!rp) {
			unlink(stdin_tmp_path);
			fprintf(stderr, "invalid stdin tempfile path\n");
			return 2;
		}
		if (file_count < 16) {
			files[file_count].path = rp;
			files[file_count].name = "-";
			files[file_count].size_bytes = total;
			file_count++;
		} else {
			free(rp);
			unlink(stdin_tmp_path);
			fprintf(stderr, "too_many_files\n");
			return 2;
		}
	}

	if (i >= argc) {
		fprintf(stderr, "missing command\n");
		if (stdin_tmp_path[0])
			unlink(stdin_tmp_path);
		return 2;
	}

	const char *cmd = argv[i];
	char *cmd_dyn = NULL;
	if (use_stdin && stdin_tmp_path[0] && cmd) {
		// Replace standalone '-' tokens with the tempfile path.
		// Supports shapes like: "cat - | head -n 5".
		size_t src_len = strlen(cmd);
		size_t need = src_len + strlen(stdin_tmp_path) + 64;
		cmd_dyn = (char *)malloc(need);
		if (!cmd_dyn) {
			if (stdin_tmp_path[0])
				unlink(stdin_tmp_path);
			fprintf(stderr, "oom\n");
			return 1;
		}
		const char *s = cmd;
		char *d = cmd_dyn;
		while (*s) {
			bool at_start = (s == cmd);
			bool left_ok = at_start || s[-1] == ' ' || s[-1] == '\t' || s[-1] == '|';
			bool is_dash = (s[0] == '-' && (s[1] == '\0' || s[1] == ' ' || s[1] == '\t' || s[1] == '|'));
			if (left_ok && is_dash) {
				size_t n = strlen(stdin_tmp_path);
				memcpy(d, stdin_tmp_path, n);
				d += n;
				s += 1;
				continue;
			}
			*d++ = *s++;
		}
		*d = '\0';
		cmd = cmd_dyn;
	}

	aicli_allowlist_t allow = {.files = files, .file_count = file_count};
	aicli_execute_request_t req = {
	    .command = cmd,
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
	free(cmd_dyn);
	if (stdin_tmp_path[0])
		unlink(stdin_tmp_path);

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
	        "  aicli _exec [--file PATH ...] [--file - | --stdin] [--start N] [--size N] <cmd>\n"
	        "  aicli chat <prompt>\n"
	        "  aicli web search <query> [--count N] [--lang xx] [--freshness day|week|month] [--max-title N] [--max-url N] [--max-snippet N] [--width N] [--raw]\n"
	        "                    (note: --start/--size are available only with --raw)\n"
	        "  aicli web fetch <url> [--start N] [--size N]\n"
	        "  aicli run [--file PATH ...] [--file - | --stdin] [--turns N] [--max-tool-calls N] [--tool-threads N]\n"
	        "           [--disable-all-tools] [--available-tools TOOL[,TOOL...]] [--force-tool TOOL]\n"
	        "           [--config PATH] [--no-config]\n"
	        "           [--debug-all[=LEVEL]] [--debug-api[=LEVEL]] [--debug-function-call[=LEVEL]] [--auto-search] <prompt>\n"
	        "  aicli --list-tools\n"
	        "\n"
	        "Config (highest priority wins):\n"
	        "  1) command line options\n"
	        "  2) environment variables\n"
	        "  3) .aicli.json in $PWD (only if under $HOME)\n"
	        "  4) .aicli.json in parent dirs up to $HOME\n"
	        "  5) .aicli.json in $HOME\n"
	        "\n"
	        "Environment:\n"
	        "  AICLI_SEARCH_PROVIDER=google_cse|google|brave (default: google_cse)\n"
	        "  AICLI_WEB_FETCH_PREFIXES=prefix1,prefix2,... (enables web fetch allowlist)\n"
	        "  GOOGLE_API_KEY=...\n"
	        "  GOOGLE_CSE_CX=...\n"
	        "  BRAVE_API_KEY=... (when provider=brave)\n");
}

static void config_apply_env_overrides(aicli_config_t *cfg)
{
	if (!cfg)
		return;
	const char *v;

	v = getenv("OPENAI_API_KEY");
	if (v && v[0])
		cfg->openai_api_key = v;
	v = getenv("OPENAI_BASE_URL");
	if (v && v[0])
		cfg->openai_base_url = v;
	v = getenv("AICLI_MODEL");
	if (v && v[0])
		cfg->model = v;

	v = getenv("AICLI_SEARCH_PROVIDER");
	if (v && v[0]) {
		if (strcmp(v, "google") == 0 || strcmp(v, "google_cse") == 0)
			cfg->search_provider = AICLI_SEARCH_PROVIDER_GOOGLE_CSE;
		else if (strcmp(v, "brave") == 0)
			cfg->search_provider = AICLI_SEARCH_PROVIDER_BRAVE;
	}

	v = getenv("GOOGLE_API_KEY");
	if (v && v[0])
		cfg->google_api_key = v;
	v = getenv("GOOGLE_CSE_CX");
	if (v && v[0])
		cfg->google_cse_cx = v;
	v = getenv("BRAVE_API_KEY");
	if (v && v[0])
		cfg->brave_api_key = v;
}

static bool config_collect_cli_flags(int argc, char **argv, const char **out_config_path,
				     bool *out_no_config)
{
	if (out_config_path)
		*out_config_path = NULL;
	if (out_no_config)
		*out_no_config = false;
	if (!argv)
		return false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--no-config") == 0) {
			if (out_no_config)
				*out_no_config = true;
			continue;
		}
		if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
			if (out_config_path)
				*out_config_path = argv[i + 1];
			i++;
			continue;
		}
	}
	return true;
}

static bool load_config_with_precedence(aicli_config_t *cfg, int argc, char **argv)
{
	if (!cfg)
		return false;
	memset(cfg, 0, sizeof(*cfg));

	// Base: config file (lowest among our three layers here), then env, then CLI.
	// We'll load env first for backward compatibility, then overlay file, then re-overlay env.
	// Finally, command-line flags (handled by callers) override at point of use.
	(void)aicli_config_load_from_env(cfg);

	const char *config_path = NULL;
	bool no_config = false;
	config_collect_cli_flags(argc, argv, &config_path, &no_config);
	if (no_config)
		return true;

	aicli_config_file_t cf = {0};
	bool found = false;
	if (config_path && config_path[0]) {
		cf.path = aicli_realpath_dup(config_path);
		if (cf.path)
			cf.dir = NULL;
		found = (cf.path != NULL);
	} else {
		found = aicli_config_file_find(&cf);
	}

	if (found) {
		// Apply file values, then re-apply env overrides to keep precedence env > file.
		(void)aicli_config_load_from_file(cfg, &cf);
		aicli_config_file_free(&cf);
		config_apply_env_overrides(cfg);
	}
	return true;
}

static int parse_optional_level(const char *opt, const char *next, bool has_next, int default_level,
			       int *out_level, int *out_consumed_next)
{
	// Supports:
	//  --flag           => default_level
	//  --flag=NUM       => NUM
	//  --flag NUM       => NUM
	// Returns 0 on success, non-zero on parse error.
	if (!out_level || !out_consumed_next)
		return 1;
	*out_consumed_next = 0;
	*out_level = default_level;

	if (!opt)
		return 1;

	const char *eq = strchr(opt, '=');
	if (eq && eq[1]) {
		errno = 0;
		long v = strtol(eq + 1, NULL, 10);
		if (errno != 0 || v < 0 || v > 10)
			return 1;
		*out_level = (int)v;
		return 0;
	}

	if (has_next && next && next[0] && next[0] != '-') {
		errno = 0;
		long v = strtol(next, NULL, 10);
		if (errno != 0 || v < 0 || v > 10)
			return 1;
		*out_level = (int)v;
		*out_consumed_next = 1;
		return 0;
	}

	return 0;
}

static int cmd_list_tools(void)
{
	printf("Available tools:\n");
	printf("  execute\n");
	printf("\n");
	printf("Notes:\n");
	printf("  - execute is read-only and limited to allowlisted files.\n");
	printf("  - Use aicli run --file PATH to allow a file for execute.\n");
	return 0;
}

static int cmd_run(int argc, char **argv, const aicli_config_t *cfg);

static int cmd_chat(int argc, char **argv, const aicli_config_t *cfg)
{
	// aicli chat <prompt>
	if (argc < 3) {
		fprintf(stderr, "missing prompt\n");
		return 2;
	}

	// Reuse the run pipeline but force tools off by default.
	// Equivalent to: aicli run --turns 1 --max-tool-calls 1 --tool-threads 1 <prompt>
	char *args[12];
	int n = 0;
	args[n++] = argv[0];
	args[n++] = "run";
	args[n++] = "--turns";
	args[n++] = "1";
	args[n++] = "--max-tool-calls";
	args[n++] = "1";
	args[n++] = "--tool-threads";
	args[n++] = "1";
	args[n++] = argv[2];
	args[n] = NULL;
	return cmd_run(n, args, cfg);
}

static const char *first_nonempty_env(const char *a, const char *b, const char *c)
{
	const char *v = NULL;
	if (a)
		v = getenv(a);
	if (v && v[0])
		return v;
	if (b)
		v = getenv(b);
	if (v && v[0])
		return v;
	if (c)
		v = getenv(c);
	if (v && v[0])
		return v;
	return NULL;
}

static int locale_to_google_lr(const char *locale, char out[32])
{
	if (!out)
		return 1;
	out[0] = '\0';
	if (!locale || !locale[0])
		return 1;

	// Accept formats like: ja_JP.UTF-8, ja-JP, ja, C, POSIX
	if (strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0)
		return 1;

	const char *p = locale;
	char lang2[3] = {0};
	int n = 0;
	while (*p && n < 2) {
		char ch = *p;
		if (ch == '_' || ch == '-' || ch == '.')
			break;
		if ((ch >= 'A' && ch <= 'Z'))
			ch = (char)(ch - 'A' + 'a');
		if (!(ch >= 'a' && ch <= 'z'))
			return 1;
		lang2[n++] = ch;
		p++;
	}
	if (n != 2)
		return 1;

	snprintf(out, 32, "lang_%s", lang2);
	return 0;
}

static int cmd_run(int argc, char **argv, const aicli_config_t *cfg)
{
	// aicli run [--file PATH ...] [--file - | --stdin]
	//          [--turns N] [--max-tool-calls N] [--tool-threads N] [--auto-search] <prompt>
	if (!cfg || !cfg->openai_api_key || !cfg->openai_api_key[0]) {
		fprintf(stderr, "OPENAI_API_KEY is required\n");
		return 2;
	}

	aicli_allowed_file_t files[32];
	int file_count = 0;
	// Keep a simple owned array of realpath strings for the tool allowlist.
	// `files[].path` may be freed later; the tool loop must retain valid pointers.
	char *allow_paths[32];
	memset(allow_paths, 0, sizeof(allow_paths));
	bool auto_search = false;
	bool use_stdin = false;
	char stdin_tmp_path[256];
	stdin_tmp_path[0] = '\0';
	const char *available_tools = NULL;
	const char *force_tool = NULL;
	int disable_all_tools = 0;
	int debug_api = 0;
	int debug_function_call = 0;
	size_t turns = 4;
	size_t max_tool_calls = 8;
	size_t tool_threads = 1;

	int i = 2;
	while (i < argc && strncmp(argv[i], "--", 2) == 0) {
		if (strcmp(argv[i], "--stdin") == 0) {
			use_stdin = true;
			i += 1;
			continue;
		}
		if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
			if (strcmp(argv[i + 1], "-") == 0) {
				use_stdin = true;
				i += 2;
				continue;
			}
			if (file_count >= (int)(sizeof(files) / sizeof(files[0]))) {
				fprintf(stderr, "too many --file entries (max %zu)\n",
				        sizeof(files) / sizeof(files[0]));
				return 2;
			}
			char *rp = NULL;
			if (argv[i + 1][0] == '/') {
				rp = aicli_realpath_dup(argv[i + 1]);
			} else {
				char cwd[4096];
				if (!getcwd(cwd, sizeof(cwd))) {
					fprintf(stderr, "failed to get cwd for --file\n");
					return 2;
				}
				char joined[8192];
				snprintf(joined, sizeof(joined), "%s/%s", cwd, argv[i + 1]);
				rp = aicli_realpath_dup(joined);
			}
			if (!rp) {
				fprintf(stderr, "invalid file: %s\n", argv[i + 1]);
				return 2;
			}
			files[file_count].path = rp;
			allow_paths[file_count] = rp;
			files[file_count].name = argv[i + 1];
			(void)aicli_get_file_size(rp, &files[file_count].size_bytes);
			file_count++;
			i += 2;
			continue;
		}
		if (strcmp(argv[i], "--turns") == 0 && i + 1 < argc) {
			errno = 0;
			unsigned long long v = strtoull(argv[i + 1], NULL, 10);
			if (errno != 0 || v == 0 || v > 32) {
				fprintf(stderr, "invalid --turns (1..32)\n");
				return 2;
			}
			turns = (size_t)v;
			i += 2;
			continue;
		}
		if (strcmp(argv[i], "--max-tool-calls") == 0 && i + 1 < argc) {
			errno = 0;
			unsigned long long v = strtoull(argv[i + 1], NULL, 10);
			if (errno != 0 || v == 0 || v > 64) {
				fprintf(stderr, "invalid --max-tool-calls (1..64)\n");
				return 2;
			}
			max_tool_calls = (size_t)v;
			i += 2;
			continue;
		}
		if (strcmp(argv[i], "--tool-threads") == 0 && i + 1 < argc) {
			errno = 0;
			unsigned long long v = strtoull(argv[i + 1], NULL, 10);
			if (errno != 0 || v == 0 || v > 64) {
				fprintf(stderr, "invalid --tool-threads (1..64)\n");
				return 2;
			}
			tool_threads = (size_t)v;
			i += 2;
			continue;
		}
		if (strcmp(argv[i], "--disable-all-tools") == 0) {
			disable_all_tools = 1;
			i += 1;
			continue;
		}
		if (strcmp(argv[i], "--available-tools") == 0 && i + 1 < argc) {
			available_tools = argv[i + 1];
			i += 2;
			continue;
		}
		if (strcmp(argv[i], "--force-tool") == 0 && i + 1 < argc) {
			force_tool = argv[i + 1];
			// Responses API currently supports tool_choice: none|auto|required.
			if (!(strcmp(force_tool, "none") == 0 || strcmp(force_tool, "auto") == 0 ||
			      strcmp(force_tool, "required") == 0)) {
				fprintf(stderr,
				        "invalid --force-tool value: %s (supported: none|auto|required)\n",
				        force_tool);
				return 2;
			}
			i += 2;
			continue;
		}
		if (strcmp(argv[i], "--debug-api") == 0 || strncmp(argv[i], "--debug-api=", 12) == 0) {
			int level = 1;
			int consumed = 0;
			if (parse_optional_level(argv[i], (i + 1 < argc) ? argv[i + 1] : NULL, (i + 1 < argc), 1,
						 &level, &consumed) != 0) {
				fprintf(stderr, "invalid --debug-api level (0..10)\n");
				return 2;
			}
			debug_api = level;
			i += 1 + consumed;
			continue;
		}
		if (strcmp(argv[i], "--debug-function-call") == 0 ||
		    strncmp(argv[i], "--debug-function-call=", 22) == 0) {
			int level = 1;
			int consumed = 0;
			if (parse_optional_level(argv[i], (i + 1 < argc) ? argv[i + 1] : NULL, (i + 1 < argc), 1,
						 &level, &consumed) != 0) {
				fprintf(stderr, "invalid --debug-function-call level (0..10)\n");
				return 2;
			}
			debug_function_call = level;
			i += 1 + consumed;
			continue;
		}
		if (strcmp(argv[i], "--debug-all") == 0 || strncmp(argv[i], "--debug-all=", 12) == 0) {
			int level = 1;
			int consumed = 0;
			if (parse_optional_level(argv[i], (i + 1 < argc) ? argv[i + 1] : NULL, (i + 1 < argc), 1,
						 &level, &consumed) != 0) {
				fprintf(stderr, "invalid --debug-all level (0..10)\n");
				return 2;
			}
			debug_api = level;
			debug_function_call = level;
			i += 1 + consumed;
			continue;
		}
		if (strcmp(argv[i], "--auto-search") == 0) {
			auto_search = true;
			i += 1;
			continue;
		}
		fprintf(stderr, "unknown option: %s\n", argv[i]);
		return 2;
	}

	if (i >= argc) {
		fprintf(stderr, "missing prompt\n");
		return 2;
	}
	const char *prompt = argv[i];

	// stdin -> temp file -> allowlist
	if (use_stdin) {
		if (file_count >= (int)(sizeof(files) / sizeof(files[0]))) {
			fprintf(stderr, "too many --file entries (max %zu)\n",
			        sizeof(files) / sizeof(files[0]));
			return 2;
		}
		if (snprintf(stdin_tmp_path, sizeof(stdin_tmp_path), "/tmp/aicli-stdin-%ld-XXXXXX",
		             (long)getpid()) <= 0) {
			fprintf(stderr, "failed to build stdin tempfile template\n");
			return 2;
		}
		int fd = mkstemp(stdin_tmp_path);
		if (fd < 0) {
			fprintf(stderr, "failed to create stdin tempfile\n");
			return 2;
		}
		size_t total = 0;
		char buf[8192];
		while (1) {
			ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
			if (r < 0) {
				close(fd);
				unlink(stdin_tmp_path);
				fprintf(stderr, "failed to read stdin\n");
				return 2;
			}
			if (r == 0)
				break;
			total += (size_t)r;
			if (total > (1024 * 1024)) {
				close(fd);
				unlink(stdin_tmp_path);
				fprintf(stderr, "stdin_too_large\n");
				return 4;
			}
			ssize_t off = 0;
			while (off < r) {
				ssize_t w = write(fd, buf + off, (size_t)(r - off));
				if (w <= 0) {
					close(fd);
					unlink(stdin_tmp_path);
					fprintf(stderr, "failed to write stdin tempfile\n");
					return 2;
				}
				off += w;
			}
		}
		close(fd);

		char *rp = aicli_realpath_dup(stdin_tmp_path);
		if (!rp) {
			unlink(stdin_tmp_path);
			fprintf(stderr, "invalid stdin tempfile path\n");
			return 2;
		}
		files[file_count].path = rp;
		allow_paths[file_count] = rp;
		files[file_count].name = "-";
		files[file_count].size_bytes = total;
		file_count++;
	}

	char *augmented_prompt = NULL;
	if (auto_search) {
		char *query = NULL;
		bool will_search = aicli_auto_search_plan(cfg, prompt, &query);
		if (!will_search) {
			free(query);
		} else {
			// Search (provider-aware)
			int is_google = (cfg->search_provider == AICLI_SEARCH_PROVIDER_GOOGLE_CSE);
			int is_brave = (cfg->search_provider == AICLI_SEARCH_PROVIDER_BRAVE);
			char *summary = NULL;

			if (is_google) {
				aicli_google_response_t gres;
				int src = aicli_google_cse_search(cfg->google_api_key, cfg->google_cse_cx,
				                                 query, 5,
				                                 NULL, &gres);
				if (src != 0 || gres.http_status != 200 || !gres.body) {
					fprintf(stderr, "google cse search failed; continuing without search\n");
					aicli_google_response_free(&gres);
					free(query);
				} else {
						// Build a compact search summary using yyjson when available.
#if HAVE_YYJSON_H
						{
							yyjson_doc *doc = yyjson_read(gres.body, gres.body_len, 0);
							if (doc) {
								yyjson_val *root = yyjson_doc_get_root(doc);
								yyjson_val *items = root ? yyjson_obj_get(root, "items") : NULL;
								yyjson_val *results = items;
								if (results && yyjson_is_arr(results)) {
									yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
									yyjson_mut_val *arr = yyjson_mut_arr(md);
									yyjson_mut_doc_set_root(md, arr);
									// We store an array of objects {title,url,description}
									size_t max = yyjson_arr_size(results);
									if (max > 5)
										max = 5;
									for (size_t ri = 0; ri < max; ri++) {
										yyjson_val *it = yyjson_arr_get(results, ri);
										if (!it || !yyjson_is_obj(it))
											continue;
										const char *title = NULL;
										const char *url = NULL;
										const char *desc = NULL;
										yyjson_val *v;
										v = yyjson_obj_get(it, "title");
										if (v && yyjson_is_str(v))
											title = yyjson_get_str(v);
										v = yyjson_obj_get(it, "link");
										if (v && yyjson_is_str(v))
											url = yyjson_get_str(v);
										v = yyjson_obj_get(it, "snippet");
										if (v && yyjson_is_str(v))
											desc = yyjson_get_str(v);

										yyjson_mut_val *o = yyjson_mut_obj(md);
										yyjson_mut_obj_add_str(md, o, "title", title ? title : "");
										yyjson_mut_obj_add_str(md, o, "url", url ? url : "");
										yyjson_mut_obj_add_str(md, o, "description", desc ? desc : "");
										yyjson_mut_arr_add_val(arr, o);
									}

									char *json = yyjson_mut_write(md, 0, NULL);
									yyjson_mut_doc_free(md);
									if (json) {
										const char *hdr = "SEARCH_RESULTS:\n";
										size_t need = strlen(hdr) + strlen(json) + 2;
										summary = (char *)malloc(need);
										if (summary)
											snprintf(summary, need, "%s%s\n", hdr, json);
										free(json);
									}
								}
							yyjson_doc_free(doc);
						}
					}
#endif
					if (!summary && gres.body && gres.body_len) {
						// Fallback: include truncated raw JSON.
						size_t n = gres.body_len;
						if (n > 2048)
							n = 2048;
						const char *hdr = "SEARCH_RESULTS_RAW_TRUNCATED:\n";
						size_t need = strlen(hdr) + n + 2;
						summary = (char *)malloc(need);
						if (summary) {
							memcpy(summary, hdr, strlen(hdr));
							memcpy(summary + strlen(hdr), gres.body, n);
							summary[strlen(hdr) + n] = '\n';
							summary[strlen(hdr) + n + 1] = '\0';
						}
					}
					aicli_google_response_free(&gres);
					free(query);
					if (summary) {
						// Prepend the search results to the prompt.
						size_t need = strlen(summary) + strlen(prompt) + 2;
						augmented_prompt = (char *)malloc(need);
						if (augmented_prompt)
							snprintf(augmented_prompt, need, "%s\n%s", summary, prompt);
						free(summary);
					}
				}
			} else if (is_brave) {
				if (!cfg->brave_api_key || !cfg->brave_api_key[0]) {
					fprintf(stderr, "BRAVE_API_KEY is not set; continuing without search\n");
					free(query);
				} else {
					aicli_brave_response_t sres;
					int src = aicli_brave_web_search(cfg->brave_api_key, query, 5, NULL, NULL, &sres);
					if (src != 0 || sres.http_status != 200 || !sres.body) {
						fprintf(stderr, "brave search failed; continuing without search\n");
						aicli_brave_response_free(&sres);
						free(query);
					} else {
						// Build a compact search summary using yyjson when available.
						{
#if HAVE_YYJSON_H
							yyjson_doc *doc = yyjson_read(sres.body, sres.body_len, 0);
							if (doc) {
								yyjson_val *root = yyjson_doc_get_root(doc);
								yyjson_val *web = root ? yyjson_obj_get(root, "web") : NULL;
								yyjson_val *results = web ? yyjson_obj_get(web, "results") : NULL;
								if (results && yyjson_is_arr(results)) {
									yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
									yyjson_mut_val *arr = yyjson_mut_arr(md);
									yyjson_mut_doc_set_root(md, arr);
									// We store an array of objects {title,url,description}
									size_t max = yyjson_arr_size(results);
									if (max > 5)
										max = 5;
									for (size_t ri = 0; ri < max; ri++) {
										yyjson_val *it = yyjson_arr_get(results, ri);
										if (!it || !yyjson_is_obj(it))
											continue;
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

										yyjson_mut_val *o = yyjson_mut_obj(md);
										yyjson_mut_obj_add_str(md, o, "title", title ? title : "");
										yyjson_mut_obj_add_str(md, o, "url", url ? url : "");
										yyjson_mut_obj_add_str(md, o, "description", desc ? desc : "");
										yyjson_mut_arr_add_val(arr, o);
									}

									char *json = yyjson_mut_write(md, 0, NULL);
									yyjson_mut_doc_free(md);
									if (json) {
										const char *hdr = "SEARCH_RESULTS:\n";
										size_t need = strlen(hdr) + strlen(json) + 2;
										summary = (char *)malloc(need);
										if (summary)
											snprintf(summary, need, "%s%s\n", hdr, json);
										free(json);
									}
								}
							}
							yyjson_doc_free(doc);
							}
#endif
							if (!summary && sres.body && sres.body_len) {
								// Fallback: include truncated raw JSON.
								size_t n = sres.body_len;
								if (n > 2048)
									n = 2048;
								const char *hdr = "SEARCH_RESULTS_RAW_TRUNCATED:\n";
								size_t need = strlen(hdr) + n + 2;
								summary = (char *)malloc(need);
								if (summary) {
									memcpy(summary, hdr, strlen(hdr));
									memcpy(summary + strlen(hdr), sres.body, n);
									summary[strlen(hdr) + n] = '\n';
									summary[strlen(hdr) + n + 1] = '\0';
								}
							}
						}

						aicli_brave_response_free(&sres);
						free(query);
						if (summary) {
							// Prepend the search results to the prompt.
							size_t need = strlen(summary) + strlen(prompt) + 2;
							augmented_prompt = (char *)malloc(need);
							if (augmented_prompt)
								snprintf(augmented_prompt, need, "%s\n%s", summary, prompt);
							free(summary);
						}
					}
				}
			} else {
				fprintf(stderr, "unknown search provider; continuing without search\n");
				free(query);
			}
		}
	}

	aicli_allowed_file_t allow_files[32];
	for (int ai = 0; ai < file_count; ai++) {
		allow_files[ai].path = allow_paths[ai];
		allow_files[ai].name = files[ai].name;
		allow_files[ai].size_bytes = files[ai].size_bytes;
	}
	aicli_allowlist_t allow = {.files = allow_files, .file_count = file_count};
	char *final_text = NULL;
	const char *to_send = augmented_prompt ? augmented_prompt : prompt;
	// tool_choice semantics (Responses API): "none" disables, "auto" lets model decide,
	// or force a specific tool by name.
	const char *tool_choice = NULL;
	if (disable_all_tools)
		tool_choice = "none";
	else if (force_tool && force_tool[0])
		tool_choice = force_tool;

	// available_tools: for now we only support "execute".
	if (available_tools && available_tools[0]) {
		if (strcmp(available_tools, "execute") != 0) {
			fprintf(stderr, "unsupported --available-tools (only: execute)\n");
			free(augmented_prompt);
			for (int fi = 0; fi < file_count; fi++)
				free((void *)files[fi].path);
			return 2;
		}
	}

	aicli_config_t cfg_local;
	memcpy(&cfg_local, cfg, sizeof(cfg_local));
	cfg_local.debug_api = debug_api;
	cfg_local.debug_function_call = debug_function_call;
	int rc = aicli_openai_run_with_tools(&cfg_local, &allow, to_send, turns, max_tool_calls,
	                                   tool_threads, tool_choice, &final_text);
	free(augmented_prompt);
	// Free allowlisted paths after the tool loop finishes.
	for (int fi = 0; fi < file_count; fi++)
		free(allow_paths[fi]);
	if (stdin_tmp_path[0])
		unlink(stdin_tmp_path);

	if (rc != 0) {
		fprintf(stderr, "openai request failed\n");
		free(final_text);
		return 2;
	}

	if (final_text && final_text[0]) {
		fputs(final_text, stdout);
		fputc('\n', stdout);
		free(final_text);
		return 0;
	}

	free(final_text);
	// Should be unreachable: openai_tool_loop returns non-zero if it can't extract output.
	fprintf(stderr, "openai response had no output_text\n");
	return 2;
}

static int cmd_web_search(int argc, char **argv, const aicli_config_t *cfg)
{
	if (argc < 4) {
		fprintf(stderr, "missing query\n");
		return 2;
	}
	if (!cfg) {
		fprintf(stderr, "config is missing\n");
		return 2;
	}

	const char *query = argv[3];
	int count = 5;
	const char *lang = NULL;
	const char *freshness = NULL;
	size_t start = 0;
	size_t size = 4096;
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
		if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
			errno = 0;
			unsigned long long v = strtoull(argv[i + 1], NULL, 10);
			if (errno != 0) {
				fprintf(stderr, "invalid --start\n");
				return 2;
			}
			start = (size_t)v;
			i++;
			continue;
		}
		if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			errno = 0;
			unsigned long long v = strtoull(argv[i + 1], NULL, 10);
			if (errno != 0) {
				fprintf(stderr, "invalid --size\n");
				return 2;
			}
			size = (size_t)v;
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

	// A方針: コマンドライン直叩きは pretty がデフォルト。
	// --raw の時だけ tool 経由（ページング/キャッシュ）を有効にする。
	if (!raw_json) {
		// Language precedence: --lang > system locale > unset
		const char *effective_lang = lang;
		if (!effective_lang || !effective_lang[0])
			effective_lang = first_nonempty_env("LC_ALL", "LC_MESSAGES", "LANG");

		if (width == 0)
			width = detect_tty_width_or_default(80);

		// Provider-aware search
		int is_google = (cfg->search_provider == AICLI_SEARCH_PROVIDER_GOOGLE_CSE);
		int is_brave = (cfg->search_provider == AICLI_SEARCH_PROVIDER_BRAVE);

		if (is_google) {
			char lr_buf[32];
			const char *lr = NULL;
			if (effective_lang && effective_lang[0]) {
				if (locale_to_google_lr(effective_lang, lr_buf) == 0)
					lr = lr_buf;
			}

			aicli_google_response_t gres;
			int rc = aicli_google_cse_search(cfg->google_api_key, cfg->google_cse_cx,
			                               query, count,
			                               lr, &gres);
			if (rc != 0) {
				fprintf(stderr, "google cse search failed: %s\n",
				        gres.error[0] ? gres.error : "unknown");
				aicli_google_response_free(&gres);
				return 2;
			}

			if (gres.http_status != 200) {
				fprintf(stderr, "google http_status=%d\n", gres.http_status);
				if (gres.body && gres.body_len)
					fwrite(gres.body, 1, gres.body_len, stdout);
				fputc('\n', stdout);
				aicli_google_response_free(&gres);
				return 1;
			}

			if (google_cse_print_formatted_from_json(gres.body, gres.body_len, query,
							        count, max_title, max_url, max_snippet, width) == 0) {
				aicli_google_response_free(&gres);
				return 0;
			}

			// Fallback: print the first ~4KB.
			size_t n = gres.body_len;
			if (n > 4096)
				n = 4096;
			fwrite(gres.body, 1, n, stdout);
			fprintf(stdout,
			        "\n... (truncated, %zu bytes total; add --raw for full JSON)\n",
			        gres.body_len);
			aicli_google_response_free(&gres);
			return 0;
		}

		if (is_brave) {
			if (!cfg->brave_api_key || !cfg->brave_api_key[0]) {
				fprintf(stderr, "BRAVE_API_KEY is required (provider=brave)\n");
				return 2;
			}
			aicli_brave_response_t bres;
			int rc = aicli_brave_web_search(cfg->brave_api_key, query, count, lang,
				                  freshness, &bres);
			if (rc != 0) {
				fprintf(stderr, "brave search failed: %s\n",
				        bres.error[0] ? bres.error : "unknown");
				aicli_brave_response_free(&bres);
				return 2;
			}

			if (bres.http_status != 200) {
				fprintf(stderr, "brave http_status=%d\n", bres.http_status);
				if (bres.body && bres.body_len)
					fwrite(bres.body, 1, bres.body_len, stdout);
				fputc('\n', stdout);
				aicli_brave_response_free(&bres);
				return 1;
			}

			// If yyjson is available, extract and print a compact view.
			// Otherwise, print the first ~4KB (best-effort).
			if (bres.body_len <= 4096) {
				if (bres.body && bres.body_len)
					fwrite(bres.body, 1, bres.body_len, stdout);
				fputc('\n', stdout);
				aicli_brave_response_free(&bres);
				return 0;
			}

#if HAVE_YYJSON_H
			yyjson_doc *doc = yyjson_read(bres.body, bres.body_len, 0);
			if (!doc) {
				fprintf(stderr, "yyjson parse error\n");
				// Fall back to truncation below.
			} else {
				yyjson_val *root = yyjson_doc_get_root(doc);
				yyjson_val *web = yyjson_obj_get(root, "web");
				yyjson_val *results = web ? yyjson_obj_get(web, "results") : NULL;
				if (!results || !yyjson_is_arr(results)) {
					fprintf(stderr,
					        "unexpected JSON shape: missing web.results[]\n");
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
						const char *url2 = NULL;
						const char *desc = NULL;
						yyjson_val *v;
						v = yyjson_obj_get(it, "title");
						if (v && yyjson_is_str(v))
							title = yyjson_get_str(v);
						v = yyjson_obj_get(it, "url");
						if (v && yyjson_is_str(v))
							url2 = yyjson_get_str(v);
						v = yyjson_obj_get(it, "description");
						if (v && yyjson_is_str(v))
							desc = yyjson_get_str(v);

						if (!title)
							title = "";
						if (!url2)
							url2 = "";
						if (!desc)
							desc = "";

						printf("%zu) ", idx + 1);
						fprint_wrapped(stdout, "", title, max_title, width);
						fprint_wrapped(stdout, "    ", url2, max_url, width);
						fprint_wrapped(stdout, "    ", desc, max_snippet, width);
						fputc('\n', stdout);
					}
					yyjson_doc_free(doc);
					aicli_brave_response_free(&bres);
					return 0;
				}
				yyjson_doc_free(doc);
			}
#endif

			size_t n = bres.body_len;
			if (n > 4096)
				n = 4096;
			fwrite(bres.body, 1, n, stdout);
			fprintf(stdout,
			        "\n... (truncated, %zu bytes total; add --raw for full JSON)\n",
			        bres.body_len);

			aicli_brave_response_free(&bres);
			return 0;
		}

		fprintf(stderr, "unknown search provider\n");
		return 2;
	}

	// --raw: tool 経由でページング/キャッシュ
	aicli_paging_cache_t *cache = aicli_paging_cache_create(64);
	if (!cache) {
		fprintf(stderr, "out of memory\n");
		return 2;
	}

	aicli_web_search_tool_request_t req = {0};
	req.query = query;
	req.count = count;
	req.lang = lang;
	req.freshness = freshness;
	req.raw = raw_json ? true : false;
	req.start = start;
	req.size = size;

	aicli_tool_result_t res = {0};
	int rc = aicli_web_search_tool_run(cfg, cache, &req, &res);
	if (rc != 0) {
		fprintf(stderr, "web search failed\n");
		aicli_paging_cache_destroy(cache);
		return rc;
	}

	if (res.stdout_text && res.stdout_text[0]) {
		fputs(res.stdout_text, stdout);
		fputc('\n', stdout);
	}

	if (res.truncated)
		fprintf(stderr, "(truncated; next_start=%zu)\n", res.next_start);

	if (res.stdout_text)
		free((void *)res.stdout_text);
	aicli_paging_cache_destroy(cache);
	return 0;
}

static int cmd_web_fetch(int argc, char **argv, const aicli_config_t *cfg)
{
	if (argc < 4) {
		fprintf(stderr, "missing url\n");
		return 2;
	}
	if (!cfg) {
		fprintf(stderr, "config is missing\n");
		return 2;
	}

	const char *url = argv[3];
	size_t start = 0;
	size_t size = 4096;

	for (int i = 4; i < argc; i++) {
		if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
			errno = 0;
			unsigned long long v = strtoull(argv[i + 1], NULL, 10);
			if (errno != 0) {
				fprintf(stderr, "invalid --start\n");
				return 2;
			}
			start = (size_t)v;
			i++;
			continue;
		}
		if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			errno = 0;
			unsigned long long v = strtoull(argv[i + 1], NULL, 10);
			if (errno != 0) {
				fprintf(stderr, "invalid --size\n");
				return 2;
			}
			size = (size_t)v;
			i++;
			continue;
		}
		fprintf(stderr, "unknown option: %s\n", argv[i]);
		return 2;
	}

	// URL allowlist prefixes come from env var. Without it, web fetch is disabled.
	const char *prefixes_env = getenv("AICLI_WEB_FETCH_PREFIXES");
	const char *prefixes[32];
	size_t prefix_count = 0;
	char *prefixes_buf = NULL;
	if (prefixes_env && prefixes_env[0]) {
		prefixes_buf = strdup(prefixes_env);
		if (!prefixes_buf) {
			fprintf(stderr, "out of memory\n");
			return 2;
		}
		char *p = prefixes_buf;
		while (p && *p) {
			while (*p == ' ' || *p == '\t' || *p == ',')
				p++;
			if (!*p)
				break;
			if (prefix_count >= (sizeof(prefixes) / sizeof(prefixes[0])))
				break;
			prefixes[prefix_count++] = p;
			char *comma = strchr(p, ',');
			if (!comma)
				break;
			*comma = '\0';
			p = comma + 1;
		}
	}

	aicli_paging_cache_t *cache = aicli_paging_cache_create(64);
	if (!cache) {
		fprintf(stderr, "out of memory\n");
		free(prefixes_buf);
		return 2;
	}

	aicli_web_fetch_tool_request_t req = {0};
	req.url = url;
	req.start = start;
	req.size = size;
	req.allowed_prefixes = prefixes;
	req.allowed_prefix_count = prefix_count;
	req.max_body_bytes = 1024 * 1024;
	req.timeout_seconds = 15L;
	req.connect_timeout_seconds = 10L;
	req.max_redirects = 0;

	aicli_tool_result_t res = {0};
	int rc = aicli_web_fetch_tool_run(cfg, cache, &req, &res);
	if (rc != 0) {
		fprintf(stderr, "web fetch failed\n");
		aicli_paging_cache_destroy(cache);
		free(prefixes_buf);
		return rc;
	}
	if (res.exit_code == 3 && (!prefixes_env || !prefixes_env[0])) {
		fprintf(stderr,
		        "web fetch is disabled by default. Set AICLI_WEB_FETCH_PREFIXES, e.g.:\n"
		        "  AICLI_WEB_FETCH_PREFIXES='https://example.com/,https://docs.example.com/'\n");
	}
	if (res.exit_code != 0 && res.stderr_text && res.stderr_text[0]) {
		fprintf(stderr, "%s\n", res.stderr_text);
	}

	if (res.stdout_text && res.stdout_text[0]) {
		fputs(res.stdout_text, stdout);
		fputc('\n', stdout);
	}

	if (res.truncated)
		fprintf(stderr, "(truncated; next_start=%zu)\n", res.next_start);

	if (res.stdout_text)
		free((void *)res.stdout_text);
	aicli_paging_cache_destroy(cache);
	free(prefixes_buf);
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

	if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
#ifdef PACKAGE_STRING
		printf("%s\n", PACKAGE_STRING);
#elif defined(VERSION)
		printf("aicli %s\n", VERSION);
#else
		printf("aicli\n");
#endif
		return 0;
	}

	if (strcmp(argv[1], "--list-tools") == 0) {
		return cmd_list_tools();
	}

	// Allow global flags (e.g. --config/--no-config) before subcommands.
	int argi = 1;
	while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
		if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0) {
			usage(stdout);
			return 0;
		}
		if (strcmp(argv[argi], "--version") == 0 || strcmp(argv[argi], "-V") == 0) {
#ifdef PACKAGE_STRING
			printf("%s\n", PACKAGE_STRING);
#elif defined(VERSION)
			printf("aicli %s\n", VERSION);
#else
			printf("aicli\n");
#endif
			return 0;
		}
		if (strcmp(argv[argi], "--no-config") == 0) {
			argi++;
			continue;
		}
		if (strcmp(argv[argi], "--config") == 0 && argi + 1 < argc) {
			argi += 2;
			continue;
		}
		// Stop at unknown flag; other subcommands may parse it.
		break;
	}

	if (argi >= argc) {
		fprintf(stderr, "missing subcommand\n");
		usage(stderr);
		return 2;
	}

	if (argi < argc && strcmp(argv[argi], "_exec") == 0) {
		// Pass argv starting at program name so cmd_exec_local sees the expected layout:
		// aicli _exec --file PATH "cat PATH"
		return cmd_exec_local(argc - (argi - 1), argv + (argi - 1));
	}

	aicli_config_t cfg;
	if (!load_config_with_precedence(&cfg, argc, argv)) {
		fprintf(stderr, "failed to load config\n");
		return 2;
	}

	int rc = 0;

	if (strcmp(argv[argi], "web") == 0) {
		if (argc >= 3 && strcmp(argv[2], "search") == 0) {
			rc = cmd_web_search(argc, argv, &cfg);
			aicli_config_free(&cfg);
			return rc;
		}
		if (argc >= 3 && strcmp(argv[2], "fetch") == 0) {
			rc = cmd_web_fetch(argc, argv, &cfg);
			aicli_config_free(&cfg);
			return rc;
		}
		fprintf(stderr, "unknown web subcommand\n");
		aicli_config_free(&cfg);
		return 2;
	}

	if (strcmp(argv[argi], "chat") == 0) {
		rc = cmd_chat(argc, argv, &cfg);
		aicli_config_free(&cfg);
		return rc;
	}

	if (strcmp(argv[argi], "run") == 0) {
		rc = cmd_run(argc, argv, &cfg);
		aicli_config_free(&cfg);
		return rc;
	}

	fprintf(stderr, "unknown subcommand: %s\n", argv[argi]);
	usage(stderr);
	aicli_config_free(&cfg);
	return 2;
}

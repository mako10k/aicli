
#include "cli.h"
#include "continue_state.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdarg.h>
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
	// Internal helper for execute testing:
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
	// For execute: keep errors on stderr, but also allow tools to return
	// error text via stdout (e.g., grep: <regex error>) while failing.
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
	fputs(aicli_cli_usage_string(), out);
}

const char *aicli_cli_usage_string(void)
{
	return "aicli - lightweight native OpenAI client\n\n"
	       "Usage:\n"
	       "  aicli _exec [--file PATH ...] [--file - | --stdin] [--start N] [--size N] <cmd>\n"
	       "  aicli chat <prompt>\n"
	       "  aicli web search <query> [--count N] [--lang xx] [--freshness day|week|month] [--max-title N] [--max-url N] [--max-snippet N] [--width N] [--raw]\n"
	       "                    (note: --start/--size are available only with --raw)\n"
	       "  aicli web fetch <url> [--start N] [--size N]\n"
	       "  aicli run [--file PATH ...] [--file - | --stdin] [--turns N] [--max-tool-calls N] [--tool-threads N]\n"
	       "           [--continue[=auto|both|after|next][=THREAD]]\n"
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
	       "Continue:\n"
	       "  --continue saves the last OpenAI response id (response.id) to a state file keyed by getsid(0)\n"
	       "  and uses it as previous_response_id on the next run (session continuity).\n"
	       "\n"
	       "Environment:\n"
	       "  OPENAI_API_KEY=... (or AICLI_OPENAI_API_KEY)\n"
	       "  AICLI_SEARCH_PROVIDER=google_cse|google|brave (default: google_cse)\n"
	       "  AICLI_WEB_FETCH_PREFIXES=prefix1,prefix2,... (enables web fetch allowlist)\n"
	       "  GOOGLE_API_KEY=...\n"
	       "  GOOGLE_CSE_CX=...\n"
	       "  BRAVE_API_KEY=... (when provider=brave)\n";
}

static void config_apply_env_overrides(aicli_config_t *cfg)
{
	if (!cfg)
		return;
	const char *v;

	v = getenv("OPENAI_API_KEY");
	if ((!v || !v[0]))
		v = getenv("AICLI_OPENAI_API_KEY");
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
		if (!aicli_config_file_is_secure(&cf)) {
			fprintf(stderr,
			        "insecure config file permissions: %s\n"
			        "Fix with: chmod 600 %s\n",
			        cf.path ? cf.path : "(null)", cf.path ? cf.path : "(null)");
			aicli_config_file_free(&cf);
			return false;
		}
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
		char *endp = NULL;
		errno = 0;
		long v = strtol(eq + 1, &endp, 10);
		if (errno != 0 || endp == (eq + 1) || !endp || *endp != '\0' || v < 0 || v > 10)
			return 1;
		*out_level = (int)v;
		return 0;
	}

	if (has_next && next && next[0] && next[0] != '-') {
		char *endp = NULL;
		errno = 0;
		long v = strtol(next, &endp, 10);
		if (errno != 0 || endp == next || !endp || *endp != '\0' || v < 0 || v > 10)
			return 1;
		*out_level = (int)v;
		*out_consumed_next = 1;
		return 0;
	}

	return 0;
}

typedef struct {
	bool loaded;
	bool has_saved_unix;
	long long saved_unix;
	bool auto_search;
	bool disable_all_tools;
	size_t turns;
	size_t max_tool_calls;
	size_t tool_threads;
	int debug_api;
	int debug_function_call;
	char search_provider[32];
	bool has_google_api_key;
	bool has_google_cse_cx;
	bool has_brave_api_key;
	bool has_web_fetch_prefixes;
	char force_tool[64];
	char available_tools[128];
	char continue_mode[16];
	bool continue_has_thread;
	char continue_thread[64];
} continue_meta_t;

static const char *continue_mode_to_string_local(aicli_continue_mode_t m)
{
	switch (m) {
	case AICLI_CONTINUE_AUTO:
		return "auto";
	case AICLI_CONTINUE_BOTH:
		return "both";
	case AICLI_CONTINUE_AFTER:
		return "after";
	case AICLI_CONTINUE_NEXT:
		return "next";
	}
	return "auto";
}

static const char *search_provider_to_string_local(aicli_search_provider_t p)
{
	if (p == AICLI_SEARCH_PROVIDER_BRAVE)
		return "brave";
	return "google_cse";
}

static void trim_line_eol(char *s)
{
	if (!s)
		return;
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
		s[n - 1] = '\0';
		n--;
	}
}

static void copy_cstr(char *dst, size_t cap, const char *src)
{
	if (!dst || cap == 0)
		return;
	if (!src)
		src = "";
	strncpy(dst, src, cap - 1);
	dst[cap - 1] = '\0';
}

static int continue_meta_path_from_state(const char *state_path, char *out, size_t out_cap)
{
	if (!state_path || !state_path[0] || !out || out_cap == 0)
		return -1;
	int n = snprintf(out, out_cap, "%s.meta", state_path);
	if (n <= 0 || (size_t)n >= out_cap)
		return -1;
	return 0;
}

static void continue_meta_from_current(continue_meta_t *m,
					      const aicli_config_t *cfg,
					      bool auto_search,
					      int disable_all_tools,
					      const char *available_tools,
					      const char *force_tool,
					      int debug_api,
					      int debug_function_call,
					      size_t turns,
					      size_t max_tool_calls,
					      size_t tool_threads,
					      const aicli_continue_opt_t *cont)
{
	if (!m)
		return;
	memset(m, 0, sizeof(*m));
	m->loaded = true;
	m->auto_search = auto_search;
	m->disable_all_tools = (disable_all_tools != 0);
	m->turns = turns;
	m->max_tool_calls = max_tool_calls;
	m->tool_threads = tool_threads;
	m->debug_api = debug_api;
	m->debug_function_call = debug_function_call;
	copy_cstr(m->search_provider, sizeof(m->search_provider),
		  search_provider_to_string_local(cfg ? cfg->search_provider : AICLI_SEARCH_PROVIDER_GOOGLE_CSE));
	m->has_google_api_key = (cfg && cfg->google_api_key && cfg->google_api_key[0]);
	m->has_google_cse_cx = (cfg && cfg->google_cse_cx && cfg->google_cse_cx[0]);
	m->has_brave_api_key = (cfg && cfg->brave_api_key && cfg->brave_api_key[0]);
	{
		const char *v = getenv("AICLI_WEB_FETCH_PREFIXES");
		m->has_web_fetch_prefixes = (v && v[0]);
	}
	copy_cstr(m->force_tool, sizeof(m->force_tool), force_tool ? force_tool : "");
	copy_cstr(m->available_tools, sizeof(m->available_tools), available_tools ? available_tools : "");
	if (cont) {
		copy_cstr(m->continue_mode, sizeof(m->continue_mode), continue_mode_to_string_local(cont->mode));
		m->continue_has_thread = cont->has_thread;
		copy_cstr(m->continue_thread, sizeof(m->continue_thread), cont->has_thread ? cont->thread_name : "");
	} else {
		copy_cstr(m->continue_mode, sizeof(m->continue_mode), "auto");
	}
}

static int continue_meta_write(const char *state_path, const continue_meta_t *m)
{
	char meta_path[4120];
	if (continue_meta_path_from_state(state_path, meta_path, sizeof(meta_path)) != 0)
		return -1;
	if (!m)
		return -1;

	FILE *fp = fopen(meta_path, "w");
	if (!fp)
		return -1;

	fprintf(fp, "version=1\n");
	fprintf(fp, "saved_unix=%lld\n", m->has_saved_unix ? m->saved_unix : 0LL);
	fprintf(fp, "auto_search=%d\n", m->auto_search ? 1 : 0);
	fprintf(fp, "disable_all_tools=%d\n", m->disable_all_tools ? 1 : 0);
	fprintf(fp, "turns=%zu\n", m->turns);
	fprintf(fp, "max_tool_calls=%zu\n", m->max_tool_calls);
	fprintf(fp, "tool_threads=%zu\n", m->tool_threads);
	fprintf(fp, "debug_api=%d\n", m->debug_api);
	fprintf(fp, "debug_function_call=%d\n", m->debug_function_call);
	fprintf(fp, "search_provider=%s\n", m->search_provider[0] ? m->search_provider : "google_cse");
	fprintf(fp, "has_google_api_key=%d\n", m->has_google_api_key ? 1 : 0);
	fprintf(fp, "has_google_cse_cx=%d\n", m->has_google_cse_cx ? 1 : 0);
	fprintf(fp, "has_brave_api_key=%d\n", m->has_brave_api_key ? 1 : 0);
	fprintf(fp, "has_web_fetch_prefixes=%d\n", m->has_web_fetch_prefixes ? 1 : 0);
	fprintf(fp, "force_tool=%s\n", m->force_tool);
	fprintf(fp, "available_tools=%s\n", m->available_tools);
	fprintf(fp, "continue_mode=%s\n", m->continue_mode[0] ? m->continue_mode : "auto");
	fprintf(fp, "continue_has_thread=%d\n", m->continue_has_thread ? 1 : 0);
	fprintf(fp, "continue_thread=%s\n", m->continue_thread);

	if (fclose(fp) != 0)
		return -1;
	return 0;
}

static int continue_meta_read(const char *state_path, continue_meta_t *out)
{
	char meta_path[4120];
	if (continue_meta_path_from_state(state_path, meta_path, sizeof(meta_path)) != 0)
		return -1;
	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));

	FILE *fp = fopen(meta_path, "r");
	if (!fp)
		return -1;

	char line[512];
	while (fgets(line, sizeof(line), fp)) {
		trim_line_eol(line);
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		const char *k = line;
		const char *v = eq + 1;

		if (strcmp(k, "saved_unix") == 0) {
			char *endp = NULL;
			errno = 0;
			long long t = strtoll(v, &endp, 10);
			if (errno == 0 && endp && *endp == '\0' && t > 0) {
				out->saved_unix = t;
				out->has_saved_unix = true;
			}
			continue;
		}
		if (strcmp(k, "auto_search") == 0) {
			out->auto_search = (atoi(v) != 0);
			continue;
		}
		if (strcmp(k, "disable_all_tools") == 0) {
			out->disable_all_tools = (atoi(v) != 0);
			continue;
		}
		if (strcmp(k, "turns") == 0) {
			out->turns = (size_t)strtoull(v, NULL, 10);
			continue;
		}
		if (strcmp(k, "max_tool_calls") == 0) {
			out->max_tool_calls = (size_t)strtoull(v, NULL, 10);
			continue;
		}
		if (strcmp(k, "tool_threads") == 0) {
			out->tool_threads = (size_t)strtoull(v, NULL, 10);
			continue;
		}
		if (strcmp(k, "debug_api") == 0) {
			out->debug_api = atoi(v);
			continue;
		}
		if (strcmp(k, "debug_function_call") == 0) {
			out->debug_function_call = atoi(v);
			continue;
		}
		if (strcmp(k, "search_provider") == 0) {
			copy_cstr(out->search_provider, sizeof(out->search_provider), v);
			continue;
		}
		if (strcmp(k, "has_google_api_key") == 0) {
			out->has_google_api_key = (atoi(v) != 0);
			continue;
		}
		if (strcmp(k, "has_google_cse_cx") == 0) {
			out->has_google_cse_cx = (atoi(v) != 0);
			continue;
		}
		if (strcmp(k, "has_brave_api_key") == 0) {
			out->has_brave_api_key = (atoi(v) != 0);
			continue;
		}
		if (strcmp(k, "has_web_fetch_prefixes") == 0) {
			out->has_web_fetch_prefixes = (atoi(v) != 0);
			continue;
		}
		if (strcmp(k, "force_tool") == 0) {
			copy_cstr(out->force_tool, sizeof(out->force_tool), v);
			continue;
		}
		if (strcmp(k, "available_tools") == 0) {
			copy_cstr(out->available_tools, sizeof(out->available_tools), v);
			continue;
		}
		if (strcmp(k, "continue_mode") == 0) {
			copy_cstr(out->continue_mode, sizeof(out->continue_mode), v);
			continue;
		}
		if (strcmp(k, "continue_has_thread") == 0) {
			out->continue_has_thread = (atoi(v) != 0);
			continue;
		}
		if (strcmp(k, "continue_thread") == 0) {
			copy_cstr(out->continue_thread, sizeof(out->continue_thread), v);
			continue;
		}
	}

	(void)fclose(fp);
	out->loaded = true;
	if (!out->search_provider[0])
		copy_cstr(out->search_provider, sizeof(out->search_provider), "google_cse");
	if (!out->continue_mode[0])
		copy_cstr(out->continue_mode, sizeof(out->continue_mode), "auto");
	return 0;
}

static bool appendf(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
	if (!buf || !len || !fmt || *len >= cap)
		return false;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
	va_end(ap);
	if (n <= 0)
		return false;
	if ((size_t)n >= (cap - *len)) {
		*len = cap;
		return false;
	}
	*len += (size_t)n;
	return true;
}

static void format_time_utc(long long t, char out[32])
{
	if (!out)
		return;
	if (t <= 0) {
		copy_cstr(out, 32, "unknown");
		return;
	}
	time_t tt = (time_t)t;
	struct tm tmv;
	if (!gmtime_r(&tt, &tmv)) {
		copy_cstr(out, 32, "unknown");
		return;
	}
	if (strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tmv) == 0)
		copy_cstr(out, 32, "unknown");
}

static bool streq0(const char *a, const char *b)
{
	const char *x = a ? a : "";
	const char *y = b ? b : "";
	return strcmp(x, y) == 0;
}

static char *build_continue_delta_context(const continue_meta_t *prev, const continue_meta_t *cur)
{
	if (!cur || !cur->loaded)
		return NULL;

	char *buf = (char *)malloc(4096);
	if (!buf)
		return NULL;
	size_t len = 0;
	buf[0] = '\0';

	long long now_unix = (long long)time(NULL);
	char now_iso[32];
	format_time_utc(now_unix, now_iso);
	(void)appendf(buf, 4096, &len,
		      "CONTINUE_DELTA_CONTEXT:\n"
		      "- current_time_utc: %s\n",
		      now_iso);

	if (!prev || !prev->loaded) {
		(void)appendf(buf, 4096, &len, "- previous_snapshot: unavailable\n");
	} else if (prev->has_saved_unix) {
		char prev_iso[32];
		format_time_utc(prev->saved_unix, prev_iso);
		long long delta = now_unix - prev->saved_unix;
		if (delta < 0)
			delta = 0;
		(void)appendf(buf, 4096, &len,
			      "- previous_time_utc: %s\n"
			      "- elapsed_seconds_since_previous: %lld\n",
			      prev_iso, delta);
	}

	int changes = 0;
	if (prev && prev->loaded) {
		if (prev->auto_search != cur->auto_search) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.auto_search: %s -> %s\n",
				      prev->auto_search ? "on" : "off", cur->auto_search ? "on" : "off");
		}
		if (prev->disable_all_tools != cur->disable_all_tools) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.disable_all_tools: %s -> %s\n",
				      prev->disable_all_tools ? "on" : "off",
				      cur->disable_all_tools ? "on" : "off");
		}
		if (!streq0(prev->force_tool, cur->force_tool)) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.force_tool: %s -> %s\n",
				      prev->force_tool[0] ? prev->force_tool : "(none)",
				      cur->force_tool[0] ? cur->force_tool : "(none)");
		}
		if (!streq0(prev->available_tools, cur->available_tools)) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.available_tools: %s -> %s\n",
				      prev->available_tools[0] ? prev->available_tools : "(default)",
				      cur->available_tools[0] ? cur->available_tools : "(default)");
		}
		if (!streq0(prev->search_provider, cur->search_provider)) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.search_provider: %s -> %s\n",
				      prev->search_provider[0] ? prev->search_provider : "google_cse",
				      cur->search_provider[0] ? cur->search_provider : "google_cse");
		}
		if (prev->has_google_api_key != cur->has_google_api_key) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.has_google_api_key: %s -> %s\n",
				      prev->has_google_api_key ? "yes" : "no",
				      cur->has_google_api_key ? "yes" : "no");
		}
		if (prev->has_google_cse_cx != cur->has_google_cse_cx) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.has_google_cse_cx: %s -> %s\n",
				      prev->has_google_cse_cx ? "yes" : "no",
				      cur->has_google_cse_cx ? "yes" : "no");
		}
		if (prev->has_brave_api_key != cur->has_brave_api_key) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.has_brave_api_key: %s -> %s\n",
				      prev->has_brave_api_key ? "yes" : "no",
				      cur->has_brave_api_key ? "yes" : "no");
		}
		if (prev->has_web_fetch_prefixes != cur->has_web_fetch_prefixes) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.has_web_fetch_prefixes: %s -> %s\n",
				      prev->has_web_fetch_prefixes ? "yes" : "no",
				      cur->has_web_fetch_prefixes ? "yes" : "no");
		}
		if (!streq0(prev->continue_mode, cur->continue_mode)) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.continue_mode: %s -> %s\n",
				      prev->continue_mode[0] ? prev->continue_mode : "auto",
				      cur->continue_mode[0] ? cur->continue_mode : "auto");
		}
		if (prev->continue_has_thread != cur->continue_has_thread ||
		    !streq0(prev->continue_thread, cur->continue_thread)) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.continue_thread: %s -> %s\n",
				      (prev->continue_has_thread && prev->continue_thread[0]) ? prev->continue_thread
				                                                        : "(none)",
				      (cur->continue_has_thread && cur->continue_thread[0]) ? cur->continue_thread
				                                                        : "(none)");
		}
		if (prev->turns != cur->turns) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.turns: %zu -> %zu\n", prev->turns, cur->turns);
		}
		if (prev->max_tool_calls != cur->max_tool_calls) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.max_tool_calls: %zu -> %zu\n",
				      prev->max_tool_calls, cur->max_tool_calls);
		}
		if (prev->tool_threads != cur->tool_threads) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.tool_threads: %zu -> %zu\n",
				      prev->tool_threads, cur->tool_threads);
		}
		if (prev->debug_api != cur->debug_api) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.debug_api: %d -> %d\n",
				      prev->debug_api, cur->debug_api);
		}
		if (prev->debug_function_call != cur->debug_function_call) {
			changes++;
			(void)appendf(buf, 4096, &len, "- changed.debug_function_call: %d -> %d\n",
				      prev->debug_function_call, cur->debug_function_call);
		}
	}

	if (changes == 0)
		(void)appendf(buf, 4096, &len, "- changed.settings: none_detected\n");

	(void)appendf(buf, 4096, &len,
		      "- instruction: prioritize current run settings when prior conversation assumptions conflict.\n");
	return buf;
}

static int cmd_list_tools(void)
{
	printf("Available tools:\n");
	printf("  execute\n");
#ifdef HAVE_MD4C
	printf("  set_rendering_mode\n");
#endif
	printf("\n");
	printf("Notes:\n");
	printf("  - execute is read-only and limited to allowlisted files.\n");
	printf("  - Use aicli run --file PATH to allow a file for execute.\n");
#ifdef HAVE_MD4C
	printf("  - set_rendering_mode accepts mode=plain|markdown (default: markdown).\n");
#endif
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
	// Equivalent to: aicli run --turns 1 --max-tool-calls 1 --tool-threads 1 --disable-all-tools <prompt>
	char *args[13];
	int n = 0;
	args[n++] = argv[0];
	args[n++] = "run";
	args[n++] = "--turns";
	args[n++] = "1";
	args[n++] = "--max-tool-calls";
	args[n++] = "1";
	args[n++] = "--tool-threads";
	args[n++] = "1";
	args[n++] = "--disable-all-tools";
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
		fprintf(stderr, "OPENAI_API_KEY (or AICLI_OPENAI_API_KEY, or config openai_api_key) is required\n");
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
	aicli_continue_opt_t cont = {0};
	bool want_continue = false;
	char prev_id[256];
	prev_id[0] = '\0';
	char state_path[4096];
	state_path[0] = '\0';
	continue_meta_t prev_meta;
	continue_meta_t curr_meta;
	memset(&prev_meta, 0, sizeof(prev_meta));
	memset(&curr_meta, 0, sizeof(curr_meta));

	int i = 2;
	while (i < argc && strncmp(argv[i], "--", 2) == 0) {
		if (strcmp(argv[i], "--continue") == 0 || strncmp(argv[i], "--continue=", 11) == 0) {
			const char *optarg = NULL;
			if (argv[i][10] == '=')
				optarg = argv[i] + 11;
			else if (i + 1 < argc && argv[i + 1][0] != '-')
				optarg = argv[i + 1];
			if (aicli_continue_parse(optarg, &cont) != 0) {
				fprintf(stderr,
				        "invalid --continue (expected: [auto|both|after|next][=THREAD] or THREAD)\n");
				return 2;
			}
			want_continue = true;
			if (!(argv[i][10] == '=') && optarg == argv[i + 1])
				i += 2;
			else
				i += 1;
			continue;
		}
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

	const char *previous_response_id = NULL;
	if (want_continue) {
		long sid = (long)getsid(0);
		if (sid <= 0) {
			fprintf(stderr, "failed to get session id for --continue\n");
			return 2;
		}
		if (aicli_continue_state_path(state_path, sizeof(state_path), sid, &cont) != 0) {
			fprintf(stderr, "failed to compute --continue state path\n");
			return 2;
		}
		int rrc = aicli_continue_read_id(state_path, prev_id, sizeof(prev_id));
		if (rrc == 0) {
			previous_response_id = prev_id;
			(void)continue_meta_read(state_path, &prev_meta);
		} else if (rrc == 1) {
			// Missing state is OK: we will just start a new conversation.
			previous_response_id = NULL;
		} else {
			fprintf(stderr, "failed to read --continue state file: %s\n", state_path);
			return 2;
		}
	}

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
	char *continue_delta = NULL;
	char *final_prompt = NULL;
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

	if (want_continue) {
		continue_meta_from_current(&curr_meta, cfg, auto_search, disable_all_tools,
					     available_tools, force_tool,
					     debug_api, debug_function_call,
					     turns, max_tool_calls, tool_threads,
					     &cont);
	}

	if (want_continue && previous_response_id && previous_response_id[0]) {
		continue_delta = build_continue_delta_context(&prev_meta, &curr_meta);
		if (continue_delta && continue_delta[0]) {
			const char *base = augmented_prompt ? augmented_prompt : prompt;
			size_t need = strlen(continue_delta) + strlen(base) + 2;
			final_prompt = (char *)malloc(need);
			if (final_prompt)
				snprintf(final_prompt, need, "%s\n%s", continue_delta, base);
		}
	}
	free(continue_delta);

	aicli_allowed_file_t allow_files[32];
	for (int ai = 0; ai < file_count; ai++) {
		allow_files[ai].path = allow_paths[ai];
		allow_files[ai].name = files[ai].name;
		allow_files[ai].size_bytes = files[ai].size_bytes;
	}
	aicli_allowlist_t allow = {.files = allow_files, .file_count = file_count};
	char *final_text = NULL;
	char *final_response_json = NULL;
	const char *to_send = final_prompt ? final_prompt : (augmented_prompt ? augmented_prompt : prompt);
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
	int rc = aicli_openai_run_with_tools(&cfg_local, &allow, to_send, previous_response_id, turns,
	                                   (size_t)max_tool_calls, tool_threads,
	                                   tool_choice, &final_text, &final_response_json);
	if (want_continue) {
			bool should_write = false;
			if (cont.mode == AICLI_CONTINUE_BOTH)
				should_write = true;
			else if (cont.mode == AICLI_CONTINUE_AFTER)
				should_write = (rc == 0);
			else if (cont.mode == AICLI_CONTINUE_NEXT)
				should_write = (rc == 0);
			else /* auto */
				should_write = (rc == 0);

		if (should_write) {
			// Best-effort: if we have previous already, keep it if we can't extract new.
			char rid[256];
			rid[0] = '\0';
			const char *to_write = NULL;
			if (final_response_json && final_response_json[0] &&
			    aicli_openai_extract_response_id(final_response_json, strlen(final_response_json), rid,
			                                 sizeof(rid)) == 0) {
				to_write = rid;
			} else if (previous_response_id && previous_response_id[0]) {
				// Fallback: keep continuity from what we used.
				to_write = previous_response_id;
			}
			if (to_write && state_path[0]) {
				if (aicli_continue_write_id(state_path, to_write) == 0) {
					if (!curr_meta.loaded) {
						continue_meta_from_current(&curr_meta, cfg, auto_search, disable_all_tools,
								     available_tools, force_tool,
								     debug_api, debug_function_call,
								     turns, max_tool_calls, tool_threads,
								     &cont);
					}
					curr_meta.has_saved_unix = true;
					curr_meta.saved_unix = (long long)time(NULL);
					(void)continue_meta_write(state_path, &curr_meta);
				}
			}
		}
	}
	free(final_prompt);
	free(augmented_prompt);
	// Free allowlisted paths after the tool loop finishes.
	for (int fi = 0; fi < file_count; fi++)
		free(allow_paths[fi]);
	if (stdin_tmp_path[0])
		unlink(stdin_tmp_path);

	if (rc != 0) {
		fprintf(stderr, "openai request failed\n");
		free(final_text);
		free(final_response_json);
		return 2;
	}

	if (final_text && final_text[0]) {
		fputs(final_text, stdout);
		fputc('\n', stdout);
		free(final_text);
		free(final_response_json);
		return 0;
	}

	free(final_text);
	free(final_response_json);
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

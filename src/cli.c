#include "cli.h"

#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aicli.h"
#include "aicli_config.h"
#include "auto_search.h"
#include "brave_search.h"
#include "google_search.h"
#include "execute_tool.h"
#include "openai_tool_loop.h"

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
	        "  aicli run [--file PATH ...] [--turns N] [--max-tool-calls N] [--tool-threads N] [--auto-search] <prompt>\n"
	        "\n"
	        "Environment:\n"
	        "  AICLI_SEARCH_PROVIDER=google_cse|google|brave (default: google_cse)\n"
	        "  GOOGLE_API_KEY=...\n"
	        "  GOOGLE_CSE_CX=...\n"
	        "  BRAVE_API_KEY=... (when provider=brave)\n");
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
	// aicli run [--file PATH ...] [--turns N] [--max-tool-calls N] [--tool-threads N] [--auto-search] <prompt>
	if (!cfg || !cfg->openai_api_key || !cfg->openai_api_key[0]) {
		fprintf(stderr, "OPENAI_API_KEY is required\n");
		return 2;
	}

	aicli_allowed_file_t files[32];
	int file_count = 0;
	bool auto_search = false;
	size_t turns = 4;
	size_t max_tool_calls = 8;
	size_t tool_threads = 1;

	int i = 2;
	while (i < argc && strncmp(argv[i], "--", 2) == 0) {
		if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
			if (file_count >= (int)(sizeof(files) / sizeof(files[0]))) {
				fprintf(stderr, "too many --file entries (max %zu)\n",
				        sizeof(files) / sizeof(files[0]));
				return 2;
			}
			char *rp = aicli_realpath_dup(argv[i + 1]);
			if (!rp) {
				fprintf(stderr, "invalid file: %s\n", argv[i + 1]);
				return 2;
			}
			files[file_count].path = rp;
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
					{
#if HAVE_YYJSON_H
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
					}
					yyjson_doc_free(doc);
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

	aicli_allowlist_t allow = {.files = files, .file_count = file_count};
	char *final_text = NULL;
	const char *to_send = augmented_prompt ? augmented_prompt : prompt;
	int rc = aicli_openai_run_with_tools(cfg, &allow, to_send, turns, max_tool_calls,
	                                   tool_threads, &final_text);
	free(augmented_prompt);
	for (int fi = 0; fi < file_count; fi++)
		free((void *)files[fi].path);

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
	return 0;
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

		aicli_google_response_t res;
		int rc = aicli_google_cse_search(cfg->google_api_key, cfg->google_cse_cx,
		                               query, count,
		                               lr, &res);
		if (rc != 0) {
			fprintf(stderr, "google cse search failed: %s\n", res.error[0] ? res.error : "unknown");
			aicli_google_response_free(&res);
			return 2;
		}

		if (res.http_status != 200) {
			fprintf(stderr, "google http_status=%d\n", res.http_status);
			if (res.body && res.body_len)
				fwrite(res.body, 1, res.body_len, stdout);
			fputc('\n', stdout);
			aicli_google_response_free(&res);
			return 1;
		}

		if (raw_json) {
			if (res.body && res.body_len)
				fwrite(res.body, 1, res.body_len, stdout);
			fputc('\n', stdout);
			aicli_google_response_free(&res);
			return 0;
		}

		// Default: print a formatted view (best-effort, no JSON library needed).
		if (google_cse_print_formatted_from_json(res.body, res.body_len, query, count,
						max_title, max_url, max_snippet, width) == 0) {
			aicli_google_response_free(&res);
			return 0;
		}

		// Fallback: print the first ~4KB.
		size_t n = res.body_len;
		if (n > 4096)
			n = 4096;
		fwrite(res.body, 1, n, stdout);
		fprintf(stdout, "\n... (truncated, %zu bytes total; add --raw for full JSON)\n",
		        res.body_len);
		aicli_google_response_free(&res);
		return 0;
	}

	if (is_brave) {
		if (!cfg->brave_api_key || !cfg->brave_api_key[0]) {
			fprintf(stderr, "BRAVE_API_KEY is required (provider=brave)\n");
			return 2;
		}
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
	yyjson_doc *doc = yyjson_read(res.body, res.body_len, 0);
	if (!doc) {
		fprintf(stderr, "yyjson parse error\n");
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

	fprintf(stderr, "unknown search provider\n");
	return 2;
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

	if (strcmp(argv[1], "chat") == 0) {
		return cmd_chat(argc, argv, &cfg);
	}

	if (strcmp(argv[1], "run") == 0) {
		return cmd_run(argc, argv, &cfg);
	}

	// Stub scaffold: other subcommands will be wired in MVP implementation.
	fprintf(stderr, "scaffold: subcommands not implemented yet (%s)\n", argv[1]);
	fprintf(stderr, "See docs/design.md for the target spec.\n");

	return 0;
}

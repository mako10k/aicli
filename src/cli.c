#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aicli.h"
#include "aicli_config.h"
#include "execute_tool.h"

static int cmd_exec_local(int argc, char **argv) {
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
	if (res.stderr_text && res.stderr_text[0]) fprintf(stderr, "%s\n", res.stderr_text);
	if (res.stdout_text) fwrite(res.stdout_text, 1, res.stdout_len, stdout);
	if (res.stdout_text) free((void *)res.stdout_text);
	for (int fi = 0; fi < file_count; fi++) {
		free((void *)files[fi].path);
	}

	if (res.has_next_start) {
		fprintf(stderr, "\n[total_bytes=%zu next_start=%zu]\n", res.total_bytes, res.next_start);
	} else {
		fprintf(stderr, "\n[total_bytes=%zu]\n", res.total_bytes);
	}
	return res.exit_code;
}

static void usage(FILE *out) {
	fprintf(out,
		"aicli - lightweight native OpenAI client\n\n"
		"Usage:\n"
		"  aicli chat <prompt>\n"
		"  aicli web search <query> [--count N] [--lang xx] [--freshness day|week|month]\n"
		"  aicli run [--auto-search] [--file PATH ...] <prompt>\n"
	);
}

int aicli_cli_main(int argc, char **argv) {
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

	// Stub scaffold: actual subcommands will be wired in MVP implementation.
	fprintf(stderr, "scaffold: subcommands not implemented yet (%s)\n", argv[1]);
	fprintf(stderr, "See docs/design.md for the target spec.\n");

	aicli_config_t cfg;
	if (!aicli_config_load_from_env(&cfg)) {
		// It's OK for scaffold; real implementation will require OPENAI_API_KEY for chat/run.
	}

	return 0;
}

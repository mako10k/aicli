#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

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

	// Stub scaffold: actual subcommands will be wired in MVP implementation.
	fprintf(stderr, "scaffold: subcommands not implemented yet (%s)\n", argv[1]);
	fprintf(stderr, "See docs/design.md for the target spec.\n");

	aicli_config_t cfg;
	if (!aicli_config_load_from_env(&cfg)) {
		// It's OK for scaffold; real implementation will require OPENAI_API_KEY for chat/run.
	}

	return 0;
}

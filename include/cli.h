#pragma once

int aicli_cli_main(int argc, char **argv);

// Read-only helper for tool loop: returns built-in CLI help/usage text.
// The returned pointer is to static storage and must not be freed.
const char *aicli_cli_usage_string(void);

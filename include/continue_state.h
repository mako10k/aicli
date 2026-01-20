#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	AICLI_CONTINUE_AUTO = 0,
	AICLI_CONTINUE_BOTH,
	AICLI_CONTINUE_AFTER,
	AICLI_CONTINUE_NEXT,
} aicli_continue_mode_t;

typedef struct {
	aicli_continue_mode_t mode;
	char thread_name[64];
	bool has_thread;
} aicli_continue_opt_t;

// Parse --continue[=SUBOPTS]
// SUBOPTS forms:
//   (empty) -> auto
//   auto|both|after|next
//   auto=THREAD|both=THREAD|after=THREAD|next=THREAD
int aicli_continue_parse(const char *optarg, aicli_continue_opt_t *out);

// Computes state file path.
// Directory is chosen by: $XDG_RUNTIME_DIR/aicli, else $TMPDIR/aicli, else /tmp/aicli.
// File name base: .previous_response_id_${pid}[ _${thread}]
int aicli_continue_state_path(char *out_path, size_t out_cap,
                             long pid,
                             const aicli_continue_opt_t *opt);

// Read previous_response_id from state file.
// Returns 0 on success (found and read). Returns 1 if file missing.
int aicli_continue_read_id(const char *path, char *out_id, size_t out_cap);

// Atomic write last response id to state file (0600).
int aicli_continue_write_id(const char *path, const char *response_id);

#ifdef __cplusplus
}
#endif

#pragma once

#include "aicli.h"
#include "execute_tool.h"

// Builds JSON describing the current allowlist with paging and filtering.
//
// Output `out->json` is heap-allocated and must be freed by caller.
// JSON shape:
// {
//   "ok": true,
//   "total": <int>,
//   "start": <int>,
//   "size": <int>,
//   "returned": <int>,
//   "has_next": <bool>,
//   "next_start": <int|null>,
//   "query": "...",
//   "files": [ {"path":"...","name":"...","size_bytes":123}, ...]
// }
int aicli_list_allowed_files_json(const aicli_allowlist_t *allow,
                                 const aicli_list_allowed_files_request_t *req,
                                 aicli_list_allowed_files_result_t *out);

void aicli_list_allowed_files_result_free(aicli_list_allowed_files_result_t *r);

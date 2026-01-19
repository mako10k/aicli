#pragma once

#include <stddef.h>

typedef struct {
	const char *model;       // required
	const char *input_text;  // required (single-turn for MVP)
	const char *system_text; // optional
} aicli_openai_request_t;

typedef struct {
	int http_status;
	char *body;
	size_t body_len;
	char error[256];
} aicli_openai_http_response_t;

// Low-level: POST /v1/responses.
// Returns 0 on successful HTTP request (even if status != 200).
// On transport/setup errors, returns non-zero and sets out->error.
int aicli_openai_responses_post(const char *api_key, const char *base_url,
			      const aicli_openai_request_t *req,
			      const char *tools_json, const char *tool_choice,
			      aicli_openai_http_response_t *out);

void aicli_openai_http_response_free(aicli_openai_http_response_t *res);

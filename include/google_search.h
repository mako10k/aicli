#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int http_status;
	char *body;
	size_t body_len;
	char error[256];
} aicli_google_response_t;

int aicli_google_cse_search(const char *api_key,
                           const char *cse_cx,
                           const char *query,
                           int num,
                           const char *lr,
                           aicli_google_response_t *out);

void aicli_google_response_free(aicli_google_response_t *r);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aicli_threadpool aicli_threadpool_t;

typedef void (*aicli_threadpool_job_fn)(void *arg);

// Creates a fixed-size thread pool. Returns NULL on failure.
// threads==0 is treated as 1.
aicli_threadpool_t *aicli_threadpool_create(size_t threads);

// Stops all workers and frees resources. Safe to call with NULL.
void aicli_threadpool_destroy(aicli_threadpool_t *p);

// Enqueue a job. Returns 0 on success.
int aicli_threadpool_submit(aicli_threadpool_t *p, aicli_threadpool_job_fn fn, void *arg);

// Wait until all queued + running jobs finish.
void aicli_threadpool_drain(aicli_threadpool_t *p);

#ifdef __cplusplus
}
#endif

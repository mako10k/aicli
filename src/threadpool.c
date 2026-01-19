#include "threadpool.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct job {
	aicli_threadpool_job_fn fn;
	void *arg;
	struct job *next;
} job_t;

struct aicli_threadpool {
	pthread_t *threads;
	size_t thread_count;

	pthread_mutex_t mu;
	pthread_cond_t cv_has_work;
	pthread_cond_t cv_drained;

	job_t *head;
	job_t *tail;
	size_t pending;
	size_t running;
	bool stop;
};

static void job_free_list(job_t *j)
{
	while (j) {
		job_t *n = j->next;
		free(j);
		j = n;
	}
}

static void *worker_main(void *arg)
{
	aicli_threadpool_t *p = (aicli_threadpool_t *)arg;

	for (;;) {
		pthread_mutex_lock(&p->mu);
		while (!p->stop && p->head == NULL) {
			pthread_cond_wait(&p->cv_has_work, &p->mu);
		}
		if (p->stop) {
			pthread_mutex_unlock(&p->mu);
			break;
		}
		job_t *j = p->head;
		p->head = j->next;
		if (!p->head)
			p->tail = NULL;
		p->pending--;
		p->running++;
		pthread_mutex_unlock(&p->mu);

		j->fn(j->arg);
		free(j);

		pthread_mutex_lock(&p->mu);
		p->running--;
		if (p->pending == 0 && p->running == 0) {
			pthread_cond_broadcast(&p->cv_drained);
		}
		pthread_mutex_unlock(&p->mu);
	}
	return NULL;
}

aicli_threadpool_t *aicli_threadpool_create(size_t threads)
{
	if (threads == 0)
		threads = 1;

	aicli_threadpool_t *p = (aicli_threadpool_t *)calloc(1, sizeof(*p));
	if (!p)
		return NULL;

	p->threads = (pthread_t *)calloc(threads, sizeof(pthread_t));
	if (!p->threads) {
		free(p);
		return NULL;
	}
	p->thread_count = threads;

	pthread_mutex_init(&p->mu, NULL);
	pthread_cond_init(&p->cv_has_work, NULL);
	pthread_cond_init(&p->cv_drained, NULL);

	for (size_t i = 0; i < threads; i++) {
		if (pthread_create(&p->threads[i], NULL, worker_main, p) != 0) {
			p->stop = true;
			for (size_t j = 0; j < i; j++)
				pthread_join(p->threads[j], NULL);
			pthread_cond_destroy(&p->cv_has_work);
			pthread_cond_destroy(&p->cv_drained);
			pthread_mutex_destroy(&p->mu);
			free(p->threads);
			free(p);
			return NULL;
		}
	}

	return p;
}

void aicli_threadpool_destroy(aicli_threadpool_t *p)
{
	if (!p)
		return;

	pthread_mutex_lock(&p->mu);
	p->stop = true;
	pthread_cond_broadcast(&p->cv_has_work);
	pthread_mutex_unlock(&p->mu);

	for (size_t i = 0; i < p->thread_count; i++)
		pthread_join(p->threads[i], NULL);

	pthread_mutex_lock(&p->mu);
	job_free_list(p->head);
	p->head = NULL;
	p->tail = NULL;
	p->pending = 0;
	p->running = 0;
	pthread_mutex_unlock(&p->mu);

	pthread_cond_destroy(&p->cv_has_work);
	pthread_cond_destroy(&p->cv_drained);
	pthread_mutex_destroy(&p->mu);

	free(p->threads);
	free(p);
}

int aicli_threadpool_submit(aicli_threadpool_t *p, aicli_threadpool_job_fn fn, void *arg)
{
	if (!p || !fn)
		return 2;

	job_t *j = (job_t *)calloc(1, sizeof(*j));
	if (!j)
		return 1;
	j->fn = fn;
	j->arg = arg;

	pthread_mutex_lock(&p->mu);
	if (p->stop) {
		pthread_mutex_unlock(&p->mu);
		free(j);
		return 3;
	}
	if (p->tail)
		p->tail->next = j;
	else
		p->head = j;
	p->tail = j;
	p->pending++;
	pthread_cond_signal(&p->cv_has_work);
	pthread_mutex_unlock(&p->mu);
	return 0;
}

void aicli_threadpool_drain(aicli_threadpool_t *p)
{
	if (!p)
		return;
	pthread_mutex_lock(&p->mu);
	while (p->pending != 0 || p->running != 0) {
		pthread_cond_wait(&p->cv_drained, &p->mu);
	}
	pthread_mutex_unlock(&p->mu);
}

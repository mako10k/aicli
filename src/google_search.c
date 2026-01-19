#include "google_search.h"

#include <curl/curl.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	char *buf;
	size_t len;
	size_t cap;
} mem_t;

static void set_err(char out[256], const char *fmt, ...)
{
	if (!out)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(out, 256, fmt, ap);
	va_end(ap);
	out[255] = '\0';
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	mem_t *m = (mem_t *)userdata;
	size_t n = size * nmemb;
	if (!m || n == 0)
		return n;
	if (m->len + n + 1 > m->cap) {
		size_t new_cap = m->cap ? m->cap : 4096;
		while (new_cap < m->len + n + 1)
			new_cap *= 2;
		char *nb = (char *)realloc(m->buf, new_cap);
		if (!nb)
			return 0;
		m->buf = nb;
		m->cap = new_cap;
	}
	memcpy(m->buf + m->len, ptr, n);
	m->len += n;
	m->buf[m->len] = '\0';
	return n;
}

void aicli_google_response_free(aicli_google_response_t *r)
{
	if (!r)
		return;
	free(r->body);
	r->body = NULL;
	r->body_len = 0;
	r->http_status = 0;
	r->error[0] = '\0';
}

int aicli_google_cse_search(const char *api_key,
                           const char *cse_cx,
                           const char *query,
                           int num,
                           const char *lr,
                           aicli_google_response_t *out)
{
	if (!out)
		return 1;
	out->http_status = 0;
	out->body = NULL;
	out->body_len = 0;
	out->error[0] = '\0';

	if (!api_key || !api_key[0]) {
		set_err(out->error, "GOOGLE_API_KEY is not set");
		return 2;
	}
	if (!cse_cx || !cse_cx[0]) {
		set_err(out->error, "GOOGLE_CSE_CX is not set");
		return 2;
	}
	if (!query || !query[0]) {
		set_err(out->error, "query is empty");
		return 2;
	}
	if (num <= 0)
		num = 5;
	if (num > 10)
		num = 10;

	CURL *curl = curl_easy_init();
	if (!curl) {
		set_err(out->error, "curl_easy_init failed");
		return 3;
	}
	mem_t mem = {0};

	char *q = curl_easy_escape(curl, query, 0);
	char *k = curl_easy_escape(curl, api_key, 0);
	char *cx = curl_easy_escape(curl, cse_cx, 0);
	char *lr_esc = NULL;
	if (lr && lr[0])
		lr_esc = curl_easy_escape(curl, lr, 0);

	if (!q || !k || !cx) {
		set_err(out->error, "curl_easy_escape failed");
		curl_free(q);
		curl_free(k);
		curl_free(cx);
		curl_free(lr_esc);
		curl_easy_cleanup(curl);
		return 3;
	}

	char url[4096];
	if (lr_esc) {
		snprintf(url, sizeof(url),
		         "https://www.googleapis.com/customsearch/v1?key=%s&cx=%s&q=%s&num=%d&lr=%s",
		         k, cx, q, num, lr_esc);
	} else {
		snprintf(url, sizeof(url),
		         "https://www.googleapis.com/customsearch/v1?key=%s&cx=%s&q=%s&num=%d",
		         k, cx, q, num);
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "aicli/1.0");

	CURLcode rc = curl_easy_perform(curl);
	if (rc != CURLE_OK) {
		set_err(out->error, "curl_easy_perform: %s", curl_easy_strerror(rc));
		curl_free(q);
		curl_free(k);
		curl_free(cx);
		curl_free(lr_esc);
		curl_easy_cleanup(curl);
		free(mem.buf);
		return 4;
	}

	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	out->http_status = (int)code;
	out->body = mem.buf;
	out->body_len = mem.len;

	curl_free(q);
	curl_free(k);
	curl_free(cx);
	curl_free(lr_esc);
	curl_easy_cleanup(curl);
	return 0;
}

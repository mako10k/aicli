#include "brave_search.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	char *data;
	size_t len;
	size_t cap;
} mem_buf_t;

static void mem_buf_free(mem_buf_t *b)
{
	if (!b)
		return;
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

static int mem_buf_reserve(mem_buf_t *b, size_t want)
{
	if (want <= b->cap)
		return 1;
	size_t new_cap = b->cap ? b->cap : 4096;
	while (new_cap < want) {
		new_cap *= 2;
		if (new_cap > (size_t)(16 * 1024 * 1024))
			return 0;
	}
	char *p = realloc(b->data, new_cap);
	if (!p)
		return 0;
	b->data = p;
	b->cap = new_cap;
	return 1;
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	mem_buf_t *b = (mem_buf_t *)userdata;
	size_t n = size * nmemb;
	if (n == 0)
		return 0;
	if (!mem_buf_reserve(b, b->len + n + 1))
		return 0;
	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	b->data[b->len] = '\0';
	return n;
}

static void set_err(char err[256], const char *msg)
{
	if (!err)
		return;
	if (!msg)
		msg = "unknown error";
	snprintf(err, 256, "%s", msg);
}

int aicli_brave_web_search(const char *api_key, const char *query, int count,
			   const char *lang, const char *freshness,
			   aicli_brave_response_t *out)
{
	if (!out)
		return 2;
	memset(out, 0, sizeof(*out));

	if (!api_key || !api_key[0]) {
		set_err(out->error, "BRAVE_API_KEY is not set");
		return 2;
	}
	if (!query || !query[0]) {
		set_err(out->error, "empty query");
		return 2;
	}
	if (count <= 0)
		count = 5;
	if (count > 20)
		count = 20;

	CURL *curl = curl_easy_init();
	if (!curl) {
		set_err(out->error, "curl_easy_init failed");
		return 2;
	}

	int rc = 0;
	mem_buf_t buf = {0};

	char *q = curl_easy_escape(curl, query, 0);
	if (!q) {
		set_err(out->error, "curl_easy_escape failed");
		rc = 2;
		goto done;
	}

	char url[2048];
	int n = snprintf(url, sizeof(url),
			 "https://api.search.brave.com/res/v1/web/search?q=%s&count=%d", q,
			 count);
	curl_free(q);
	if (n <= 0 || (size_t)n >= sizeof(url)) {
		set_err(out->error, "url too long");
		rc = 2;
		goto done;
	}

	// Optional parameters
	if (lang && lang[0]) {
		char *l = curl_easy_escape(curl, lang, 0);
		if (l) {
			strncat(url, "&search_lang=", sizeof(url) - strlen(url) - 1);
			strncat(url, l, sizeof(url) - strlen(url) - 1);
			curl_free(l);
		}
	}
	if (freshness && freshness[0]) {
		char *f = curl_easy_escape(curl, freshness, 0);
		if (f) {
			strncat(url, "&freshness=", sizeof(url) - strlen(url) - 1);
			strncat(url, f, sizeof(url) - strlen(url) - 1);
			curl_free(f);
		}
	}

	struct curl_slist *headers = NULL;
	char auth[512];
	snprintf(auth, sizeof(auth), "X-Subscription-Token: %s", api_key);
	headers = curl_slist_append(headers, auth);
	headers = curl_slist_append(headers, "Accept: application/json");

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "aicli/0.0.0");
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	// Hard cap: if server sends too much, abort via write_cb returning 0.
	// (mem_buf_reserve caps at 16 MiB)

	CURLcode cc = curl_easy_perform(curl);
	if (cc != CURLE_OK) {
		set_err(out->error, curl_easy_strerror(cc));
		rc = 2;
		goto done;
	}

	long status = 0;
	(void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	out->http_status = (int)status;
	out->body = buf.data;
	out->body_len = buf.len;
	buf.data = NULL;
	buf.len = 0;
	buf.cap = 0;

done:
	mem_buf_free(&buf);
	if (headers)
		curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return rc;
}

void aicli_brave_response_free(aicli_brave_response_t *res)
{
	if (!res)
		return;
	free(res->body);
	res->body = NULL;
	res->body_len = 0;
}

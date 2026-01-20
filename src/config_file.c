#include "aicli_config_file.h"

#include "path_util.h"

#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <yyjson.h>

static char *dup_cstr(const char *s)
{
	if (!s)
		return NULL;
	size_t n = strlen(s);
	char *p = (char *)malloc(n + 1);
	if (!p)
		return NULL;
	memcpy(p, s, n + 1);
	return p;
}

static bool path_is_under_home(const char *path, const char *home)
{
	if (!path || !home || !home[0])
		return false;
	size_t hn = strlen(home);
	if (strncmp(path, home, hn) != 0)
		return false;
	// exact match or a slash boundary
	return path[hn] == '\0' || path[hn] == '/';
}

static char *join_path2(const char *dir, const char *name)
{
	if (!dir || !name)
		return NULL;
	size_t dn = strlen(dir);
	size_t nn = strlen(name);
	bool need_slash = (dn > 0 && dir[dn - 1] != '/');
	size_t need = dn + (need_slash ? 1 : 0) + nn + 1;
	char *p = (char *)malloc(need);
	if (!p)
		return NULL;
	if (need_slash)
		snprintf(p, need, "%s/%s", dir, name);
	else
		snprintf(p, need, "%s%s", dir, name);
	return p;
}

static char *dirname_dup(const char *path)
{
	if (!path)
		return NULL;
	const char *slash = strrchr(path, '/');
	if (!slash)
		return dup_cstr(".");
	if (slash == path)
		return dup_cstr("/");
	size_t n = (size_t)(slash - path);
	char *p = (char *)malloc(n + 1);
	if (!p)
		return NULL;
	memcpy(p, path, n);
	p[n] = '\0';
	return p;
}

static bool file_exists(const char *path)
{
	if (!path || !path[0])
		return false;
	return access(path, R_OK) == 0;
}

static bool is_secure_config_path(const char *path)
{
	if (!path || !path[0])
		return false;
	struct stat st;
	if (stat(path, &st) != 0)
		return false;
	if (!S_ISREG(st.st_mode))
		return false;
	uid_t uid = getuid();
	if (st.st_uid != uid)
		return false;
	// Disallow any group/other access (read/write/exec) to avoid leaking secrets.
	if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
		return false;
	return true;
}

bool aicli_config_file_is_secure(const aicli_config_file_t *cf)
{
	if (!cf || !cf->path)
		return false;
	return is_secure_config_path(cf->path);
}

static bool find_in_dir(aicli_config_file_t *out, const char *dir)
{
	char *cand = join_path2(dir, AICLI_DEFAULT_CONFIG_FILENAME);
	if (!cand)
		return false;
	if (!file_exists(cand)) {
		free(cand);
		return false;
	}
	char *rp = aicli_realpath_dup(cand);
	free(cand);
	if (!rp)
		return false;
	if (!is_secure_config_path(rp)) {
		free(rp);
		return false;
	}
	out->path = rp;
	out->dir = dirname_dup(rp);
	if (!out->dir) {
		free(out->path);
		out->path = NULL;
		return false;
	}
	return true;
}

bool aicli_config_file_find(aicli_config_file_t *out)
{
	if (!out)
		return false;
	memset(out, 0, sizeof(*out));

	const char *home = getenv("HOME");
	if (!home || !home[0])
		return false;

	char cwd_buf[PATH_MAX];
	if (!getcwd(cwd_buf, sizeof(cwd_buf)))
		return false;

	char *cwd_rp = aicli_realpath_dup(cwd_buf);
	if (!cwd_rp)
		return false;

	// Only consider cwd/parents if cwd is under home.
	if (!path_is_under_home(cwd_rp, home)) {
		free(cwd_rp);
		// Fallback to $HOME only.
		return find_in_dir(out, home);
	}

	// Walk from cwd up to home (inclusive).
	char *cur = cwd_rp;
	while (1) {
		if (find_in_dir(out, cur)) {
			free(cur);
			return true;
		}
		if (strcmp(cur, home) == 0 || strcmp(cur, "/") == 0)
			break;
		char *parent = dirname_dup(cur);
		if (!parent)
			break;
		if (strcmp(parent, cur) == 0) {
			free(parent);
			break;
		}
		free(cur);
		cur = parent;
		// Stop once we escaped home (paranoia).
		if (!path_is_under_home(cur, home))
			break;
	}
	free(cur);

	// Finally, try $HOME.
	return find_in_dir(out, home);
}

void aicli_config_file_free(aicli_config_file_t *cf)
{
	if (!cf)
		return;
	free(cf->path);
	free(cf->dir);
	memset(cf, 0, sizeof(*cf));
}

static void apply_str_if_present(const char **dst, yyjson_val *obj, const char *key)
{
	yyjson_val *v = yyjson_obj_get(obj, key);
	if (v && yyjson_is_str(v))
		*dst = yyjson_get_str(v);
}

bool aicli_config_load_from_file(aicli_config_t *cfg, const aicli_config_file_t *cf)
{
	if (!cfg || !cf || !cf->path)
		return false;

	FILE *fp = fopen(cf->path, "rb");
	if (!fp)
		return false;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return false;
	}
	long n = ftell(fp);
	if (n < 0 || n > (1024 * 1024)) {
		fclose(fp);
		return false;
	}
	rewind(fp);

	char *buf = (char *)malloc((size_t)n + 1);
	if (!buf) {
		fclose(fp);
		return false;
	}
	size_t got = fread(buf, 1, (size_t)n, fp);
	fclose(fp);
	buf[got] = '\0';

	yyjson_doc *doc = yyjson_read(buf, got, 0);
	free(buf);
	if (!doc)
		return false;

	yyjson_val *root = yyjson_doc_get_root(doc);
	if (!root || !yyjson_is_obj(root)) {
		yyjson_doc_free(doc);
		return false;
	}

	// Note: strings point into yyjson_doc; caller must keep doc alive if used.
	// For now we copy into process-lifetime by duplicating into heap.
	// We'll store duplicated strings in aicli_config_t via existing env pointers
	// model; to avoid a larger refactor, we duplicate into malloc and leak-free via
	// future config_free().

	const char *tmp_model = NULL;
	const char *tmp_openai_key = NULL;
	const char *tmp_base = NULL;
	const char *tmp_google_key = NULL;
	const char *tmp_google_cx = NULL;
	const char *tmp_brave_key = NULL;
	const char *tmp_provider = NULL;

	apply_str_if_present(&tmp_model, root, "model");
	apply_str_if_present(&tmp_openai_key, root, "openai_api_key");
	apply_str_if_present(&tmp_base, root, "openai_base_url");
	apply_str_if_present(&tmp_provider, root, "search_provider");
	apply_str_if_present(&tmp_google_key, root, "google_api_key");
	apply_str_if_present(&tmp_google_cx, root, "google_cse_cx");
	apply_str_if_present(&tmp_brave_key, root, "brave_api_key");

	if (tmp_openai_key)
	{
		if (cfg->openai_api_key_owned)
			free((void *)cfg->openai_api_key);
		cfg->openai_api_key = dup_cstr(tmp_openai_key);
		cfg->openai_api_key_owned = (cfg->openai_api_key != NULL);
	}
	if (tmp_model)
	{
		if (cfg->model_owned)
			free((void *)cfg->model);
		cfg->model = dup_cstr(tmp_model);
		cfg->model_owned = (cfg->model != NULL);
	}
	if (tmp_base)
	{
		if (cfg->openai_base_url_owned)
			free((void *)cfg->openai_base_url);
		cfg->openai_base_url = dup_cstr(tmp_base);
		cfg->openai_base_url_owned = (cfg->openai_base_url != NULL);
	}
	if (tmp_google_key)
	{
		if (cfg->google_api_key_owned)
			free((void *)cfg->google_api_key);
		cfg->google_api_key = dup_cstr(tmp_google_key);
		cfg->google_api_key_owned = (cfg->google_api_key != NULL);
	}
	if (tmp_google_cx)
	{
		if (cfg->google_cse_cx_owned)
			free((void *)cfg->google_cse_cx);
		cfg->google_cse_cx = dup_cstr(tmp_google_cx);
		cfg->google_cse_cx_owned = (cfg->google_cse_cx != NULL);
	}
	if (tmp_brave_key)
	{
		if (cfg->brave_api_key_owned)
			free((void *)cfg->brave_api_key);
		cfg->brave_api_key = dup_cstr(tmp_brave_key);
		cfg->brave_api_key_owned = (cfg->brave_api_key != NULL);
	}

	if (tmp_provider) {
		if (strcmp(tmp_provider, "google") == 0 || strcmp(tmp_provider, "google_cse") == 0)
			cfg->search_provider = AICLI_SEARCH_PROVIDER_GOOGLE_CSE;
		else if (strcmp(tmp_provider, "brave") == 0)
			cfg->search_provider = AICLI_SEARCH_PROVIDER_BRAVE;
	}

	yyjson_doc_free(doc);
	return true;
}

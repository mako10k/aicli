#include "continue_state.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char *mode_to_string(aicli_continue_mode_t m) {
	switch (m) {
	case AICLI_CONTINUE_AUTO:
		return "auto";
	case AICLI_CONTINUE_BOTH:
		return "both";
	case AICLI_CONTINUE_AFTER:
		return "after";
	case AICLI_CONTINUE_NEXT:
		return "next";
	}
	return "auto";
}

static int parse_mode(const char *s, aicli_continue_mode_t *out) {
	if (s == NULL || *s == '\0') {
		*out = AICLI_CONTINUE_AUTO;
		return 0;
	}
	if (strcmp(s, "auto") == 0) {
		*out = AICLI_CONTINUE_AUTO;
		return 0;
	}
	if (strcmp(s, "both") == 0) {
		*out = AICLI_CONTINUE_BOTH;
		return 0;
	}
	if (strcmp(s, "after") == 0) {
		*out = AICLI_CONTINUE_AFTER;
		return 0;
	}
	if (strcmp(s, "next") == 0) {
		*out = AICLI_CONTINUE_NEXT;
		return 0;
	}
	return -1;
}

static void sanitize_thread_name(const char *in, char out[64], bool *has) {
	*has = false;
	out[0] = '\0';
	if (in == NULL || *in == '\0') return;

	size_t j = 0;
	for (size_t i = 0; in[i] != '\0' && j + 1 < 64; i++) {
		unsigned char c = (unsigned char)in[i];
		if (isalnum(c) || c == '-' || c == '_') {
			out[j++] = (char)c;
		} else if (c == ' ' || c == '.' || c == ':' || c == '/') {
			out[j++] = '_';
		} else {
			// skip
		}
	}
	out[j] = '\0';
	if (j > 0) *has = true;
}

int aicli_continue_parse(const char *optarg, aicli_continue_opt_t *out) {
	if (out == NULL) return -1;
	memset(out, 0, sizeof(*out));
	out->mode = AICLI_CONTINUE_AUTO;

	if (optarg == NULL || *optarg == '\0') {
		return 0;
	}

	char buf[128];
	strncpy(buf, optarg, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *eq = strchr(buf, '=');
	if (eq) {
		*eq = '\0';
		const char *mode_s = buf;
		const char *thread_s = eq + 1;
		aicli_continue_mode_t m;
		if (parse_mode(mode_s, &m) != 0) return -1;
		out->mode = m;
		sanitize_thread_name(thread_s, out->thread_name, &out->has_thread);
		return 0;
	}

	// If only one token and it's not a known mode, treat it as THREAD (mode=auto)
	aicli_continue_mode_t m;
	if (parse_mode(buf, &m) == 0) {
		out->mode = m;
		return 0;
	}

	sanitize_thread_name(buf, out->thread_name, &out->has_thread);
	return 0;
}

static int mkdir_p_0700(const char *path) {
	struct stat st;
	if (stat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode)) return 0;
		errno = ENOTDIR;
		return -1;
	}
	if (mkdir(path, 0700) == 0) return 0;
	return -1;
}

static const char *pick_runtime_dir(void) {
	const char *d = getenv("XDG_RUNTIME_DIR");
	if (d && *d) return d;
	d = getenv("TMPDIR");
	if (d && *d) return d;
	return "/tmp";
}

int aicli_continue_state_path(char *out_path, size_t out_cap,
			     long sid,
			     const aicli_continue_opt_t *opt) {
	if (out_path == NULL || out_cap == 0) return -1;
	const char *base = pick_runtime_dir();

	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/%s", base, "aicli");
	if (mkdir_p_0700(dir) != 0 && errno != EEXIST) {
		return -1;
	}

	// Use session id (getsid) to allow continuity across separate invocations.
	// Optional THREAD suffix allows multiple independent conversations.
	if (opt && opt->has_thread) {
		return snprintf(out_path, out_cap, "%s/.previous_response_id_s%ld_%s", dir,
		                sid, opt->thread_name) < (int)out_cap
		           ? 0
		           : -1;
	}
	return snprintf(out_path, out_cap, "%s/.previous_response_id_s%ld", dir, sid) < (int)out_cap
	           ? 0
	           : -1;
}

int aicli_continue_read_id(const char *path, char *out_id, size_t out_cap) {
	if (path == NULL || out_id == NULL || out_cap == 0) return -1;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT) return 1;
		return -1;
	}
	ssize_t n = read(fd, out_id, (ssize_t)out_cap - 1);
	close(fd);
	if (n <= 0) return -1;
	out_id[n] = '\0';
	// trim newline
	size_t len = strlen(out_id);
	while (len > 0 && (out_id[len - 1] == '\n' || out_id[len - 1] == '\r')) {
		out_id[len - 1] = '\0';
		len--;
	}
	return 0;
}

int aicli_continue_write_id(const char *path, const char *response_id) {
	if (path == NULL || response_id == NULL || *response_id == '\0') return -1;

	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) return -1;

	size_t len = strlen(response_id);
	if (write(fd, response_id, len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
		close(fd);
		unlink(tmp);
		return -1;
	}
	fsync(fd);
	close(fd);

	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

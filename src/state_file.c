/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * state_file.c - the persistent key=value state file.
 *
 * The portal backend remembers the last source kind and the direct backend
 * remembers the last source spec.  Both used to truncate the same file, so
 * each write erased the other key.  Every update now reads the file, replaces
 * (or appends) a single key and renames a temporary file over the original.
 * A lock file serialises concurrent writers (--allow-multiple).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "state_file.h"
#include "util.h"

#define VALUE_MAX 512

char *state_path_for_token(const char *token_path) {
	const char *slash;
	char *path;
	size_t length;

	if(!token_path)
		return NULL;

	slash = strrchr(token_path, '/');
	if(!slash)
		return strdup("wayland-share-bridge.state");

	length = (size_t) (slash - token_path) + 1;
	path = malloc(length + sizeof("state"));
	if(!path)
		return NULL;

	memcpy(path, token_path, length);
	memcpy(path + length, "state", sizeof("state"));

	return path;
}

char *state_read(const char *path, const char *key) {
	FILE *file;
	char line[1024];
	char *found = NULL;
	size_t key_len;

	if(!path || !key)
		return NULL;

	file = fopen(path, "r");
	if(!file)
		return NULL;

	key_len = strlen(key);
	while(fgets(line, sizeof(line), file)) {
		size_t length = strcspn(line, "\r\n");

		line[length] = '\0';
		if(strncmp(line, key, key_len) != 0 || line[key_len] != '=')
			continue;

		free(found);
		found = strdup(line + key_len + 1);
	}
	fclose(file);

	return found;
}

static int line_matches(const char *line, const char *key, size_t key_len) {
	return strncmp(line, key, key_len) == 0 && line[key_len] == '=';
}

static int fsync_directory(const char *path) {
	char *copy, *slash;
	int fd, ret = -1;

	copy = strdup(path);
	if(!copy)
		return -1;

	slash = strrchr(copy, '/');
	if(slash) {
		if(slash == copy)
			slash[1] = '\0';
		else
			*slash = '\0';
	} else {
		strcpy(copy, ".");
	}

	fd = open(copy, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if(fd >= 0) {
		ret = fsync(fd);
		close(fd);
	}

	free(copy);
	return ret;
}

int state_write(const char *path, const char *key, const char *value) {
	char clean[VALUE_MAX];
	char *tmp_path = NULL;
	char *lock_path = NULL;
	FILE *input = NULL, *output = NULL;
	char line[1024];
	size_t key_len, i;
	int lock_fd = -1;
	int fd = -1;
	int replaced = 0;
	int have_version = 0;
	int failed = 0;
	int ret = -1;

	if(!path || !key || !value)
		return -1;

	/* The schema version is owned by this file format. */
	if(!strcmp(key, "version"))
		value = STATE_FILE_VERSION;

	snprintf(clean, sizeof(clean), "%s", value);
	for(i = 0; clean[i]; i++) {
		if(clean[i] == '\n' || clean[i] == '\r')
			clean[i] = ' ';
	}
	key_len = strlen(key);

	if(path_mkdir_parents(path) != 0) {
		fprintf(stderr, "warning: cannot update %s\n", path);
		return -1;
	}

	lock_path = malloc(strlen(path) + sizeof(".lock"));
	if(!lock_path)
		return -1;
	sprintf(lock_path, "%s.lock", path);

	lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if(lock_fd >= 0)
		flock(lock_fd, LOCK_EX);

	tmp_path = malloc(strlen(path) + sizeof(".tmp.XXXXXX"));
	if(!tmp_path)
		goto out;
	sprintf(tmp_path, "%s.tmp.XXXXXX", path);

	fd = mkstemp(tmp_path);
	if(fd < 0) {
		fprintf(stderr, "warning: cannot update %s\n", path);
		goto out;
	}
	if(fchmod(fd, 0600) != 0)
		fprintf(stderr, "warning: cannot set the permissions of %s\n", tmp_path);

	output = fdopen(fd, "w");
	if(!output) {
		close(fd);
		fd = -1;
		goto out;
	}
	fd = -1;	/* owned by output now */

	input = fopen(path, "r");
	while(input && fgets(line, sizeof(line), input)) {
		if(line_matches(line, "version", strlen("version")))
			have_version = 1;
		if(!line_matches(line, key, key_len)) {
			if(fputs(line, output) == EOF)
				failed = 1;
		} else if(!replaced) {
			if(fprintf(output, "%s=%s\n", key, clean) < 0)
				failed = 1;
			replaced = 1;
		}
		/* Later duplicates of the same key are dropped. */
		if(failed)
			break;
	}
	if(input)
		fclose(input);
	input = NULL;

	if(!failed && !replaced && fprintf(output, "%s=%s\n", key, clean) < 0)
		failed = 1;
	if(!failed && !have_version && strcmp(key, "version") != 0 &&
	   fprintf(output, "version=%s\n", STATE_FILE_VERSION) < 0)
		failed = 1;
	if(!failed && fflush(output) != 0)
		failed = 1;
	if(!failed && fsync(fileno(output)) != 0)
		failed = 1;
	if(fclose(output) != 0)
		failed = 1;
	output = NULL;

	if(failed || rename(tmp_path, path) != 0) {
		fprintf(stderr, "warning: cannot update %s\n", path);
		goto out;
	}
	fsync_directory(path);
	ret = 0;

out:
	if(input)
		fclose(input);
	if(output)
		fclose(output);
	if(fd >= 0)
		close(fd);
	if(tmp_path) {
		if(ret != 0)
			unlink(tmp_path);
		free(tmp_path);
	}
	if(lock_fd >= 0) {
		flock(lock_fd, LOCK_UN);
		close(lock_fd);
	}
	free(lock_path);

	return ret;
}

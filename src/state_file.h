/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef STATE_FILE_H
#define STATE_FILE_H

/*
 * The small key=value file the bridge uses to remember the last source and the
 * source kind.  Both capture backends write to it, so every update merges into
 * the existing file instead of truncating it.
 */

/* Schema version written to the file (see CHANGELOG for migrations). */
#define STATE_FILE_VERSION "1"

/* "<dirname of token_path>/state"; NULL when token_path is NULL. */
char *state_path_for_token(const char *token_path);

/* Value of the last "key=value" line, malloc'd; NULL when absent. */
char *state_read(const char *path, const char *key);

/* Merges key=value into the file and replaces it atomically.  0 on success. */
int state_write(const char *path, const char *key, const char *value);

#endif /* STATE_FILE_H */

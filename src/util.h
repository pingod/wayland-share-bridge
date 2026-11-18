/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/*
 * Copies src into dst (size bytes) without ever splitting a UTF-8 code
 * point.  Invalid bytes are replaced by '?', so the result is always valid
 * UTF-8 - libdbus aborts the process when a string property is not.
 * dst is always NUL-terminated.  Returns strlen(dst).
 */
size_t utf8_copy(char *dst, size_t size, const char *src);

/* 1 when the string is valid UTF-8; the empty string counts as valid. */
int utf8_valid(const char *text);

/* Shortens text to roughly max_columns display columns, adding an ellipsis. */
void ellipsize(const char *text, char *buffer, size_t size, int max_columns);

/* Creates every parent directory of path (like mkdir -p on its dirname). */
int path_mkdir_parents(const char *path);

#endif /* UTIL_H */

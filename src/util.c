/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * util.c - small helpers shared by the bridge.
 *
 * The fixed-size buffers in this project hold window titles and app ids that
 * come from the compositor in arbitrary UTF-8.  Plain snprintf() truncation
 * can cut a multi-byte sequence in half, and libdbus aborts the whole process
 * when it is handed such a string (dbus_message_iter_append_basic() checks
 * the encoding).  utf8_copy() truncates on code point boundaries instead.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "util.h"

/*
 * Decodes one UTF-8 sequence.  *length is set to the number of bytes used,
 * or 0 when the byte does not start a valid sequence.  The caller passes a
 * NUL-terminated string, so the continuation checks below never read past
 * the terminator.
 */
static unsigned int utf8_decode(const unsigned char *p, int *length) {
	unsigned int cp;

	if(p[0] < 0x80) {
		*length = 1;
		return p[0];
	}

	if(p[0] >= 0xc2 && p[0] <= 0xdf && (p[1] & 0xc0) == 0x80) {
		*length = 2;
		return ((unsigned int) (p[0] & 0x1f) << 6) | (p[1] & 0x3f);
	}

	if(p[0] >= 0xe0 && p[0] <= 0xef &&
	   (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
		cp = ((unsigned int) (p[0] & 0x0f) << 12) |
		     ((unsigned int) (p[1] & 0x3f) << 6) | (p[2] & 0x3f);
		if(cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)) {
			*length = 0;
			return 0;
		}
		*length = 3;
		return cp;
	}

	if(p[0] >= 0xf0 && p[0] <= 0xf4 &&
	   (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
		cp = ((unsigned int) (p[0] & 0x07) << 18) |
		     ((unsigned int) (p[1] & 0x3f) << 12) |
		     ((unsigned int) (p[2] & 0x3f) << 6) | (p[3] & 0x3f);
		if(cp < 0x10000 || cp > 0x10ffff) {
			*length = 0;
			return 0;
		}
		*length = 4;
		return cp;
	}

	*length = 0;
	return 0;
}

size_t utf8_copy(char *dst, size_t size, const char *src) {
	const unsigned char *p = (const unsigned char *) (src ? src : "");
	size_t used = 0;

	if(size == 0)
		return 0;

	while(*p) {
		int length = 0;

		utf8_decode(p, &length);
		if(length == 0) {
			if(used + 1 >= size)
				break;
			dst[used++] = '?';
			p++;
			continue;
		}
		if(used + (size_t) length >= size)
			break;
		memcpy(dst + used, p, (size_t) length);
		used += (size_t) length;
		p += length;
	}

	dst[used] = '\0';
	return used;
}

int utf8_valid(const char *text) {
	const unsigned char *p = (const unsigned char *) (text ? text : "");

	while(*p) {
		int length = 0;

		utf8_decode(p, &length);
		if(length == 0)
			return 0;
		p += length;
	}
	return 1;
}

void ellipsize(const char *text, char *buffer, size_t size, int max_columns) {
	const unsigned char *p = (const unsigned char *) (text ? text : "");
	size_t used = 0;
	int columns = 0;

	if(size == 0)
		return;

	while(*p) {
		unsigned int cp;
		int length = 0;
		int width;

		cp = utf8_decode(p, &length);
		if(length == 0) {
			cp = '?';
			length = 1;
		}

		width = (cp >= 0x1100 && (cp <= 0x115f || cp == 0x2329 || cp == 0x232a ||
			(cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) ||
			(cp >= 0xac00 && cp <= 0xd7a3) ||
			(cp >= 0xf900 && cp <= 0xfaff) ||
			(cp >= 0xfe30 && cp <= 0xfe4f) ||
			(cp >= 0xff00 && cp <= 0xff60) ||
			(cp >= 0xffe0 && cp <= 0xffe6) ||
			(cp >= 0x1f300 && cp <= 0x1f64f) ||
			(cp >= 0x1f900 && cp <= 0x1f9ff) ||
			(cp >= 0x20000 && cp <= 0x3fffd))) ? 2 : 1;

		if(columns + width > max_columns) {
			if(used + 3 < size) {
				memcpy(buffer + used, "\xe2\x80\xa6", 3);
				used += 3;
			}
			break;
		}
		if(used + (size_t) length >= size)
			break;
		memcpy(buffer + used, p, (size_t) length);
		used += (size_t) length;
		columns += width;
		p += length;
	}

	buffer[used] = '\0';
}

int path_mkdir_parents(const char *path) {
	char *copy, *p;
	int ret = 0;

	if(!path)
		return -1;

	copy = strdup(path);
	if(!copy)
		return -1;

	for(p = copy + 1; *p; p++) {
		if(*p != '/')
			continue;

		*p = 0;
		if(mkdir(copy, 0700) != 0 && errno != EEXIST) {
			ret = -1;
			break;
		}
		*p = '/';
	}

	free(copy);
	return ret;
}

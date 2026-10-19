/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

enum cursor_mode {
	CURSOR_HIDDEN = 0,
	CURSOR_EMBEDDED = 1,
	CURSOR_METADATA = 2,
};

/*
 * What the user is allowed to pick in the portal dialog.  The portal does not
 * offer a way to preselect a source, but the "types" option still lets us ask
 * for screens only or windows only, which is the next best thing.
 */
enum source_kind {
	SOURCE_KIND_AUTO = -1,   /* remembered from the last run, else ANY */
	SOURCE_KIND_ANY = 0,
	SOURCE_KIND_SCREEN = 1,
	SOURCE_KIND_WINDOW = 2,
};

struct source_info {
	int valid;
	int type; /* 1 = monitor, 2 = window */
	int x, y, width, height;
};

struct options {
	const char *title;
	const char *class_name;
	enum cursor_mode cursor;
	enum source_kind source_kind;
	int remember;
	int show;
	int tray;
	const char *token_file;
};

#endif /* CONFIG_H */

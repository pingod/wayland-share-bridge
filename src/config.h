/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* Values accepted by the portal's ScreenCast cursor_mode option. */
enum cursor_mode {
	CURSOR_HIDDEN = 1,
	CURSOR_EMBEDDED = 2,
	CURSOR_METADATA = 4,
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

/* What the direct capture backend should mirror. */
struct source_request {
	enum source_kind kind;   /* SCREEN or WINDOW */
	char id[192];            /* output name / toplevel identifier, "" = first */
	int cropped;             /* 1 = take x,y,width,height out of the screen */
	int x, y;
	unsigned int width, height;
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

/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef SOURCE_SPEC_H
#define SOURCE_SPEC_H

#include "config.h"

/*
 * Parsers for the command line values.  They live in their own translation
 * unit so they can be unit tested without starting the bridge.
 */

/* "hidden"/"none", "embedded"/"always", "metadata"/"on-demand". */
int parse_cursor_mode(const char *value, enum cursor_mode *out);

/* "auto", "any", "screen"/"monitor", "window". */
int parse_source_kind(const char *value, enum source_kind *out);

/* "screen:NAME", "window:APP_ID", "window:APP_ID|TITLE" or
 * "region:NAME,X,Y,W,H". */
int parse_source_spec(const char *spec, struct source_request *req);

#endif /* SOURCE_SPEC_H */

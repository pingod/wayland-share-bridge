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

/* "source"/"original", "1080p"/"high", "720p"/"medium", "480p"/"low". */
int parse_quality(const char *value, enum capture_quality *out);

/* An integer frame rate in [FPS_MIN, FPS_MAX]. */
int parse_fps(const char *value, int *out);

/* Stable name used in the state file ("source", "1080p", ...). */
const char *quality_name(enum capture_quality quality);

/* Resolution cap of a preset; 0/0 means "no cap" (QUALITY_SOURCE). */
void quality_limits(enum capture_quality quality,
		    unsigned int *max_width, unsigned int *max_height);

#endif /* SOURCE_SPEC_H */

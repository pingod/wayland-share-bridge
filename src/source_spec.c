/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * source_spec.c - parsers for the --cursor, --source-kind and --source
 * values.  Extracted from main.c so tests/ can exercise them.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "source_spec.h"

int parse_cursor_mode(const char *value, enum cursor_mode *out) {
	if(!value || !out)
		return -1;

	if(!strcmp(value, "hidden") || !strcmp(value, "none")) {
		*out = CURSOR_HIDDEN;
		return 0;
	}
	if(!strcmp(value, "embedded") || !strcmp(value, "always")) {
		*out = CURSOR_EMBEDDED;
		return 0;
	}
	if(!strcmp(value, "metadata") || !strcmp(value, "on-demand")) {
		*out = CURSOR_METADATA;
		return 0;
	}

	return -1;
}

int parse_source_kind(const char *value, enum source_kind *out) {
	if(!value || !out)
		return -1;

	if(!strcmp(value, "auto")) {
		*out = SOURCE_KIND_AUTO;
		return 0;
	}
	if(!strcmp(value, "any")) {
		*out = SOURCE_KIND_ANY;
		return 0;
	}
	if(!strcmp(value, "screen") || !strcmp(value, "monitor")) {
		*out = SOURCE_KIND_SCREEN;
		return 0;
	}
	if(!strcmp(value, "window")) {
		*out = SOURCE_KIND_WINDOW;
		return 0;
	}

	return -1;
}

/* "NAME,X,Y,W,H" of a region spec. */
static int parse_region(const char *value, struct source_request *req) {
	const char *comma = strchr(value, ',');
	char name[192];
	char *end;
	long numbers[4];
	size_t name_len;
	int i;

	if(!comma)
		return -1;
	name_len = (size_t) (comma - value);
	if(name_len == 0 || name_len >= sizeof(name))
		return -1;
	memcpy(name, value, name_len);
	name[name_len] = '\0';

	value = comma + 1;
	for(i = 0; i < 4; i++) {
		errno = 0;
		numbers[i] = strtol(value, &end, 10);
		if(end == value || errno == ERANGE)
			return -1;
		if(i < 3) {
			if(*end != ',')
				return -1;
			value = end + 1;
		} else if(*end != '\0') {
			return -1;
		}
	}

	if(numbers[0] < 0 || numbers[0] > 65535 ||
	   numbers[1] < 0 || numbers[1] > 65535 ||
	   numbers[2] < 1 || numbers[2] > 65535 ||
	   numbers[3] < 1 || numbers[3] > 65535)
		return -1;

	req->kind = SOURCE_KIND_SCREEN;
	req->cropped = 1;
	req->x = (int) numbers[0];
	req->y = (int) numbers[1];
	req->width = (unsigned int) numbers[2];
	req->height = (unsigned int) numbers[3];
	memcpy(req->id, name, name_len + 1);

	return 0;
}

int parse_source_spec(const char *spec, struct source_request *req) {
	size_t length;

	if(!spec || !req)
		return -1;

	memset(req, 0, sizeof(*req));
	if(!strncmp(spec, "region:", 7))
		return parse_region(spec + 7, req);
	if(!strncmp(spec, "screen:", 7))
		req->kind = SOURCE_KIND_SCREEN;
	else if(!strncmp(spec, "window:", 7))
		req->kind = SOURCE_KIND_WINDOW;
	else
		return -1;

	length = strlen(spec + 7);
	if(length >= sizeof(req->id))
		return -1;
	memcpy(req->id, spec + 7, length + 1);

	return 0;
}

int parse_quality(const char *value, enum capture_quality *out) {
	if(!value || !out)
		return -1;

	if(!strcmp(value, "source") || !strcmp(value, "original") || !strcmp(value, "native"))
		*out = QUALITY_SOURCE;
	else if(!strcmp(value, "1080p") || !strcmp(value, "high"))
		*out = QUALITY_1080P;
	else if(!strcmp(value, "720p") || !strcmp(value, "medium"))
		*out = QUALITY_720P;
	else if(!strcmp(value, "480p") || !strcmp(value, "low"))
		*out = QUALITY_480P;
	else
		return -1;

	return 0;
}

int parse_fps(const char *value, int *out) {
	char *end;
	long fps;

	if(!value || !out)
		return -1;

	errno = 0;
	fps = strtol(value, &end, 10);
	if(end == value || *end != '\0' || errno == ERANGE)
		return -1;
	if(fps < FPS_MIN || fps > FPS_MAX)
		return -1;

	*out = (int) fps;
	return 0;
}

const char *quality_name(enum capture_quality quality) {
	switch(quality) {
	case QUALITY_1080P:
		return "1080p";
	case QUALITY_720P:
		return "720p";
	case QUALITY_480P:
		return "480p";
	default:
		return "source";
	}
}

void quality_limits(enum capture_quality quality,
		    unsigned int *max_width, unsigned int *max_height) {
	unsigned int width = 0, height = 0;

	switch(quality) {
	case QUALITY_1080P:
		width = 1920;
		height = 1080;
		break;
	case QUALITY_720P:
		width = 1280;
		height = 720;
		break;
	case QUALITY_480P:
		width = 854;
		height = 480;
		break;
	default:
		break;
	}

	if(max_width)
		*max_width = width;
	if(max_height)
		*max_height = height;
}

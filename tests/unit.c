/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * unit.c - tests for the pure helpers.
 *
 * The bridge itself needs a compositor and an X server, so the parts that can
 * be verified without one (UTF-8 truncation, source spec parsing, frame
 * conversion and the state file) are covered here.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "image_convert.h"
#include "source_spec.h"
#include "state_file.h"
#include "util.h"

static int failures;

#define CHECK(condition, ...) do { \
	if(!(condition)) { \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
		failures++; \
	} \
} while(0)

static void test_utf8_copy(void) {
	char buffer[32];
	size_t length;

	length = utf8_copy(buffer, sizeof(buffer), "hello");
	CHECK(length == 5 && !strcmp(buffer, "hello"), "plain copy");

	/* 190 bytes + a 3 byte character in a 192 byte buffer: the character
	 * does not fit and must not be cut in half. */
	{
		char source[256];
		char truncated[192];
		size_t i;

		for(i = 0; i < 190; i++)
			source[i] = 'a';
		memcpy(source + 190, "\xe4\xbd\xa0", 3);
		source[193] = '\0';

		utf8_copy(truncated, sizeof(truncated), source);
		CHECK(strlen(truncated) == 190, "truncated at a code point boundary (got %zu)",
		      strlen(truncated));
		CHECK(utf8_valid(truncated), "truncated string is valid UTF-8");
	}

	/* An invalid byte is replaced instead of being passed through. */
	utf8_copy(buffer, sizeof(buffer), "a\xff" "b");
	CHECK(!strcmp(buffer, "a?b"), "invalid byte replaced (got '%s')", buffer);
	CHECK(utf8_valid(buffer), "sanitised string is valid UTF-8");

	/* A truncated multi-byte sequence must not survive either. */
	utf8_copy(buffer, sizeof(buffer), "a\xe4\xbd");
	CHECK(utf8_valid(buffer), "truncated input sanitised");

	/* size 0 must not write. */
	CHECK(utf8_copy(NULL, 0, "x") == 0, "size 0 is a no-op");
}

static void test_utf8_valid(void) {
	CHECK(utf8_valid(""), "empty string");
	CHECK(utf8_valid("hello"), "ascii");
	CHECK(utf8_valid("\xe4\xbd\xa0\xe5\xa5\xbd"), "chinese");
	CHECK(utf8_valid("\xf0\x9f\x98\x80"), "emoji");
	CHECK(!utf8_valid("\xff"), "0xff is invalid");
	CHECK(!utf8_valid("\xc0\x80"), "overlong encoding is invalid");
	CHECK(!utf8_valid("\xed\xa0\x80"), "surrogate is invalid");
}

static void test_ellipsize(void) {
	char buffer[64];

	ellipsize("hello world", buffer, sizeof(buffer), 5);
	CHECK(utf8_valid(buffer), "ellipsized ascii is valid");
	CHECK(strlen(buffer) == 8, "ellipsis appended (got %zu bytes, '%s')",
	      strlen(buffer), buffer);

	ellipsize("\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c", buffer,
		  sizeof(buffer), 3);
	CHECK(utf8_valid(buffer), "ellipsized chinese is valid");
	CHECK(strlen(buffer) == 6, "one wide character plus the ellipsis");
}

static void test_source_spec(void) {
	struct source_request req;

	CHECK(parse_source_spec("screen:DP-2", &req) == 0, "screen spec");
	CHECK(req.kind == SOURCE_KIND_SCREEN && !req.cropped &&
	      !strcmp(req.id, "DP-2"), "screen fields");

	CHECK(parse_source_spec("window:firefox|DeepSeek", &req) == 0, "window spec");
	CHECK(req.kind == SOURCE_KIND_WINDOW &&
	      !strcmp(req.id, "firefox|DeepSeek"), "window fields");

	CHECK(parse_source_spec("region:DP-2,10,20,1280,720", &req) == 0, "region spec");
	CHECK(req.kind == SOURCE_KIND_SCREEN && req.cropped &&
	      req.x == 10 && req.y == 20 && req.width == 1280 && req.height == 720,
	      "region fields");
	CHECK(!strcmp(req.id, "DP-2"), "region name");

	CHECK(parse_source_spec("bogus", &req) != 0, "unknown prefix");
	CHECK(parse_source_spec("screen:", &req) == 0 && req.id[0] == '\0',
	      "empty id means the first source");
	CHECK(parse_source_spec("region:DP-2,0,0,0,10", &req) != 0, "zero width");
	CHECK(parse_source_spec("region:DP-2,0,0,10", &req) != 0, "missing number");

	{
		enum cursor_mode cursor;
		enum source_kind kind;

		CHECK(parse_cursor_mode("hidden", &cursor) == 0 && cursor == CURSOR_HIDDEN,
		      "cursor hidden");
		CHECK(parse_cursor_mode("metadata", &cursor) == 0 && cursor == CURSOR_METADATA,
		      "cursor metadata");
		CHECK(parse_cursor_mode("nope", &cursor) != 0, "cursor invalid");
		CHECK(parse_source_kind("screen", &kind) == 0 && kind == SOURCE_KIND_SCREEN,
		      "kind screen");
		CHECK(parse_source_kind("auto", &kind) == 0 && kind == SOURCE_KIND_AUTO,
		      "kind auto");
		CHECK(parse_source_kind("nope", &kind) != 0, "kind invalid");
	}
}

/* A 3x2 frame: every pixel is (r, g, b, 0xff) with a unique r. */
static void make_frame(unsigned char *frame) {
	unsigned int i;

	for(i = 0; i < 6; i++) {
		frame[i * 4 + 0] = (unsigned char) (i + 1);
		frame[i * 4 + 1] = 0x10;
		frame[i * 4 + 2] = 0x20;
		frame[i * 4 + 3] = 0xff;
	}
}

static int pixel_is(const unsigned char *dst, unsigned int width, unsigned int x,
		    unsigned int y, unsigned char red) {
	const unsigned char *p = dst + ((size_t) y * width + x) * 4;

	return p[0] == red && p[1] == 0x10 && p[2] == 0x20;
}

static void test_image_convert(void) {
	unsigned char source[3 * 2 * 4];
	unsigned char dst[3 * 3 * 4];

	make_frame(source);

	/* identity */
	memset(dst, 0xaa, sizeof(dst));
	CHECK(image_convert(dst, 3, 2, 12, source, sizeof(source), 12, 0, 0, 3, 2, 0, 0) == 0,
	      "identity convert");
	CHECK(pixel_is(dst, 3, 0, 0, 1) && pixel_is(dst, 3, 2, 0, 3) &&
	      pixel_is(dst, 3, 0, 1, 4) && pixel_is(dst, 3, 2, 1, 6),
	      "identity pixels");

	/* crop (1,0,2,1) */
	memset(dst, 0xaa, sizeof(dst));
	CHECK(image_convert(dst, 2, 1, 8, source, sizeof(source), 12, 1, 0, 2, 1, 0, 0) == 0,
	      "crop convert");
	CHECK(pixel_is(dst, 2, 0, 0, 2) && pixel_is(dst, 2, 1, 0, 3), "cropped pixels");

	/* red/blue swap */
	memset(dst, 0xaa, sizeof(dst));
	image_convert(dst, 3, 2, 12, source, sizeof(source), 12, 0, 0, 3, 2, 1, 0);
	CHECK(dst[0] == 0x20 && dst[2] == 1, "swap_rb");

	/* 90 degrees counter-clockwise: C F / B E / A D */
	memset(dst, 0xaa, sizeof(dst));
	CHECK(image_convert(dst, 2, 3, 8, source, sizeof(source), 12, 0, 0, 3, 2, 0, 1) == 0,
	      "rotate 90");
	CHECK(pixel_is(dst, 2, 0, 0, 3) && pixel_is(dst, 2, 1, 0, 6) &&
	      pixel_is(dst, 2, 0, 1, 2) && pixel_is(dst, 2, 1, 1, 5) &&
	      pixel_is(dst, 2, 0, 2, 1) && pixel_is(dst, 2, 1, 2, 4),
	      "rotate 90 pixels");

	/* 270 degrees counter-clockwise: D A / E B / F C */
	memset(dst, 0xaa, sizeof(dst));
	CHECK(image_convert(dst, 2, 3, 8, source, sizeof(source), 12, 0, 0, 3, 2, 0, 3) == 0,
	      "rotate 270");
	CHECK(pixel_is(dst, 2, 0, 0, 4) && pixel_is(dst, 2, 1, 0, 1) &&
	      pixel_is(dst, 2, 0, 1, 5) && pixel_is(dst, 2, 1, 1, 2) &&
	      pixel_is(dst, 2, 0, 2, 6) && pixel_is(dst, 2, 1, 2, 3),
	      "rotate 270 pixels");

	/* 180 degrees: F E D / C B A */
	memset(dst, 0xaa, sizeof(dst));
	CHECK(image_convert(dst, 3, 2, 12, source, sizeof(source), 12, 0, 0, 3, 2, 0, 2) == 0,
	      "rotate 180");
	CHECK(pixel_is(dst, 3, 0, 0, 6) && pixel_is(dst, 3, 2, 0, 4) &&
	      pixel_is(dst, 3, 0, 1, 3) && pixel_is(dst, 3, 2, 1, 1),
	      "rotate 180 pixels");

	/* A smaller frame clears the rest of the destination. */
	memset(dst, 0xaa, sizeof(dst));
	image_convert(dst, 3, 2, 12, source, sizeof(source), 12, 0, 0, 2, 1, 0, 0);
	CHECK(pixel_is(dst, 3, 0, 0, 1) && pixel_is(dst, 3, 1, 0, 2), "partial pixels");
	/* row 0, column 2 (dst_w is 3) and the whole second row (stride 12) */
	CHECK(dst[2 * 4] == 0 && dst[2 * 4 + 3] == 0 &&
	      dst[1 * 12] == 0 && dst[1 * 12 + 11] == 0,
	      "uncovered area cleared");

	/* Invalid input is rejected, not written. */
	CHECK(image_convert(NULL, 3, 2, 12, source, sizeof(source), 12, 0, 0, 3, 2, 0, 0) != 0,
	      "NULL destination rejected");
	CHECK(image_convert(dst, 3, 2, 12, source, sizeof(source), 0, 0, 0, 3, 2, 0, 0) != 0,
	      "zero stride rejected");
}

static char *make_temp_dir(void) {
	char template[] = "/tmp/wsb-test-XXXXXX";
	char *dir = mkdtemp(template);

	return dir ? strdup(dir) : NULL;
}

static void test_state_file(void) {
	char *dir = make_temp_dir();
	char *path, *value;
	pid_t pid;
	int status;

	CHECK(dir != NULL, "temporary directory");
	if(!dir)
		return;

	path = malloc(strlen(dir) + sizeof("/state"));
	sprintf(path, "%s/state", dir);

	CHECK(state_write(path, "source", "screen:DP-2") == 0, "write source");
	CHECK(state_write(path, "source_kind", "screen") == 0, "write source_kind");

	value = state_read(path, "source");
	CHECK(value && !strcmp(value, "screen:DP-2"), "read source (%s)",
	      value ? value : "(null)");
	free(value);

	value = state_read(path, "source_kind");
	CHECK(value && !strcmp(value, "screen"), "read source_kind (%s)",
	      value ? value : "(null)");
	free(value);

	value = state_read(path, "version");
	CHECK(value && !strcmp(value, STATE_FILE_VERSION), "schema version written");
	free(value);

	/* newline injection is flattened into the value */
	CHECK(state_write(path, "source", "screen:a\nscreen:b") == 0, "sanitised write");
	value = state_read(path, "source");
	CHECK(value && !strchr(value, '\n'), "newlines removed");
	free(value);

	/* Two concurrent writers must not lose each other's key. */
	pid = fork();
	if(pid == 0) {
		int i;

		for(i = 0; i < 50; i++)
			state_write(path, "source", "screen:DP-2");
		_exit(0);
	}
	{
		int i;

		for(i = 0; i < 50; i++)
			state_write(path, "source_kind", "screen");
	}
	waitpid(pid, &status, 0);

	value = state_read(path, "source");
	CHECK(value != NULL, "source survived concurrent writers");
	free(value);
	value = state_read(path, "source_kind");
	CHECK(value != NULL, "source_kind survived concurrent writers");
	free(value);

	unlink(path);
	{
		char *lock = malloc(strlen(path) + sizeof(".lock"));
		sprintf(lock, "%s.lock", path);
		unlink(lock);
		free(lock);
	}
	rmdir(dir);
	free(path);
	free(dir);
}

int main(void) {
	test_utf8_copy();
	test_utf8_valid();
	test_ellipsize();
	test_source_spec();
	test_image_convert();
	test_state_file();

	if(failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all unit tests passed\n");
	return 0;
}

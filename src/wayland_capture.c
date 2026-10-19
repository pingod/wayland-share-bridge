/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Direct Wayland capture backend.
 *
 * Instead of going through the desktop portal (which always pops up the
 * compositor's own picker), this backend talks to the compositor directly:
 * it lists outputs and foreign toplevels itself and mirrors the chosen one
 * with the ext-image-copy-capture protocol.  That makes switching sources a
 * single menu click.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "wayland_capture.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#define FRAME_INTERVAL_MS 33

struct capture_output {
	struct wl_output *output;
	uint32_t name;
	char name_text[96];
	int width;
	int height;
	struct capture_output *next;
};

struct capture_toplevel {
	struct ext_foreign_toplevel_handle_v1 *handle;
	char app_id[96];
	char title[192];
	char identifier[192];
	int closed;
	struct capture_toplevel *next;
};

static struct {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_shm *shm;
	struct ext_output_image_capture_source_manager_v1 *output_source_mgr;
	struct ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_source_mgr;
	struct ext_image_copy_capture_manager_v1 *capture_mgr;
	struct ext_foreign_toplevel_list_v1 *toplevel_list;

	struct capture_output *outputs;
	struct capture_toplevel *toplevels;

	struct ext_image_capture_source_v1 *source;
	struct ext_image_copy_capture_session_v1 *session;
	struct ext_image_copy_capture_frame_v1 *frame;
	struct wl_buffer *buffer;
	void *data;
	size_t data_size;
	int width;
	int height;
	uint32_t format;
	int session_ready;
	int frame_pending;
	int stopped;
	int failed;
	uint32_t last_frame_ms;

	struct xdata *xdata;
	enum source_kind current_kind;
	char current_label[192];
} cap = {
	.format = WL_SHM_FORMAT_XRGB8888,
};

static uint32_t now_ms(void) {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t) (ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

static void copy_text(char *dst, size_t size, const char *src) {
	if(!src)
		src = "";
	snprintf(dst, size, "%s", src);
}

/* ------------------------------------------------------------------ */
/* wl_output                                                          */

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
			    int32_t physical_width, int32_t physical_height, int32_t subpixel,
			    const char *make, const char *model, int32_t transform) {
	(void) data; (void) output; (void) x; (void) y; (void) physical_width;
	(void) physical_height; (void) subpixel; (void) make; (void) model; (void) transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
			int32_t width, int32_t height, int32_t refresh) {
	struct capture_output *item = data;

	(void) output; (void) refresh;
	if(!(flags & WL_OUTPUT_MODE_CURRENT))
		return;
	item->width = width;
	item->height = height;
}

static void output_done(void *data, struct wl_output *output) {
	(void) data; (void) output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor) {
	(void) data; (void) output; (void) factor;
}

static void output_name(void *data, struct wl_output *output, const char *name) {
	struct capture_output *item = data;

	(void) output;
	copy_text(item->name_text, sizeof(item->name_text), name);
}

static void output_description(void *data, struct wl_output *output, const char *description) {
	(void) data; (void) output; (void) description;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

/* ------------------------------------------------------------------ */
/* ext_foreign_toplevel_handle_v1                                     */

static void toplevel_handle_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
	struct capture_toplevel *item = data;

	(void) handle;
	item->closed = 1;
}

static void toplevel_handle_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
	(void) data; (void) handle;
}

static void toplevel_handle_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
				  const char *title) {
	struct capture_toplevel *item = data;

	(void) handle;
	copy_text(item->title, sizeof(item->title), title);
}

static void toplevel_handle_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
				   const char *app_id) {
	struct capture_toplevel *item = data;

	(void) handle;
	copy_text(item->app_id, sizeof(item->app_id), app_id);
}

static void toplevel_handle_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
				       const char *identifier) {
	struct capture_toplevel *item = data;

	(void) handle;
	copy_text(item->identifier, sizeof(item->identifier), identifier);
}

static const struct ext_foreign_toplevel_handle_v1_listener toplevel_handle_listener = {
	.closed = toplevel_handle_closed,
	.done = toplevel_handle_done,
	.title = toplevel_handle_title,
	.app_id = toplevel_handle_app_id,
	.identifier = toplevel_handle_identifier,
};

static void toplevel_list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
				   struct ext_foreign_toplevel_handle_v1 *handle) {
	struct capture_toplevel *item;

	(void) data; (void) list;
	item = calloc(1, sizeof(*item));
	if(!item)
		return;
	item->handle = handle;
	item->next = cap.toplevels;
	cap.toplevels = item;
	ext_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_handle_listener, item);
}

static void toplevel_list_finished(void *data, struct ext_foreign_toplevel_list_v1 *list) {
	(void) data; (void) list;
}

static const struct ext_foreign_toplevel_list_v1_listener toplevel_list_listener = {
	.toplevel = toplevel_list_toplevel,
	.finished = toplevel_list_finished,
};

/* ------------------------------------------------------------------ */
/* capture session                                                    */

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session,
			       uint32_t format) {
	(void) data; (void) session;
	/* XRGB8888 matches the X11 window layout best; keep looking if unset. */
	if(format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_ARGB8888) {
		if(cap.format != WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_XRGB8888)
			cap.format = format;
	}
}

static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session,
				  struct wl_array *device) {
	(void) data; (void) session; (void) device;
}

static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session,
				  uint32_t format, struct wl_array *modifiers) {
	(void) data; (void) session; (void) format; (void) modifiers;
}

static int create_shm_buffer(void) {
	struct wl_shm_pool *pool;
	int stride = cap.width * 4;
	int size = stride * cap.height;
	int fd;

	fd = memfd_create("wayland-share-bridge", MFD_CLOEXEC);
	if(fd < 0)
		return -1;
	if(ftruncate(fd, size) < 0) {
		close(fd);
		return -1;
	}
	cap.data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(cap.data == MAP_FAILED) {
		cap.data = NULL;
		close(fd);
		return -1;
	}
	cap.data_size = size;
	pool = wl_shm_create_pool(cap.shm, fd, size);
	close(fd);
	if(!pool)
		return -1;
	cap.buffer = wl_shm_pool_create_buffer(pool, 0, cap.width, cap.height, stride, cap.format);
	wl_shm_pool_destroy(pool);
	return cap.buffer ? 0 : -1;
}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame) {
	(void) data;
	if(cap.xdata && cap.data)
		xhandler_draw(cap.xdata, cap.data, (uint32_t) cap.data_size, cap.width * 4);

	cap.frame_pending = 0;
	cap.last_frame_ms = now_ms();
	ext_image_copy_capture_frame_v1_destroy(frame);
	cap.frame = NULL;
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame,
			 uint32_t reason) {
	(void) data;
	fprintf(stderr, "capture: frame failed (reason %u)\n", reason);
	cap.frame_pending = 0;
	cap.failed = 1;
	ext_image_copy_capture_frame_v1_destroy(frame);
	cap.frame = NULL;
}

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame,
			    uint32_t transform) {
	(void) data; (void) frame; (void) transform;
}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame,
			 int32_t x, int32_t y, int32_t width, int32_t height) {
	(void) data; (void) frame; (void) x; (void) y; (void) width; (void) height;
}

static void frame_presentation_time(void *data, struct ext_image_copy_capture_frame_v1 *frame,
				    uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
	(void) data; (void) frame; (void) sec_hi; (void) sec_lo; (void) nsec;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform = frame_transform,
	.damage = frame_damage,
	.presentation_time = frame_presentation_time,
	.ready = frame_ready,
	.failed = frame_failed,
};

static void session_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session,
				uint32_t width, uint32_t height) {
	(void) data; (void) session;
	cap.width = (int) width;
	cap.height = (int) height;
}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *session) {
	(void) data; (void) session;
	if(cap.width <= 0 || cap.height <= 0) {
		fprintf(stderr, "capture: session reported no buffer size\n");
		cap.failed = 1;
		return;
	}
	if(create_shm_buffer() != 0) {
		fprintf(stderr, "capture: cannot allocate a shm buffer\n");
		cap.failed = 1;
		return;
	}
	if(cap.xdata)
		xhandler_resize(cap.xdata, (unsigned int) cap.width, (unsigned int) cap.height, 0);

	cap.session_ready = 1;
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *session) {
	(void) data; (void) session;
	cap.stopped = 1;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	.buffer_size = session_buffer_size,
	.shm_format = session_shm_format,
	.dmabuf_device = session_dmabuf_device,
	.dmabuf_format = session_dmabuf_format,
	.done = session_done,
	.stopped = session_stopped,
};

/* ------------------------------------------------------------------ */
/* registry                                                           */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version) {
	struct capture_output *item;

	(void) data;
	if(!strcmp(interface, wl_shm_interface.name)) {
		cap.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if(!strcmp(interface, wl_output_interface.name)) {
		item = calloc(1, sizeof(*item));
		if(!item)
			return;
		item->output = wl_registry_bind(registry, name, &wl_output_interface,
						version < 4 ? version : 4);
		item->name = name;
		item->next = cap.outputs;
		cap.outputs = item;
		wl_output_add_listener(item->output, &output_listener, item);
	} else if(!strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
		cap.output_source_mgr = wl_registry_bind(registry, name,
					&ext_output_image_capture_source_manager_v1_interface, 1);
	} else if(!strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name)) {
		cap.toplevel_source_mgr = wl_registry_bind(registry, name,
					&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	} else if(!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
		cap.capture_mgr = wl_registry_bind(registry, name,
						&ext_image_copy_capture_manager_v1_interface, 1);
	} else if(!strcmp(interface, ext_foreign_toplevel_list_v1_interface.name)) {
		cap.toplevel_list = wl_registry_bind(registry, name,
						&ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(cap.toplevel_list,
							  &toplevel_list_listener, NULL);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
	(void) data; (void) registry; (void) name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------ */
/* public API                                                         */

int wlcap_init(void) {
	cap.display = wl_display_connect(NULL);
	if(!cap.display) {
		fprintf(stderr, "capture: cannot connect to the compositor\n");
		return -1;
	}
	cap.registry = wl_display_get_registry(cap.display);
	if(!cap.registry) {
		wlcap_destroy();
		return -1;
	}
	wl_registry_add_listener(cap.registry, &registry_listener, NULL);
	wl_display_roundtrip(cap.display);
	wl_display_roundtrip(cap.display);

	if(!cap.shm || !cap.capture_mgr || !cap.output_source_mgr || !cap.toplevel_source_mgr) {
		fprintf(stderr, "capture: the compositor does not support direct capture\n");
		wlcap_destroy();
		return -1;
	}
	return 0;
}

int wlcap_fd(void) {
	return cap.display ? wl_display_get_fd(cap.display) : -1;
}

int wlcap_flush(void) {
	return cap.display ? wl_display_flush(cap.display) : -1;
}

int wlcap_dispatch(void) {
	struct pollfd pfd;

	if(!cap.display)
		return 0;
	while(wl_display_prepare_read(cap.display) != 0) {
		if(wl_display_dispatch_pending(cap.display) < 0)
			return -1;
	}
	wl_display_flush(cap.display);
	pfd.fd = wl_display_get_fd(cap.display);
	pfd.events = POLLIN;
	pfd.revents = 0;
	if(poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
		if(wl_display_read_events(cap.display) < 0)
			return -1;
	} else {
		wl_display_cancel_read(cap.display);
	}
	if(wl_display_dispatch_pending(cap.display) < 0)
		return -1;
	return 0;
}

static int output_label(char *buf, size_t size, const struct capture_output *item) {
	const char *name = item->name_text[0] ? item->name_text : "output";

	if(item->width > 0 && item->height > 0)
		return snprintf(buf, size, "%s (%dx%d)", name, item->width, item->height) < (int) size ? 0 : -1;
	return snprintf(buf, size, "%s", name) < (int) size ? 0 : -1;
}

int wlcap_list(enum source_kind kind, struct capture_source **list) {
	struct capture_source *items = NULL;
	int count = 0;
	struct capture_output *output;
	struct capture_toplevel *toplevel;

	*list = NULL;
	if(!cap.display)
		return 0;

	/* Refresh the toplevel/output state. */
	if(wl_display_roundtrip(cap.display) < 0)
		return 0;

	if(kind == SOURCE_KIND_SCREEN || kind == SOURCE_KIND_ANY) {
		for(output = cap.outputs; output; output = output->next) {
			items = realloc(items, sizeof(*items) * (count + 1));
			if(!items)
				return 0;
			memset(&items[count], 0, sizeof(items[count]));
			items[count].kind = SOURCE_KIND_SCREEN;
			copy_text(items[count].id, sizeof(items[count].id), output->name_text);
			output_label(items[count].label, sizeof(items[count].label), output);
			count++;
		}
	}

	if(kind == SOURCE_KIND_WINDOW || kind == SOURCE_KIND_ANY) {
		for(toplevel = cap.toplevels; toplevel; toplevel = toplevel->next) {
			char id[192];
			char label[192];
			int i, duplicates = 0;

			if(toplevel->closed)
				continue;
			if(!strcmp(toplevel->app_id, "wayland-share-bridge"))
				continue; /* skip our own mirror window */
			/* The foreign-toplevel identifier is unique for every window,
			 * which keeps two windows with the same title apart. */
			if(toplevel->identifier[0])
				snprintf(id, sizeof(id), "%s", toplevel->identifier);
			else
				snprintf(id, sizeof(id), "%s|%s", toplevel->app_id, toplevel->title);
			if(toplevel->app_id[0])
				snprintf(label, sizeof(label), "%s — %s",
					 toplevel->app_id, toplevel->title);
			else
				snprintf(label, sizeof(label), "%s", toplevel->title);
			for(i = 0; i < count; i++) {
				size_t len;

				if(items[i].kind != SOURCE_KIND_WINDOW)
					continue;
				len = strlen(label);
				if(!strncmp(items[i].label, label, len) &&
				   (items[i].label[len] == '\0' || items[i].label[len] == ' '))
					duplicates++;
			}
			if(duplicates) {
				char numbered[224];

				snprintf(numbered, sizeof(numbered), "%s (%d)", label, duplicates + 1);
				copy_text(label, sizeof(label), numbered);
			}
			items = realloc(items, sizeof(*items) * (count + 1));
			if(!items)
				return 0;
			memset(&items[count], 0, sizeof(items[count]));
			items[count].kind = SOURCE_KIND_WINDOW;
			copy_text(items[count].id, sizeof(items[count].id), id);
			copy_text(items[count].label, sizeof(items[count].label), label);
			count++;
		}
	}

	*list = items;
	return count;
}

static int match_source(enum source_kind kind, const char *id, struct capture_output **out_output,
			struct capture_toplevel **out_toplevel) {
	struct capture_output *output;
	struct capture_toplevel *toplevel;
	int window_fallback = -1;

	*out_output = NULL;
	*out_toplevel = NULL;

	if(kind == SOURCE_KIND_SCREEN || kind == SOURCE_KIND_ANY) {
		for(output = cap.outputs; output; output = output->next) {
			if(!id || !id[0] || !strcmp(output->name_text, id)) {
				*out_output = output;
				return 0;
			}
		}
	}

	if(kind == SOURCE_KIND_WINDOW || kind == SOURCE_KIND_ANY) {
		for(toplevel = cap.toplevels; toplevel; toplevel = toplevel->next) {
			char window_id[192];

			if(toplevel->closed)
				continue;
			if(!strcmp(toplevel->app_id, "wayland-share-bridge"))
				continue;
			if(window_fallback < 0)
				window_fallback = 0;
			snprintf(window_id, sizeof(window_id), "%s|%s",
				 toplevel->app_id, toplevel->title);
			if(id && id[0] &&
			   (!strcmp(toplevel->identifier, id) || !strcmp(window_id, id) ||
			    !strcmp(toplevel->title, id))) {
				*out_toplevel = toplevel;
				return 0;
			}
		}
		if(window_fallback >= 0) {
			for(toplevel = cap.toplevels; toplevel; toplevel = toplevel->next) {
				if(!toplevel->closed && strcmp(toplevel->app_id, "wayland-share-bridge")) {
					*out_toplevel = toplevel;
					return 0;
				}
			}
		}
	}

	return -1;
}

static void destroy_capture(void) {
	if(cap.frame) {
		ext_image_copy_capture_frame_v1_destroy(cap.frame);
		cap.frame = NULL;
	}
	cap.frame_pending = 0;
	if(cap.session) {
		ext_image_copy_capture_session_v1_destroy(cap.session);
		cap.session = NULL;
	}
	if(cap.source) {
		ext_image_capture_source_v1_destroy(cap.source);
		cap.source = NULL;
	}
	if(cap.buffer) {
		wl_buffer_destroy(cap.buffer);
		cap.buffer = NULL;
	}
	if(cap.data) {
		munmap(cap.data, cap.data_size);
		cap.data = NULL;
	}
	cap.data_size = 0;
	cap.width = 0;
	cap.height = 0;
	cap.session_ready = 0;
	cap.stopped = 0;
	cap.failed = 0;
	if(cap.display)
		wl_display_flush(cap.display);
}

int wlcap_start(enum source_kind kind, const char *id, struct xdata *xdata) {
	struct capture_output *output = NULL;
	struct capture_toplevel *toplevel = NULL;
	uint32_t options = 0;

	if(!cap.display)
		return -1;

	/* Resolve the source before tearing down the current capture, so a
	 * stale menu entry does not blank the window that is being shared. */
	if(match_source(kind, id, &output, &toplevel) != 0) {
		fprintf(stderr, "capture: no matching source\n");
		return -1;
	}
	wlcap_stop();

	if(output) {
		cap.source = ext_output_image_capture_source_manager_v1_create_source(
			cap.output_source_mgr, output->output);
		cap.current_kind = SOURCE_KIND_SCREEN;
		output_label(cap.current_label, sizeof(cap.current_label), output);
		options = EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS;
	} else {
		cap.source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
			cap.toplevel_source_mgr, toplevel->handle);
		cap.current_kind = SOURCE_KIND_WINDOW;
		snprintf(cap.current_label, sizeof(cap.current_label), "%s — %s",
			 toplevel->app_id, toplevel->title);
	}
	if(!cap.source) {
		fprintf(stderr, "capture: cannot create the capture source\n");
		return -1;
	}

	cap.xdata = xdata;
	cap.format = WL_SHM_FORMAT_XRGB8888;
	cap.last_frame_ms = now_ms();
	cap.session = ext_image_copy_capture_manager_v1_create_session(cap.capture_mgr,
								       cap.source, options);
	if(!cap.session) {
		fprintf(stderr, "capture: cannot create a capture session\n");
		destroy_capture();
		return -1;
	}
	ext_image_copy_capture_session_v1_add_listener(cap.session, &session_listener, NULL);
	wl_display_flush(cap.display);

	/* Wait for the session handshake so the first frame can go out. */
	while(!cap.session_ready && !cap.stopped && !cap.failed) {
		if(wl_display_roundtrip(cap.display) < 0)
			break;
	}
	if(!cap.session_ready) {
		fprintf(stderr, "capture: the session was not accepted\n");
		destroy_capture();
		return -1;
	}
	return 0;
}

void wlcap_stop(void) {
	destroy_capture();
	cap.xdata = NULL;
	cap.current_kind = SOURCE_KIND_ANY;
	cap.current_label[0] = '\0';
}

int wlcap_active(void) {
	return cap.session_ready && !cap.failed && !cap.stopped;
}

const char *wlcap_current_label(void) {
	return cap.current_label;
}

void wlcap_tick(void) {
	if(!cap.display || !cap.session_ready || cap.frame_pending || !cap.buffer)
		return;
	if(now_ms() - cap.last_frame_ms < FRAME_INTERVAL_MS)
		return;

	cap.frame = ext_image_copy_capture_session_v1_create_frame(cap.session);
	if(!cap.frame) {
		cap.failed = 1;
		return;
	}
	ext_image_copy_capture_frame_v1_add_listener(cap.frame, &frame_listener, NULL);
	ext_image_copy_capture_frame_v1_attach_buffer(cap.frame, cap.buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(cap.frame, 0, 0, cap.width, cap.height);
	ext_image_copy_capture_frame_v1_capture(cap.frame);
	cap.frame_pending = 1;
	wl_display_flush(cap.display);
}

void wlcap_destroy(void) {
	struct capture_output *output, *next_output;
	struct capture_toplevel *toplevel, *next_toplevel;

	destroy_capture();
	if(cap.toplevel_list) {
		ext_foreign_toplevel_list_v1_destroy(cap.toplevel_list);
		cap.toplevel_list = NULL;
	}
	if(cap.capture_mgr) {
		ext_image_copy_capture_manager_v1_destroy(cap.capture_mgr);
		cap.capture_mgr = NULL;
	}
	if(cap.toplevel_source_mgr) {
		ext_foreign_toplevel_image_capture_source_manager_v1_destroy(cap.toplevel_source_mgr);
		cap.toplevel_source_mgr = NULL;
	}
	if(cap.output_source_mgr) {
		ext_output_image_capture_source_manager_v1_destroy(cap.output_source_mgr);
		cap.output_source_mgr = NULL;
	}
	for(output = cap.outputs; output; output = next_output) {
		next_output = output->next;
		if(output->output)
			wl_output_destroy(output->output);
		free(output);
	}
	cap.outputs = NULL;
	for(toplevel = cap.toplevels; toplevel; toplevel = next_toplevel) {
		next_toplevel = toplevel->next;
		if(toplevel->handle)
			ext_foreign_toplevel_handle_v1_destroy(toplevel->handle);
		free(toplevel);
	}
	cap.toplevels = NULL;
	if(cap.shm) {
		wl_shm_destroy(cap.shm);
		cap.shm = NULL;
	}
	if(cap.registry) {
		wl_registry_destroy(cap.registry);
		cap.registry = NULL;
	}
	if(cap.display) {
		wl_display_disconnect(cap.display);
		cap.display = NULL;
	}
}

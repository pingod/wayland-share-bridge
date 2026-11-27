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
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "util.h"
#include "wayland_capture.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

/* Default frame interval (30 fps) until --fps / the tray picks another. */
#define FRAME_INTERVAL_MS 33
/* How long a frame may stay pending before it is considered stuck. */
#define FRAME_TIMEOUT_MS 2000

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
	int buffer_w;
	int buffer_h;
	uint32_t format;
	int format_seen;
	int format_rank;
	int swap_rb;
	int session_ready;
	int frame_pending;
	int stopped;
	int failed;
	uint32_t last_frame_ms;
	uint32_t frame_request_ms;
	uint32_t frame_interval_ms;
	int stalled;
	/*
	 * A frame failed because the source was resized.  The compositor
	 * follows that with fresh buffer constraints; until they arrive the
	 * old buffer must not be reused.
	 */
	int realloc_pending;
	uint32_t constraints_wait_ms;
	/* The next frame fills a freshly allocated buffer and must be shown
	 * in full, whatever damage the compositor reports. */
	int need_full_present;
	int failures;

	struct source_request request;
	int crop_x;
	int crop_y;
	unsigned int crop_w;
	unsigned int crop_h;
	int frame_damage;
	int damage_seen;
	int damage_x;
	int damage_y;
	int damage_w;
	int damage_h;
	int transform;

	struct xdata *xdata;
	enum source_kind current_kind;
	char current_label[320];
	char own_title[192];
} cap = {
	.format = WL_SHM_FORMAT_XRGB8888,
	.frame_interval_ms = FRAME_INTERVAL_MS,
};

static uint32_t now_ms(void) {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t) (ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

/* The tray queries the screen and the window list back to back; reuse one
 * compositor roundtrip instead of doing two. */
#define LIST_TTL_MS 50
static int list_refreshed;
static uint32_t last_list_ms;

static void copy_text(char *dst, size_t size, const char *src) {
	utf8_copy(dst, size, src);
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

/* The bridge's own mirror window is an XWayland toplevel without an app_id,
 * so match it by title as well; capturing it would mirror the mirror. */
static int is_own_toplevel(const struct capture_toplevel *item) {
	if(!strcmp(item->app_id, "wayland-share-bridge"))
		return 1;
	return cap.own_title[0] && !strcmp(item->title, cap.own_title);
}

static void toplevel_remove(struct capture_toplevel *item) {
	struct capture_toplevel **link = &cap.toplevels;

	while(*link && *link != item)
		link = &(*link)->next;
	if(*link)
		*link = item->next;

	if(item->handle)
		ext_foreign_toplevel_handle_v1_destroy(item->handle);
	free(item);
}

static void toplevel_handle_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
	(void) handle;
	toplevel_remove(data);
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

/*
 * X11 stores pixels as BGRx.  XRGB8888/ARGB8888 already match that layout,
 * XBGR8888/ABGR8888 need the red and blue channels swapped.  The first
 * format is not necessarily the best one, so every event is ranked.
 */
static int format_rank(uint32_t format) {
	switch(format) {
	case WL_SHM_FORMAT_XRGB8888:
		return 0;
	case WL_SHM_FORMAT_XBGR8888:
		return 1;
	case WL_SHM_FORMAT_ARGB8888:
		return 2;
	case WL_SHM_FORMAT_ABGR8888:
		return 3;
	default:
		return -1;
	}
}

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session,
			       uint32_t format) {
	int rank = format_rank(format);

	(void) data;
	(void) session;
	if(rank < 0)
		return;

	cap.format_seen = 1;
	if(rank < cap.format_rank) {
		cap.format_rank = rank;
		cap.format = format;
		cap.swap_rb = (rank == 1 || rank == 3);
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
	if(!cap.buffer)
		return -1;
	cap.buffer_w = cap.width;
	cap.buffer_h = cap.height;
	return 0;
}

/* Drops the frame, the shm buffer and the mapping in one place. */
static void release_buffer(void) {
	if(cap.frame) {
		ext_image_copy_capture_frame_v1_destroy(cap.frame);
		cap.frame = NULL;
	}
	cap.frame_pending = 0;
	if(cap.buffer) {
		wl_buffer_destroy(cap.buffer);
		cap.buffer = NULL;
	}
	if(cap.data) {
		munmap(cap.data, cap.data_size);
		cap.data = NULL;
	}
	cap.data_size = 0;
	cap.buffer_w = 0;
	cap.buffer_h = 0;
}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame) {
	int rotated = (cap.transform == 1 || cap.transform == 3 ||
		       cap.transform == 5 || cap.transform == 7);
	unsigned int dst_w = rotated ? cap.crop_h : cap.crop_w;
	unsigned int dst_h = rotated ? cap.crop_w : cap.crop_h;

	(void) data;
	if(cap.xdata && cap.data) {
		int force, scaled;

		/* src_* is the size asked for, which is not the mirror size once a
		 * quality preset caps it. */
		if(cap.xdata->src_width != dst_w || cap.xdata->src_height != dst_h)
			xhandler_resize(cap.xdata, dst_w, dst_h, cap.swap_rb);

		force = cap.need_full_present || xhandler_repaint_pending(cap.xdata);
		scaled = xhandler_scaled(cap.xdata);

		/*
		 * Without damage the compositor copies the previous content again;
		 * pushing it to the X server would be pure work.  A freshly
		 * allocated buffer is different: the part the compositor does not
		 * damage was never shown, and after a resize that is most of the
		 * window, so the first frame into it is always presented in full.
		 * A scaled mirror cannot map damage rectangles either.
		 */
		if(force || cap.frame_damage || !cap.damage_seen) {
			if(xhandler_upload(cap.xdata, cap.data, (uint32_t) cap.data_size,
					   cap.width * 4, cap.crop_x, cap.crop_y,
					   cap.crop_w, cap.crop_h, cap.transform) == 0) {
				if(cap.transform == 0 && !force &&
				   !scaled && cap.damage_seen && cap.frame_damage) {
					xhandler_present(cap.xdata,
							 cap.damage_x - cap.crop_x,
							 cap.damage_y - cap.crop_y,
							 (unsigned int) cap.damage_w,
							 (unsigned int) cap.damage_h);
				} else {
					xhandler_present(cap.xdata, 0, 0,
							 cap.xdata->width, cap.xdata->height);
				}
				cap.need_full_present = 0;
			}
		}
		if(cap.frame_damage)
			cap.damage_seen = 1;
	}
	cap.frame_pending = 0;
	cap.stalled = 0;
	cap.failures = 0;
	cap.last_frame_ms = now_ms();
	ext_image_copy_capture_frame_v1_destroy(frame);
	cap.frame = NULL;
}

/*
 * Not every failure ends the stream.  A window that is resized while a frame
 * is in flight makes the compositor fail that frame with buffer_constraints,
 * followed by fresh constraints (buffer_size + done) for the new size; the
 * session stays alive and the next frame must simply use a new buffer.
 * Latching cap.failed here stopped the whole capture, left the sharing app
 * with a black picture and made the bridge hide its mirror window.
 */
static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame,
			 uint32_t reason) {
	(void) data;
	cap.frame_pending = 0;
	ext_image_copy_capture_frame_v1_destroy(frame);
	cap.frame = NULL;

	switch(reason) {
	case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
		fprintf(stderr, "capture: the source was resized; waiting for new buffers\n");
		cap.realloc_pending = 1;
		cap.constraints_wait_ms = now_ms();
		break;
	case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED:
		cap.stopped = 1;
		break;
	default:
		/* "unknown" is retriable; retry, but do not spin forever on a
		 * source that never completes a frame. */
		if(++cap.failures >= 60) {
			fprintf(stderr, "capture: frame failed (reason %u)\n", reason);
			cap.failed = 1;
		}
		break;
	}
}

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame,
			    uint32_t transform) {
	(void) data; (void) frame;
	cap.transform = (int) transform;
}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame,
			 int32_t x, int32_t y, int32_t width, int32_t height) {
	int x0 = x, y0 = y, x1 = x + width, y1 = y + height;
	int nx0, ny0, nx1, ny1;

	(void) data; (void) frame;
	if(width <= 0 || height <= 0)
		return;

	if(x0 < 0)
		x0 = 0;
	if(y0 < 0)
		y0 = 0;
	if(cap.width > 0 && x1 > cap.width)
		x1 = cap.width;
	if(cap.height > 0 && y1 > cap.height)
		y1 = cap.height;
	if(x1 <= x0 || y1 <= y0)
		return;

	if(cap.frame_damage) {
		nx0 = x0 < cap.damage_x ? x0 : cap.damage_x;
		ny0 = y0 < cap.damage_y ? y0 : cap.damage_y;
		nx1 = x1 > cap.damage_x + cap.damage_w ? x1 : cap.damage_x + cap.damage_w;
		ny1 = y1 > cap.damage_y + cap.damage_h ? y1 : cap.damage_y + cap.damage_h;
	} else {
		nx0 = x0;
		ny0 = y0;
		nx1 = x1;
		ny1 = y1;
	}

	cap.damage_x = nx0;
	cap.damage_y = ny0;
	cap.damage_w = nx1 - nx0;
	cap.damage_h = ny1 - ny0;
	cap.frame_damage = 1;
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
	unsigned int cx = 0, cy = 0, cw, ch;

	(void) data; (void) session;
	if(cap.width <= 0 || cap.height <= 0) {
		fprintf(stderr, "capture: session reported no buffer size\n");
		cap.failed = 1;
		return;
	}
	if(!cap.format_seen) {
		fprintf(stderr, "capture: the compositor offered no supported shm format\n");
		cap.failed = 1;
		return;
	}

	/* A second done (resize, mode change) keeps the buffer when nothing
	 * about the geometry changed.  A frame that failed on stale buffer
	 * constraints still needs a fresh buffer, even at the same size. */
	if(cap.buffer && !cap.realloc_pending &&
	   cap.width == cap.buffer_w && cap.height == cap.buffer_h)
		return;

	release_buffer();

	cw = (unsigned int) cap.width;
	ch = (unsigned int) cap.height;
	if(cap.request.cropped) {
		if(cap.request.x >= cap.width || cap.request.y >= cap.height) {
			fprintf(stderr, "capture: the region is outside the screen\n");
			cap.failed = 1;
			return;
		}
		cx = (unsigned int) cap.request.x;
		cy = (unsigned int) cap.request.y;
		cw = cap.request.width;
		if(cw > (unsigned int) cap.width - cx)
			cw = (unsigned int) cap.width - cx;
		ch = cap.request.height;
		if(ch > (unsigned int) cap.height - cy)
			ch = (unsigned int) cap.height - cy;
		if(cw == 0 || ch == 0) {
			fprintf(stderr, "capture: empty region\n");
			cap.failed = 1;
			return;
		}
	}

	if(create_shm_buffer() != 0) {
		fprintf(stderr, "capture: cannot allocate a shm buffer\n");
		cap.failed = 1;
		return;
	}

	cap.crop_x = (int) cx;
	cap.crop_y = (int) cy;
	cap.crop_w = cw;
	cap.crop_h = ch;
	cap.realloc_pending = 0;
	cap.constraints_wait_ms = 0;
	cap.need_full_present = 1;

	if(cap.xdata)
		xhandler_resize(cap.xdata, cw, ch, cap.swap_rb);

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
	struct capture_output **link = &cap.outputs;

	(void) data; (void) registry;
	while(*link && (*link)->name != name)
		link = &(*link)->next;
	if(!*link)
		return;

	{
		struct capture_output *item = *link;

		*link = item->next;
		if(item->output)
			wl_output_destroy(item->output);
		free(item);
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------ */
/* public API                                                         */

int wlcap_init(const char *own_title) {
	if(own_title)
		snprintf(cap.own_title, sizeof(cap.own_title), "%s", own_title);
	else
		cap.own_title[0] = '\0';

	cap.display = wl_display_connect(NULL);
	if(!cap.display) {
		fprintf(stderr, "capture: cannot connect to the compositor\n");
		return -1;
	}
	list_refreshed = 0;
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

int wlcap_dispatch(void) {
	struct pollfd pfd;

	if(!cap.display)
		return 0;
	while(wl_display_prepare_read(cap.display) != 0) {
		if(wl_display_dispatch_pending(cap.display) < 0)
			return -1;
	}
	if(wl_display_flush(cap.display) < 0 && errno != EAGAIN)
		return -1;
	pfd.fd = wl_display_get_fd(cap.display);
	pfd.events = POLLIN;
	pfd.revents = 0;
	{
		int ret = poll(&pfd, 1, 0);

		if(ret > 0 && (pfd.revents & POLLIN)) {
			if(wl_display_read_events(cap.display) < 0)
				return -1;
		} else {
			wl_display_cancel_read(cap.display);
			/* HUP/ERR without data: the compositor went away. */
			if(ret > 0 && (pfd.revents & (POLLHUP | POLLERR)))
				return -1;
		}
	}
	if(wl_display_dispatch_pending(cap.display) < 0)
		return -1;
	return 0;
}

/*
 * wl_output v4 and later report a connector name; older compositors do not,
 * so fall back to a stable position based id instead of an empty string that
 * makes every output indistinguishable.
 */
static void output_id(const struct capture_output *item, char *buf, size_t size) {
	const struct capture_output *it;
	int index = 0;

	if(item->name_text[0]) {
		snprintf(buf, size, "%s", item->name_text);
		return;
	}
	for(it = cap.outputs; it && it != item; it = it->next)
		index++;
	snprintf(buf, size, "output%d", index);
}

static int output_label(char *buf, size_t size, const struct capture_output *item) {
	char name[96];

	output_id(item, name, sizeof(name));
	if(item->width > 0 && item->height > 0)
		return snprintf(buf, size, "%s (%dx%d)", name, item->width, item->height) < (int) size ? 0 : -1;
	return snprintf(buf, size, "%s", name) < (int) size ? 0 : -1;
}

static int compare_sources(const void *left, const void *right) {
	const struct capture_source *a = left;
	const struct capture_source *b = right;
	int result;

	if(a->kind != b->kind)
		return a->kind < b->kind ? -1 : 1;
	result = strcasecmp(a->kind == SOURCE_KIND_WINDOW && a->group[0] ? a->group : "Other",
			    b->kind == SOURCE_KIND_WINDOW && b->group[0] ? b->group : "Other");
	if(result)
		return result;
	result = strcasecmp(a->title, b->title);
	if(result)
		return result;
	return strcasecmp(a->id, b->id);
}

int wlcap_list(enum source_kind kind, struct capture_source **list) {
	struct capture_source *items = NULL;
	int count = 0;
	struct capture_output *output;
	struct capture_toplevel *toplevel;

	*list = NULL;
	if(!cap.display)
		return 0;

	/* Refresh the toplevel/output state.  The tray asks for the screen and
	 * the window list back to back, so a very recent roundtrip is reused. */
	if(!list_refreshed || (int32_t) (now_ms() - last_list_ms) >= LIST_TTL_MS) {
		if(wl_display_roundtrip(cap.display) < 0)
			return 0;
		list_refreshed = 1;
		last_list_ms = now_ms();
	}

	if(kind == SOURCE_KIND_SCREEN || kind == SOURCE_KIND_ANY) {
		for(output = cap.outputs; output; output = output->next) {
			char id[96];
			char label[192];
			struct capture_source *grown = realloc(items, sizeof(*items) * (count + 1));

			if(!grown) {
				free(items);
				return 0;
			}
			items = grown;
			memset(&items[count], 0, sizeof(items[count]));
			items[count].kind = SOURCE_KIND_SCREEN;
			output_id(output, id, sizeof(id));
			copy_text(items[count].id, sizeof(items[count].id), id);
			output_label(label, sizeof(label), output);
			copy_text(items[count].label, sizeof(items[count].label), label);
			copy_text(items[count].title, sizeof(items[count].title), label);
			count++;
		}
	}

	if(kind == SOURCE_KIND_WINDOW || kind == SOURCE_KIND_ANY) {
		for(toplevel = cap.toplevels; toplevel; toplevel = toplevel->next) {
			char id[192];
			char label[192];
			char raw[420];
			int i, duplicates = 0;

			if(is_own_toplevel(toplevel))
				continue; /* skip our own mirror window */
			/* The foreign-toplevel identifier is unique for every window,
			 * which keeps two windows with the same title apart. */
			if(toplevel->identifier[0]) {
				copy_text(id, sizeof(id), toplevel->identifier);
			} else {
				snprintf(raw, sizeof(raw), "%s|%s",
					 toplevel->app_id, toplevel->title);
				copy_text(id, sizeof(id), raw);
			}
			if(toplevel->app_id[0]) {
				snprintf(raw, sizeof(raw), "%s — %s",
					 toplevel->app_id, toplevel->title);
				copy_text(label, sizeof(label), raw);
			} else {
				copy_text(label, sizeof(label), toplevel->title);
			}
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
			{
				struct capture_source *grown = realloc(items, sizeof(*items) * (count + 1));

				if(!grown) {
					free(items);
					return 0;
				}
				items = grown;
			}
			memset(&items[count], 0, sizeof(items[count]));
			items[count].kind = SOURCE_KIND_WINDOW;
			copy_text(items[count].id, sizeof(items[count].id), id);
			copy_text(items[count].label, sizeof(items[count].label), label);
			copy_text(items[count].group, sizeof(items[count].group),
				  toplevel->app_id);
			copy_text(items[count].title, sizeof(items[count].title),
				  toplevel->title);
			count++;
		}
	}

	/* A stable order keeps the grouped menu from jumping around. */
	if(count > 1)
		qsort(items, (size_t) count, sizeof(*items), compare_sources);

	*list = items;
	return count;
}

static int match_source(const struct source_request *req,
			struct capture_output **out_output,
			struct capture_toplevel **out_toplevel) {
	struct capture_output *output;
	struct capture_toplevel *toplevel;

	*out_output = NULL;
	*out_toplevel = NULL;

	if(req->kind == SOURCE_KIND_SCREEN || req->kind == SOURCE_KIND_ANY) {
		for(output = cap.outputs; output; output = output->next) {
			char id[96];

			output_id(output, id, sizeof(id));
			if(!req->id[0] || !strcmp(id, req->id)) {
				*out_output = output;
				return 0;
			}
		}
	}

	if(req->kind == SOURCE_KIND_WINDOW || req->kind == SOURCE_KIND_ANY) {
		for(toplevel = cap.toplevels; toplevel; toplevel = toplevel->next) {
			char window_id[192];

			if(is_own_toplevel(toplevel))
				continue;
			if(!req->id[0]) {
				*out_toplevel = toplevel;
				return 0;
			}
			char raw[420];

			snprintf(raw, sizeof(raw), "%s|%s",
				 toplevel->app_id, toplevel->title);
			copy_text(window_id, sizeof(window_id), raw);
			if(!strcmp(toplevel->identifier, req->id) ||
			   !strcmp(window_id, req->id) ||
			   !strcmp(toplevel->title, req->id) ||
			   !strcmp(toplevel->app_id, req->id)) {
				*out_toplevel = toplevel;
				return 0;
			}
		}
	}

	return -1;
}

static void destroy_capture(void) {
	release_buffer();
	if(cap.session) {
		ext_image_copy_capture_session_v1_destroy(cap.session);
		cap.session = NULL;
	}
	if(cap.source) {
		ext_image_capture_source_v1_destroy(cap.source);
		cap.source = NULL;
	}
	cap.width = 0;
	cap.height = 0;
	cap.session_ready = 0;
	cap.stopped = 0;
	cap.failed = 0;
	cap.crop_x = 0;
	cap.crop_y = 0;
	cap.crop_w = 0;
	cap.crop_h = 0;
	cap.frame_damage = 0;
	cap.damage_seen = 0;
	cap.damage_x = 0;
	cap.damage_y = 0;
	cap.damage_w = 0;
	cap.damage_h = 0;
	cap.transform = 0;
	cap.format_seen = 0;
	cap.format_rank = 99;
	cap.swap_rb = 0;
	cap.stalled = 0;
	cap.frame_request_ms = 0;
	cap.realloc_pending = 0;
	cap.constraints_wait_ms = 0;
	cap.need_full_present = 0;
	cap.failures = 0;
	if(cap.display)
		wl_display_flush(cap.display);
}

int wlcap_start(const struct source_request *req, struct xdata *xdata) {
	struct capture_output *output = NULL;
	struct capture_toplevel *toplevel = NULL;
	uint32_t options = 0;
	uint32_t deadline;

	if(!cap.display || !req)
		return -1;

	/* Resolve the source before tearing down the current capture, so a
	 * stale menu entry does not blank the window that is being shared. */
	if(match_source(req, &output, &toplevel) != 0) {
		fprintf(stderr, "capture: no matching source\n");
		return -1;
	}
	wlcap_stop();
	cap.request = *req;

	if(output) {
		cap.source = ext_output_image_capture_source_manager_v1_create_source(
			cap.output_source_mgr, output->output);
		cap.current_kind = SOURCE_KIND_SCREEN;
		if(req->cropped) {
			char label[192];

			output_label(label, sizeof(label), output);
			snprintf(cap.current_label, sizeof(cap.current_label),
				 "%s region %d,%d %ux%u", label,
				 req->x, req->y, req->width, req->height);
		} else {
			output_label(cap.current_label, sizeof(cap.current_label), output);
		}
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
	cap.format_seen = 0;
	cap.format_rank = 99;
	cap.swap_rb = 0;
	cap.transform = 0;
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

	/* Wait for the session handshake with a deadline: a compositor that
	 * never answers must not freeze the UI. */
	deadline = now_ms() + 1500;
	while(!cap.session_ready && !cap.stopped && !cap.failed) {
		struct pollfd pfd;
		int ret;

		if((int32_t) (now_ms() - deadline) >= 0) {
			fprintf(stderr, "capture: the session was not accepted\n");
			destroy_capture();
			return -1;
		}

		pfd.fd = wl_display_get_fd(cap.display);
		pfd.events = POLLIN;
		pfd.revents = 0;
		ret = poll(&pfd, 1, 50);
		if(ret < 0) {
			if(errno == EINTR)
				continue;
			fprintf(stderr, "capture: the session was not accepted\n");
			destroy_capture();
			return -1;
		}
		if(ret > 0 && (pfd.revents & POLLIN)) {
			if(wl_display_dispatch(cap.display) < 0) {
				fprintf(stderr, "capture: lost the compositor connection\n");
				destroy_capture();
				return -1;
			}
		}
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

void wlcap_set_fps(int fps) {
	if(fps < 1)
		fps = 1;
	if(fps > 240)
		fps = 240;

	cap.frame_interval_ms = (uint32_t) (1000 / fps);
	if(cap.frame_interval_ms == 0)
		cap.frame_interval_ms = 1;
}

int wlcap_active(void) {
	return cap.session_ready && !cap.failed && !cap.stopped;
}

const char *wlcap_current_label(void) {
	return cap.current_label;
}

int wlcap_stalled(void) {
	return cap.stalled;
}

void wlcap_tick(void) {
	if(!cap.display || !cap.session_ready || !cap.buffer)
		return;

	if(cap.realloc_pending) {
		/*
		 * The compositor normally answers a buffer_constraints failure with
		 * buffer_size + done, and session_done() allocates the new buffer.
		 * If that never arrives, reallocate from the geometry known so far
		 * instead of staying black forever.
		 */
		if((int32_t) (now_ms() - cap.constraints_wait_ms) < FRAME_TIMEOUT_MS)
			return;

		cap.realloc_pending = 0;
		release_buffer();
		if(create_shm_buffer() != 0) {
			fprintf(stderr, "capture: cannot reallocate the shm buffer\n");
			cap.failed = 1;
			return;
		}
		cap.need_full_present = 1;
	}

	if(cap.frame_pending) {
		if((int32_t) (now_ms() - cap.frame_request_ms) < FRAME_TIMEOUT_MS)
			return;

		/* Some compositors (Hyprland among them) never complete a frame
		 * for a window that is off-screen or on a hidden workspace: the
		 * frame is left pending forever and the mirror would stay black.
		 * Drop it and ask again; the caller surfaces the stall. */
		if(cap.frame) {
			ext_image_copy_capture_frame_v1_destroy(cap.frame);
			cap.frame = NULL;
		}
		cap.frame_pending = 0;
		if(!cap.stalled)
			fprintf(stderr, "capture: no frame from the compositor in %d ms; "
				"retrying (is the source off-screen or hidden?)\n",
				FRAME_TIMEOUT_MS);
		cap.stalled = 1;
	}

	if(now_ms() - cap.last_frame_ms < cap.frame_interval_ms)
		return;

	cap.frame_damage = 0;
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
	cap.frame_request_ms = now_ms();
	wl_display_flush(cap.display);
}

void wlcap_destroy(void) {
	struct capture_output *output, *next_output;
	struct capture_toplevel *toplevel, *next_toplevel;

	destroy_capture();
	memset(&cap.request, 0, sizeof(cap.request));
	cap.current_kind = SOURCE_KIND_ANY;
	cap.current_label[0] = '\0';
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

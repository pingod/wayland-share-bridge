/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef X11_HANDLER_H
#define X11_HANDLER_H

#include <stdint.h>

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#include "config.h"

struct xdata {
	int fd;

	unsigned int width;
	unsigned int height;
	int swap_rb;

	/* MIT-SHM is used when the server supports it (no socket copy). */
	int use_shm;
	int shm_attached;
	char *shm_addr;
	XShmSegmentInfo shminfo;

	uint32_t *buffer;

	Display *display;
	Window window;
	XImage *image;
	int visible;
	int mapped;
};

struct xdata *xhandler_init(const struct options *opts);

/* (Re)allocates the mirror image and resizes the window. */
void xhandler_resize(struct xdata *data, unsigned int width, unsigned int height, int swap_rb);

/*
 * Converts a frame region into the mirror image without talking to the X
 * server (so a damage rectangle can be presented separately).
 */
int xhandler_upload(struct xdata *data, const void *pixels, uint32_t size, int32_t stride,
		    int src_x, int src_y, unsigned int width, unsigned int height,
		    int transform);

/* Pushes a rectangle of the current image to the window. */
void xhandler_present(struct xdata *data, int x, int y, unsigned int width, unsigned int height);

/*
 * Convenience wrapper: uploads the rectangle (src_x, src_y, width, height)
 * of a frame into the top left corner of the window and presents all of it.
 */
void xhandler_draw(struct xdata *data, const void *pixels, uint32_t size, int32_t stride,
		   int src_x, int src_y, unsigned int width, unsigned int height,
		   int transform);

/*
 * Maps or unmaps the mirror window.  Unmapping removes it from the desktop
 * entirely, which is what an idle bridge wants; a mapped but transparent
 * window still appears in window lists.
 */
void xhandler_set_mapped(struct xdata *data, int mapped);

/*
 * Shows or hides the mirror window.  A shown window accepts the pointer (so
 * it can be dragged with the compositor's own mouse binding); a hidden one is
 * click-through.
 */
void xhandler_set_visible(struct xdata *data, int visible);

void xhandler_routine(struct xdata *data);

void xhandler_destroy(struct xdata *data);

#endif /* X11_HANDLER_H */

/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * x11_handler.c - the X11 side of the bridge.
 *
 * Creates a plain X11 window that X11 clients (WPS Office, Zoom, Feishu,
 * Electron apps, ...) can see and capture, then paints every received
 * PipeWire frame into it with plain XPutImage.  No toolkit, no GL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

#include "x11_handler.h"

static void set_cardinal(Display *dpy, Window win, const char *name, unsigned long value) {
	Atom atom = XInternAtom(dpy, name, False);

	XChangeProperty(dpy, win, atom, XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *) &value, 1);
}

static void set_window_type(Display *dpy, Window win) {
	Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	Atom normal = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);

	XChangeProperty(dpy, win, type, XA_ATOM, 32, PropModeReplace,
		(unsigned char *) &normal, 1);
}

static void make_input_transparent(Display *dpy, Window win) {
	XserverRegion empty = XFixesCreateRegion(dpy, NULL, 0);

	XFixesSetWindowShapeRegion(dpy, win, ShapeInput, 0, 0, empty);
	XFixesDestroyRegion(dpy, empty);
}

struct xdata *xhandler_init(const struct options *opts) {
	struct xdata *data;
	XSetWindowAttributes attrs;
	XClassHint class_hint;

	data = calloc(1, sizeof(struct xdata));
	if(!data)
		return NULL;

	data->display = XOpenDisplay(NULL);
	if(!data->display) {
		fprintf(stderr, "Cannot open X11 display (is DISPLAY set?)\n");
		free(data);
		return NULL;
	}

	attrs.background_pixel = 0;
	attrs.event_mask = ExposureMask | StructureNotifyMask;

	data->window = XCreateWindow(data->display, DefaultRootWindow(data->display),
		0, 0, 800, 600, 0, CopyFromParent, InputOutput, CopyFromParent,
		CWBackPixel | CWEventMask, &attrs);
	data->fd = ConnectionNumber(data->display);

	XStoreName(data->display, data->window, opts->title);

	class_hint.res_name = (char *) opts->class_name;
	class_hint.res_class = (char *) opts->class_name;
	XSetClassHint(data->display, data->window, &class_hint);

	set_window_type(data->display, data->window);
	set_cardinal(data->display, data->window, "_NET_WM_PID", (unsigned long) getpid());

	/*
	 * Compositors that honour _NET_WM_WINDOW_OPACITY (KDE Plasma, GNOME,
	 * sway, ...) will render the window fully transparent.  Hyprland
	 * ignores the property for XWayland windows, so the launcher script
	 * installs an equivalent window rule there; see data/.
	 */
	/* Hidden by default: empty input region so clicks go through. */
	make_input_transparent(data->display, data->window);

	if(!opts->show)
		set_cardinal(data->display, data->window, "_NET_WM_WINDOW_OPACITY", 0);

	data->visible = opts->show ? 1 : 0;
	XMapWindow(data->display, data->window);
	XFlush(data->display);

	return data;
}

static void set_input_region(struct xdata *data, int interactive) {
	XWindowAttributes attr;
	XserverRegion region;

	if(!XGetWindowAttributes(data->display, data->window, &attr))
		return;

	if(interactive) {
		XRectangle rect;

		rect.x = 0;
		rect.y = 0;
		rect.width = (unsigned short) attr.width;
		rect.height = (unsigned short) attr.height;
		region = XFixesCreateRegion(data->display, &rect, 1);
	} else {
		region = XFixesCreateRegion(data->display, NULL, 0);
	}

	XFixesSetWindowShapeRegion(data->display, data->window, ShapeInput, 0, 0,
				   region);
	XFixesDestroyRegion(data->display, region);
}

void xhandler_set_visible(struct xdata *data, int visible) {
	if(!data || !data->display || !data->window)
		return;

	visible = visible ? 1 : 0;
	set_cardinal(data->display, data->window, "_NET_WM_WINDOW_OPACITY",
		     visible ? 0xffffffffUL : 0);
	set_input_region(data, visible);
	if(visible)
		XRaiseWindow(data->display, data->window);
	data->visible = visible;
	XFlush(data->display);
}

void xhandler_resize(struct xdata *data, unsigned int width, unsigned int height, int swap_rb) {
	XWindowChanges changes;

	if(width == 0 || height == 0 || width > 16384 || height > 16384)
		return;

	if(data->image) {
		XDestroyImage(data->image);	/* also frees data->buffer */
		data->image = NULL;
		data->buffer = NULL;
	}

	data->width = width;
	data->height = height;
	data->swap_rb = swap_rb;

	data->buffer = malloc((size_t) width * height * 4);
	if(!data->buffer) {
		fprintf(stderr, "Out of memory for a %ux%u frame\n", width, height);
		return;
	}

	data->image = XCreateImage(data->display,
		DefaultVisual(data->display, DefaultScreen(data->display)),
		(unsigned int) DefaultDepth(data->display, DefaultScreen(data->display)),
		ZPixmap, 0, (char *) data->buffer, width, height, 32, 0);
	if(!data->image) {
		fprintf(stderr, "XCreateImage failed for a %ux%u frame\n", width, height);
		free(data->buffer);
		data->buffer = NULL;
		return;
	}

	changes.width = (int) width;
	changes.height = (int) height;
	XConfigureWindow(data->display, data->window, CWWidth | CWHeight, &changes);
	XFlush(data->display);
}

void xhandler_draw(struct xdata *data, const void *pixels, uint32_t size, int32_t stride) {
	const unsigned char *src = pixels;
	unsigned char *dst;
	unsigned int row, x;
	size_t row_bytes, rows;

	if(!data->image || !data->buffer || data->width == 0 || data->height == 0)
		return;

	row_bytes = (size_t) data->width * 4;
	if(stride <= 0 || (size_t) stride < row_bytes)
		stride = (int32_t) row_bytes;

	rows = (size_t) size / (size_t) stride;
	if(rows > data->height)
		rows = data->height;

	/* Keep the rest of the last (partial) frame black. */
	memset(data->buffer, 0, row_bytes * data->height);

	for(row = 0; row < rows; row++) {
		const unsigned char *s = src + (size_t) row * (size_t) stride;
		dst = (unsigned char *) data->buffer + (size_t) row * row_bytes;

		if(data->swap_rb) {
			/* Source is RGBA/RGBx, X11 wants BGRx in memory. */
			for(x = 0; x < data->width; x++) {
				dst[x * 4 + 0] = s[x * 4 + 2];
				dst[x * 4 + 1] = s[x * 4 + 1];
				dst[x * 4 + 2] = s[x * 4 + 0];
				dst[x * 4 + 3] = 0xff;
			}
		} else {
			memcpy(dst, s, row_bytes);
		}
	}

	XPutImage(data->display, data->window,
		DefaultGC(data->display, DefaultScreen(data->display)),
		data->image, 0, 0, 0, 0, data->width, data->height);
	XFlush(data->display);
}

uint8_t xhandler_routine(struct xdata *data) {
	while(XPending(data->display)) {
		XEvent ev;

		XNextEvent(data->display, &ev);
	}

	return 0;
}

void xhandler_destroy(struct xdata *data) {
	if(!data)
		return;

	if(data->image) {
		XDestroyImage(data->image);
		data->image = NULL;
		data->buffer = NULL;
	}
	if(data->display) {
		if(data->window)
			XDestroyWindow(data->display, data->window);
		XCloseDisplay(data->display);
	}
	free(data);
}

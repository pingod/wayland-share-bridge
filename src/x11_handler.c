/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * x11_handler.c - the X11 side of the bridge.
 *
 * Creates a plain X11 window that X11 clients (WPS Office, Zoom, Feishu,
 * Electron apps, ...) can see and capture, then paints every received frame
 * into it.  When the server supports MIT-SHM the frame is shared with the
 * server instead of being copied through the X11 socket; otherwise plain
 * XPutImage() is used.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include "image_convert.h"
#include "x11_handler.h"

/*
 * The default Xlib error handler exits the process.  A BadLength (an
 * oversized PutImage) or a BadWindow (the window vanished with the
 * compositor) must not take the whole bridge down, so errors are logged and
 * the loop keeps running.
 */
static int x_error_handler(Display *display, XErrorEvent *event) {
	char text[128];

	XGetErrorText(display, event->error_code, text, sizeof(text));
	fprintf(stderr, "X11 error: %s (request %d.%d)\n", text,
		event->request_code, event->minor_code);
	return 0;
}

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

/*
 * WM_NAME is Latin-1 per ICCCM; window pickers read _NET_WM_NAME.  Both are
 * set so non-ASCII titles show up correctly.
 */
static void set_window_name(Display *dpy, Window win, const char *title) {
	Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	Atom net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
	Atom net_icon = XInternAtom(dpy, "_NET_WM_ICON_NAME", False);
	size_t length = strlen(title);

	XStoreName(dpy, win, title);
	XChangeProperty(dpy, win, net_name, utf8, 8, PropModeReplace,
		(const unsigned char *) title, (int) length);
	XChangeProperty(dpy, win, net_icon, utf8, 8, PropModeReplace,
		(const unsigned char *) title, (int) length);
}

/*
 * The window is never meant to appear in a task list or pager: it is an
 * off-screen paint target that only exists so X11 clients can capture it.
 */
static void set_window_state(Display *dpy, Window win) {
	Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
	Atom atoms[2];

	atoms[0] = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
	atoms[1] = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);

	XChangeProperty(dpy, win, state, XA_ATOM, 32, PropModeReplace,
		(unsigned char *) atoms, 2);
}

/*
 * Hidden: an empty input region, so clicks go through to whatever is below.
 * Shown: a full input region, because a compositor cannot target a window
 * that is not under the pointer - Hyprland's Super+drag hits whatever surface
 * accepts input at the cursor, so a click-through window cannot be dragged.
 */
static void set_input_region(Display *dpy, Window win, int interactive,
			     unsigned int width, unsigned int height) {
	XserverRegion region;

	if(interactive) {
		XRectangle rect;

		/*
		 * Use the size the caller knows about, not XGetWindowAttributes():
		 * a resize issues XConfigureWindow and the server has not applied
		 * it yet, so the attributes still report the previous size and the
		 * input region would stay too small to grab the window.
		 */
		if(width == 0 || height == 0) {
			XWindowAttributes attr;

			if(!XGetWindowAttributes(dpy, win, &attr))
				return;
			width = (unsigned int) attr.width;
			height = (unsigned int) attr.height;
		}
		rect.x = 0;
		rect.y = 0;
		rect.width = (unsigned short) width;
		rect.height = (unsigned short) height;
		region = XFixesCreateRegion(dpy, &rect, 1);
	} else {
		region = XFixesCreateRegion(dpy, NULL, 0);
	}

	XFixesSetWindowShapeRegion(dpy, win, ShapeInput, 0, 0, region);
	XFixesDestroyRegion(dpy, region);
}

static void shm_release(struct xdata *data) {
	if(data->shm_attached) {
		XSync(data->display, False);
		XShmDetach(data->display, &data->shminfo);
		data->shm_attached = 0;
	}
	if(data->shm_addr) {
		shmdt(data->shm_addr);
		data->shm_addr = NULL;
	}
	if(data->shminfo.shmid >= 0) {
		shmctl(data->shminfo.shmid, IPC_RMID, NULL);
		data->shminfo.shmid = -1;
	}
}

static void image_free(struct xdata *data) {
	if(!data->image)
		return;

	if(data->shm_addr) {
		/* The pixels belong to the segment, not to Xlib. */
		data->image->data = NULL;
		XDestroyImage(data->image);
	} else {
		XDestroyImage(data->image);	/* also frees data->buffer */
	}
	data->image = NULL;
	data->buffer = NULL;
	shm_release(data);
}

/* XShmAttach() reports server side failures asynchronously. */
static int shm_attach_failed;

static int shm_error_handler(Display *display, XErrorEvent *event) {
	(void) display;
	(void) event;
	shm_attach_failed = 1;
	return 0;
}

static int shm_image_alloc(struct xdata *data, unsigned int width, unsigned int height,
			   size_t bytes) {
	Visual *visual = DefaultVisual(data->display, DefaultScreen(data->display));
	int depth = DefaultDepth(data->display, DefaultScreen(data->display));
	int (*old_handler)(Display *, XErrorEvent *);

	data->shminfo.shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
	if(data->shminfo.shmid < 0)
		return -1;

	data->shminfo.shmaddr = shmat(data->shminfo.shmid, NULL, 0);
	if(data->shminfo.shmaddr == (char *) -1) {
		data->shminfo.shmaddr = NULL;
		return -1;
	}
	data->shminfo.readOnly = False;

	shm_attach_failed = 0;
	old_handler = XSetErrorHandler(shm_error_handler);
	XShmAttach(data->display, &data->shminfo);
	XSync(data->display, False);
	XSetErrorHandler(old_handler);
	if(shm_attach_failed)
		return -1;

	data->shm_attached = 1;
	data->shm_addr = data->shminfo.shmaddr;
	data->image = XShmCreateImage(data->display, visual, depth, ZPixmap,
				      data->shminfo.shmaddr, &data->shminfo,
				      width, height);
	if(!data->image || !data->image->data)
		return -1;

	data->buffer = (uint32_t *) data->image->data;
	return 0;
}

static int image_alloc(struct xdata *data, unsigned int width, unsigned int height) {
	size_t bytes = (size_t) width * height * 4;
	Visual *visual = DefaultVisual(data->display, DefaultScreen(data->display));
	int depth = DefaultDepth(data->display, DefaultScreen(data->display));

	if(bytes == 0)
		return -1;

	if(data->use_shm && shm_image_alloc(data, width, height, bytes) == 0)
		return 0;

	if(data->image) {
		data->image->data = NULL;
		XDestroyImage(data->image);
		data->image = NULL;
	}
	data->buffer = NULL;
	shm_release(data);
	/* One failure is enough; do not retry SHM for every resize. */
	data->use_shm = 0;

	data->buffer = malloc(bytes);
	if(!data->buffer)
		return -1;

	data->image = XCreateImage(data->display, visual, depth, ZPixmap, 0,
				   (char *) data->buffer, width, height, 32, 0);
	if(!data->image) {
		free(data->buffer);
		data->buffer = NULL;
		return -1;
	}

	return 0;
}

struct xdata *xhandler_init(const struct options *opts) {
	struct xdata *data;
	XSetWindowAttributes attrs;
	XClassHint class_hint;

	/* The PipeWire thread paints through the same Display as the main
	 * loop, so Xlib must serialise access to it. */
	if(!XInitThreads()) {
		fprintf(stderr, "Cannot initialise Xlib threading support\n");
		return NULL;
	}

	XSetErrorHandler(x_error_handler);

	data = calloc(1, sizeof(struct xdata));
	if(!data)
		return NULL;
	data->shminfo.shmid = -1;

	data->display = XOpenDisplay(NULL);
	if(!data->display) {
		fprintf(stderr, "Cannot open X11 display (is DISPLAY set?)\n");
		free(data);
		return NULL;
	}

	data->use_shm = XShmQueryExtension(data->display) ? 1 : 0;

	attrs.background_pixel = 0;
	attrs.event_mask = ExposureMask | StructureNotifyMask;

	data->window = XCreateWindow(data->display, DefaultRootWindow(data->display),
		0, 0, 800, 600, 0, CopyFromParent, InputOutput, CopyFromParent,
		CWBackPixel | CWEventMask, &attrs);
	data->fd = ConnectionNumber(data->display);

	set_window_name(data->display, data->window, opts->title);

	class_hint.res_name = (char *) opts->class_name;
	class_hint.res_class = (char *) opts->class_name;
	XSetClassHint(data->display, data->window, &class_hint);

	set_window_type(data->display, data->window);
	set_window_state(data->display, data->window);
	set_cardinal(data->display, data->window, "_NET_WM_PID", (unsigned long) getpid());

	/*
	 * Compositors that honour _NET_WM_WINDOW_OPACITY (KDE Plasma, GNOME,
	 * sway, ...) will render the window fully transparent.  Hyprland
	 * ignores the property for XWayland windows, so the launcher script
	 * installs an equivalent window rule there; see data/.
	 */
	/* Hidden by default: empty input region so clicks go through.  With
	 * --show the window is visible and has to be draggable. */
	set_input_region(data->display, data->window, opts->show, 0, 0);

	if(!opts->show)
		set_cardinal(data->display, data->window, "_NET_WM_WINDOW_OPACITY", 0);

	/*
	 * An idle bridge has nothing to mirror, so it keeps no window on the
	 * desktop: the window is mapped when a stream starts or when the user
	 * asks to see it.  Mapping it here would leave a client the compositor
	 * lists even though it shows nothing.
	 */
	data->visible = opts->show ? 1 : 0;
	data->mapped = 0;
	if(opts->show) {
		data->mapped = 1;
		XMapWindow(data->display, data->window);
	}
	XFlush(data->display);

	return data;
}

/*
 * Fits width x height inside the current quality cap, keeping the aspect
 * ratio.  Never upscales: a cap larger than the source keeps it 1:1.
 */
static void capped_size(const struct xdata *data, unsigned int width, unsigned int height,
			unsigned int *out_width, unsigned int *out_height) {
	*out_width = width;
	*out_height = height;

	if(data->max_width == 0 || data->max_height == 0)
		return;
	if(width <= data->max_width && height <= data->max_height)
		return;

	if((uint64_t) width * data->max_height <= (uint64_t) height * data->max_width) {
		/* The height is the limiting dimension. */
		*out_height = data->max_height;
		*out_width = (unsigned int) (((uint64_t) width * data->max_height +
					      height / 2) / height);
	} else {
		*out_width = data->max_width;
		*out_height = (unsigned int) (((uint64_t) height * data->max_width +
					       width / 2) / width);
	}
	if(*out_width == 0)
		*out_width = 1;
	if(*out_height == 0)
		*out_height = 1;
}

static void apply_output_size(struct xdata *data, unsigned int width, unsigned int height) {
	XWindowChanges changes;

	if(data->image && width == data->width && height == data->height)
		return;

	image_free(data);

	data->width = width;
	data->height = height;

	if(image_alloc(data, width, height) != 0) {
		fprintf(stderr, "Out of memory for a %ux%u frame\n", width, height);
		return;
	}

	changes.width = (int) width;
	changes.height = (int) height;
	XConfigureWindow(data->display, data->window, CWWidth | CWHeight, &changes);
	/* Keep the (interactive) input region in sync with the new size. */
	if(data->visible)
		set_input_region(data->display, data->window, 1, data->width, data->height);
	data->repaint = 1;
	XFlush(data->display);
}

static void set_source_size(struct xdata *data, unsigned int width, unsigned int height,
			    int swap_rb) {
	unsigned int out_width, out_height;

	data->src_width = width;
	data->src_height = height;
	data->swap_rb = swap_rb;

	capped_size(data, width, height, &out_width, &out_height);
	data->scaled = out_width != width || out_height != height;
	apply_output_size(data, out_width, out_height);
}

void xhandler_resize(struct xdata *data, unsigned int width, unsigned int height, int swap_rb) {
	if(!data || !data->display)
		return;

	if(width == 0 || height == 0 || width > 16384 || height > 16384) {
		fprintf(stderr, "Refusing a %ux%u frame (limits are 1..16384)\n", width, height);
		return;
	}

	set_source_size(data, width, height, swap_rb);
}

void xhandler_set_max_size(struct xdata *data, unsigned int max_width, unsigned int max_height) {
	if(!data)
		return;
	if(data->max_width == max_width && data->max_height == max_height)
		return;

	data->max_width = max_width;
	data->max_height = max_height;
	/* The capture thread applies the cap on its next upload - it may own the
	 * image while a portal stream runs - and this makes sure that upload
	 * happens even when the next frame carries no damage. */
	data->repaint = 1;
}

int xhandler_scaled(const struct xdata *data) {
	return data ? data->scaled : 0;
}

int xhandler_repaint_pending(const struct xdata *data) {
	return data ? data->repaint : 0;
}

int xhandler_upload(struct xdata *data, const void *pixels, uint32_t size, int32_t stride,
		    int src_x, int src_y, unsigned int width, unsigned int height,
		    int transform) {
	int rotated;
	unsigned int out_w, out_h;

	if(!data || !data->display || stride <= 0 || !pixels)
		return -1;
	if(width == 0 || height == 0)
		return -1;

	rotated = (transform == 1 || transform == 3 ||
		   transform == 5 || transform == 7);
	out_w = rotated ? height : width;
	out_h = rotated ? width : height;

	/* Applies a quality change and keeps the window in sync with the frame. */
	set_source_size(data, out_w, out_h, data->swap_rb);

	if(!data->image || !data->buffer)
		return -1;

	if(data->scaled)
		return image_convert_scaled((unsigned char *) data->buffer,
					    data->width, data->height,
					    (size_t) data->width * 4,
					    pixels, size, stride,
					    src_x, src_y, width, height,
					    data->swap_rb, transform);

	return image_convert((unsigned char *) data->buffer, data->width, data->height,
			     (size_t) data->width * 4,
			     pixels, size, stride,
			     src_x, src_y, width, height,
			     data->swap_rb, transform);
}

void xhandler_present(struct xdata *data, int x, int y, unsigned int width, unsigned int height) {
	int max_w, max_h;

	if(!data || !data->image || !data->display)
		return;

	/* A new image or window size needs one full repaint; a damage rectangle
	 * computed for the previous size must not be used. */
	if(data->repaint) {
		x = 0;
		y = 0;
		width = data->width;
		height = data->height;
	}

	max_w = (int) data->width;
	max_h = (int) data->height;

	if(x < 0) {
		unsigned int cut = (unsigned int) (-x);

		if(width <= cut)
			return;
		width -= cut;
		x = 0;
	}
	if(y < 0) {
		unsigned int cut = (unsigned int) (-y);

		if(height <= cut)
			return;
		height -= cut;
		y = 0;
	}
	if(x >= max_w || y >= max_h)
		return;
	if((int) width > max_w - x)
		width = (unsigned int) (max_w - x);
	if((int) height > max_h - y)
		height = (unsigned int) (max_h - y);
	if(width == 0 || height == 0)
		return;

	if(data->shm_attached) {
		/*
		 * XShmPutImage copies from the shared segment inside the server;
		 * after XSync() the segment can be reused for the next frame.
		 */
		XShmPutImage(data->display, data->window,
			     DefaultGC(data->display, DefaultScreen(data->display)),
			     data->image, x, y, x, y, width, height, False);
		XSync(data->display, False);
	} else {
		XPutImage(data->display, data->window,
			  DefaultGC(data->display, DefaultScreen(data->display)),
			  data->image, x, y, x, y, width, height);
		XFlush(data->display);
	}
	data->repaint = 0;
}

void xhandler_draw(struct xdata *data, const void *pixels, uint32_t size, int32_t stride,
		   int src_x, int src_y, unsigned int width, unsigned int height,
		   int transform) {
	if(xhandler_upload(data, pixels, size, stride, src_x, src_y, width, height, transform) == 0)
		xhandler_present(data, 0, 0, data->width, data->height);
}

/*
 * An unmapped window is not on the desktop at all: the compositor lists no
 * client for it, it cannot take focus and nothing can be drawn over it.  A
 * mapped window with opacity 0 still shows up in window lists and overviews,
 * which is what a stopped or quit bridge must not leave behind.
 */
void xhandler_set_mapped(struct xdata *data, int mapped) {
	if(!data || !data->display || !data->window)
		return;

	mapped = mapped ? 1 : 0;
	if(data->mapped == mapped)
		return;

	if(mapped)
		XMapWindow(data->display, data->window);
	else
		XUnmapWindow(data->display, data->window);
	data->mapped = mapped;
	XFlush(data->display);
}

void xhandler_set_visible(struct xdata *data, int visible) {
	if(!data || !data->display || !data->window)
		return;

	visible = visible ? 1 : 0;
	set_cardinal(data->display, data->window, "_NET_WM_WINDOW_OPACITY",
		     visible ? 0xffffffffUL : 0);
	/* Shown windows accept the pointer so the compositor can drag them;
	 * hidden ones stay click-through. */
	set_input_region(data->display, data->window, visible, data->width, data->height);
	if(visible)
		XRaiseWindow(data->display, data->window);
	data->visible = visible;
	XFlush(data->display);
}

void xhandler_routine(struct xdata *data) {
	if(!data || !data->display)
		return;

	while(XPending(data->display)) {
		XEvent ev;

		XNextEvent(data->display, &ev);
	}
}

void xhandler_destroy(struct xdata *data) {
	if(!data)
		return;

	image_free(data);
	if(data->display) {
		if(data->window)
			XDestroyWindow(data->display, data->window);
		XCloseDisplay(data->display);
	}
	free(data);
}

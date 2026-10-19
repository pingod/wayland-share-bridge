/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef X11_HANDLER_H
#define X11_HANDLER_H

#include <stdint.h>

#include <X11/Xlib.h>

#include "config.h"

struct xdata {
	int fd;

	unsigned int width;
	unsigned int height;
	int swap_rb;

	uint32_t *buffer;

	Display *display;
	Window window;
	XImage *image;
	int visible;
	int pos_x;
	int pos_y;
	int pos_saved;
};

struct xdata *xhandler_init(const struct options *opts);

void xhandler_resize(struct xdata *data, unsigned int width, unsigned int height, int swap_rb);

void xhandler_draw(struct xdata *data, const void *pixels, uint32_t size, int32_t stride);

/*
 * Shows or hides the mirror window.  On compositors that honour
 * _NET_WM_WINDOW_OPACITY (KDE, GNOME, ...) the opacity property is enough;
 * the caller can additionally apply a compositor specific rule (Hyprland).
 */
void xhandler_set_visible(struct xdata *data, int visible);

uint8_t xhandler_routine(struct xdata *data);

void xhandler_destroy(struct xdata *data);

#endif /* X11_HANDLER_H */

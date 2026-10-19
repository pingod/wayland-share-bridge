/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef WAYLAND_CAPTURE_H
#define WAYLAND_CAPTURE_H

#include <stdint.h>

#include "config.h"
#include "x11_handler.h"

/* A capturable source advertised by the compositor. */
struct capture_source {
	enum source_kind kind;
	char id[192];
	char label[192];
};

/*
 * Connects to the compositor's Wayland socket and binds the image copy
 * capture protocols.  Returns 0 on success, -1 when the compositor does not
 * support direct capture (the caller should fall back to the portal).
 */
int wlcap_init(void);

int wlcap_fd(void);

/* Flush pending requests; returns non-zero when the connection died. */
int wlcap_flush(void);

/* Reads and dispatches every pending event (non-blocking). */
int wlcap_dispatch(void);

/*
 * Fills *list with a freshly allocated array describing the currently
 * available sources of the given kind (SCREEN, WINDOW, ANY).  Returns the
 * number of sources; the caller frees *list.
 */
int wlcap_list(enum source_kind kind, struct capture_source **list);

/*
 * Starts mirroring a source into the given X11 window.  *id is matched
 * against the advertised sources: an output name for screens, or
 * "app_id|title" (or just a title) for windows.  When id is NULL or does not
 * match, the first available source of that kind is used.  Returns 0 on
 * success.
 */
int wlcap_start(enum source_kind kind, const char *id, struct xdata *xdata);

void wlcap_stop(void);

int wlcap_active(void);

/* Human readable description of the current source. */
const char *wlcap_current_label(void);

/*
 * Called on every event loop iteration.  Requests a new frame when the
 * previous one has been consumed and the frame interval has elapsed.
 */
void wlcap_tick(void);

void wlcap_destroy(void);

#endif /* WAYLAND_CAPTURE_H */

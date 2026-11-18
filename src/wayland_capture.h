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
	/* Windows: the app id, used to group the tray menu by application. */
	char group[96];
	/* Windows: the bare title, so a grouped menu entry stays short. */
	char title[192];
};

/*
 * Connects to the compositor's Wayland socket and binds the image copy
 * capture protocols.  own_title is the title of the bridge window, so the
 * mirror is never offered as a capture source.  Returns 0 on success, -1
 * when the compositor does not support direct capture (the caller should
 * fall back to the portal).
 */
int wlcap_init(const char *own_title);

/* Reads and dispatches every pending event (non-blocking). */
int wlcap_dispatch(void);

/*
 * Fills *list with a freshly allocated array describing the currently
 * available sources of the given kind (SCREEN, WINDOW, ANY).  Returns the
 * number of sources; the caller frees *list.
 */
int wlcap_list(enum source_kind kind, struct capture_source **list);

/*
 * Starts mirroring a source into the given X11 window.  The request id is
 * matched against the advertised sources: the output name for screens, or
 * the foreign-toplevel identifier, "app_id|title", the title or the app_id
 * for windows.  An empty id means "the first source of that kind"; a
 * non-empty id that matches nothing fails instead of silently sharing
 * something else.  A cropped screen request mirrors only the given region.
 * Returns 0 on success.
 */
int wlcap_start(const struct source_request *req, struct xdata *xdata);

void wlcap_stop(void);

int wlcap_active(void);

/* Non-zero while the compositor has not completed a frame for ~2 s. */
int wlcap_stalled(void);

/* Human readable description of the current source. */
const char *wlcap_current_label(void);

/*
 * Called on every event loop iteration.  Requests a new frame when the
 * previous one has been consumed and the frame interval has elapsed.
 */
void wlcap_tick(void);

void wlcap_destroy(void);

#endif /* WAYLAND_CAPTURE_H */

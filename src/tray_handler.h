/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef TRAY_HANDLER_H
#define TRAY_HANDLER_H

#include "config.h"

struct tray;

/*
 * Creates the status notifier icon ("tray") plus the D-Bus control service.
 * Returns NULL when the session bus or the StatusNotifierWatcher is not
 * available; the bridge keeps running without a tray in that case.
 */
struct tray *thandler_init(const char *title);

int thandler_fd(struct tray *tray);

/* Dispatch queued messages.  Returns non-zero on a fatal bus error. */
int thandler_routine(struct tray *tray);

/* Returns -1 when nothing was requested, otherwise the kind to reselect. */
int thandler_take_reselect(struct tray *tray, enum source_kind *kind);

int thandler_take_quit(struct tray *tray);

void thandler_set_status(struct tray *tray, const char *status);

/* Shows the running version in the tray menu and tooltip. */
void thandler_set_version(struct tray *tray, const char *version);

/*
 * Updates the state shown by the menu: whether something is being shared, the
 * source that is currently shared (marked with a check mark) and whether the
 * mirror window is visible.
 */
void thandler_set_state(struct tray *tray, int sharing, int visible,
			enum source_kind kind, const char *id);

/* Returns 1 when the user asked to stop sharing. */
int thandler_take_stop(struct tray *tray);

/* Returns 1 when the user asked to toggle the mirror window visibility. */
int thandler_take_visibility(struct tray *tray);

struct capture_source;

/*
 * Registers the callback used to enumerate direct capture sources shown in the
 * menu.  Passing NULL clears it.
 */
void thandler_set_source_provider(struct tray *tray,
				  int (*provider)(enum source_kind, struct capture_source **));

/*
 * Returns 1 when the user picked a concrete source from the menu; fills *kind
 * and *id (the id points into the tray and stays valid until the next pick).
 */
int thandler_take_selection(struct tray *tray, enum source_kind *kind, const char **id);

void thandler_destroy(struct tray *tray);

/*
 * Control client used by --reselect, --quit and --status.  Sends the method
 * to a running instance.  Returns 0 on success and fills *reply_text (owned
 * by the caller) when the method returns a string.
 */
int tcontrol_request(const char *method, const char *argument, char **reply_text);

/* Returns 1 when another bridge instance exports the control interface. */
int tcontrol_is_running(void);

#endif /* TRAY_HANDLER_H */

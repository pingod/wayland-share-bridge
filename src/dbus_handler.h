/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef DBUS_HANDLER_H
#define DBUS_HANDLER_H

#include <signal.h>
#include <stdint.h>

#include <dbus/dbus.h>

#include "config.h"

struct ddata {
	DBusConnection *con;
	int fd;

	char *session;
	uint32_t node_id;
	int pipewire_fd;

	enum cursor_mode cursor_mode;
	enum source_kind source_kind;
	struct source_info source;
	int persist;
	const char *token_path;
	char *restore_token;

	/* Latest ScreenCast Response signal, captured by a message filter. */
	DBusMessage *pending_response;

	/* Called regularly while waiting for the portal, so the tray stays
	 * responsive even when a source picker dialog is open. */
	void (*tick)(void *user_data);
	void *tick_data;

	/* Set by main so a portal wait aborts when the bridge is asked to
	 * stop (e.g. a picker dialog is left open). */
	volatile sig_atomic_t *stop;
};

struct ddata *dhandler_init(enum cursor_mode cursor_mode, enum source_kind source_kind,
			    int persist, const char *token_path);

void dhandler_set_source_kind(struct ddata *data, enum source_kind kind);

void dhandler_set_tick(struct ddata *data, void (*tick)(void *user_data), void *tick_data);

void dhandler_set_stop_flag(struct ddata *data, volatile sig_atomic_t *flag);

uint8_t dhandler_create_session(struct ddata *data);

uint8_t dhandler_select_sources(struct ddata *data);

uint8_t dhandler_start(struct ddata *data);

uint8_t dhandler_open_pipewire_remote(struct ddata *data);

/* Close the portal session (used when the source is picked again). */
void dhandler_close_session(struct ddata *data);

const char *dhandler_describe_source(struct ddata *data);

uint8_t dhandler_routine(struct ddata *data);

void dhandler_destroy(struct ddata *data);

#endif /* DBUS_HANDLER_H */

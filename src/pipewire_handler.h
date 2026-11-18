/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef PIPEWIRE_HANDLER_H
#define PIPEWIRE_HANDLER_H

#include <stdint.h>

#include <pipewire/pipewire.h>

#include "x11_handler.h"

struct pdata {
	struct xdata *xdata;
	uint32_t node_id;
	uint32_t width;
	uint32_t height;
	/* Set when the stream cannot be created, connected or negotiated. */
	int failed;
	/* Set when the stream stops or reports an error. */
	int lost;

	struct pw_thread_loop *loop;
	struct pw_context *context;
	struct pw_core *core;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct pw_stream *stream;
	struct spa_hook stream_listener;
};

/*
 * Connects to the portal's PipeWire node.  The caller keeps ownership of fd:
 * the handler works on a private duplicate, which PipeWire closes when the
 * core is disconnected.  Returns NULL when the node does not appear.
 */
struct pdata *phandler_init(int fd, uint32_t node_id, struct xdata *xdata);

int phandler_connection_lost(struct pdata *data);

void phandler_clear_connection_lost(struct pdata *data);

void phandler_destroy(struct pdata *data);

#endif /* PIPEWIRE_HANDLER_H */

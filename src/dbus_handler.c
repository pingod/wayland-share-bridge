/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * dbus_handler.c - the xdg-desktop-portal ScreenCast client.
 *
 * Creates a ScreenCast session, lets the user pick what to share with the
 * desktop's own picker, and returns the PipeWire node plus an optional
 * restore token so the next start can skip the picker.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "dbus_handler.h"

#define PORTAL_BUS "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE "org.freedesktop.portal.ScreenCast"

static unsigned int token_counter;

static DBusMessage *method_call_new(const char *method) {
	return dbus_message_new_method_call(PORTAL_BUS, PORTAL_PATH, PORTAL_IFACE, method);
}

static const char *next_handle_token(const char *what) {
	static char token[64];

	snprintf(token, sizeof(token), "wayland_share_%s_%u", what, ++token_counter);

	return token;
}

static void append_dict_entry_string(DBusMessageIter *dict, const char *key, const char *value) {
	DBusMessageIter entry, variant;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(dict, &entry);
}

static void append_dict_entry_uint(DBusMessageIter *dict, const char *key, uint32_t value) {
	DBusMessageIter entry, variant;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &value);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(dict, &entry);
}

static int stop_requested(struct ddata *data) {
	return data->stop && !*data->stop;
}

static DBusMessage *wait_for_method_reply(struct ddata *data, DBusMessage *msg, int timeout_ms) {
	DBusError error;
	DBusMessage *reply;

	dbus_error_init(&error);
	reply = dbus_connection_send_with_reply_and_block(data->con, msg, timeout_ms, &error);
	if(!reply)
		dbus_error_free(&error);

	return reply;
}

static DBusHandlerResult response_filter(DBusConnection *con, DBusMessage *msg, void *user_data) {
	struct ddata *data = user_data;

	(void) con;

	if(dbus_message_is_signal(msg, "org.freedesktop.portal.Request", "Response")) {
		if(data->pending_response)
			dbus_message_unref(data->pending_response);
		data->pending_response = dbus_message_ref(msg);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusMessage *wait_for_response(struct ddata *data, const char *request) {
	int waited = 0;

	while(!stop_requested(data)) {
		if(data->pending_response) {
			if(dbus_message_has_path(data->pending_response, request)) {
				DBusMessage *response = data->pending_response;

				data->pending_response = NULL;
				return response;
			}

			dbus_message_unref(data->pending_response);
			data->pending_response = NULL;
		}

		if(!dbus_connection_read_write_dispatch(data->con, 100))
			return NULL;

		if(data->tick)
			data->tick(data->tick_data);

		waited += 100;
		if(waited > 300000)
			return NULL;
	}

	return NULL;
}

/* The first argument of a portal Response is the response code: 0 = ok. */
static uint32_t response_code(DBusMessage *msg) {
	DBusMessageIter iter;
	uint32_t code = 2;

	if(!msg || !dbus_message_iter_init(msg, &iter))
		return 2;
	if(dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32)
		dbus_message_iter_get_basic(&iter, &code);
	return code;
}

/*
 * Look up a variant in the Response results dictionary: a{sv}.  Returns the
 * iterator positioned on the value, or NULL when the key is missing.
 */
static int dict_lookup(DBusMessage *msg, const char *wanted, DBusMessageIter *value) {
	DBusMessageIter iter, dict;

	if(!dbus_message_iter_init(msg, &iter))
		return 0;
	if(dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32)
		return 0;
	if(!dbus_message_iter_next(&iter))
		return 0;
	if(dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return 0;

	dbus_message_iter_recurse(&iter, &dict);
	while(dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, variant;
		const char *key = NULL;

		dbus_message_iter_recurse(&dict, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		if(!dbus_message_iter_next(&entry))
			return 0;
		dbus_message_iter_recurse(&entry, &variant);

		if(key && !strcmp(key, wanted)) {
			*value = variant;
			return 1;
		}

		dbus_message_iter_next(&dict);
	}

	return 0;
}

static char *dict_lookup_string(DBusMessage *msg, const char *key) {
	DBusMessageIter value;
	const char *str = NULL;

	if(dict_lookup(msg, key, &value) != 1)
		return NULL;
	if(dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_STRING)
		return NULL;

	dbus_message_iter_get_basic(&value, &str);

	return str ? strdup(str) : NULL;
}

/*
 * "streams" is a(ua{sv}); returns the PipeWire node id of the first stream
 * and fills in the optional properties the portal knows about (source type,
 * size and position).  These properties are what tells us whether the user
 * picked a monitor or a window, so the next start can ask for the same kind.
 */
static uint32_t parse_stream(DBusMessage *msg, struct source_info *info) {
	DBusMessageIter value, array, entry, props;
	uint32_t node_id = 0;

	memset(info, 0, sizeof(*info));

	if(dict_lookup(msg, "streams", &value) != 1)
		return 0;
	if(dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_ARRAY)
		return 0;

	dbus_message_iter_recurse(&value, &array);
	if(dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_STRUCT)
		return 0;

	dbus_message_iter_recurse(&array, &entry);
	if(dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_UINT32)
		dbus_message_iter_get_basic(&entry, &node_id);

	if(!dbus_message_iter_next(&entry))
		return node_id;
	if(dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_ARRAY)
		return node_id;

	dbus_message_iter_recurse(&entry, &props);
	while(dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter dict_entry, variant;
		const char *key = NULL;

		dbus_message_iter_recurse(&props, &dict_entry);
		dbus_message_iter_get_basic(&dict_entry, &key);
		if(!dbus_message_iter_next(&dict_entry)) {
			dbus_message_iter_next(&props);
			continue;
		}
		dbus_message_iter_recurse(&dict_entry, &variant);

		if(key && !strcmp(key, "source_type")) {
			if(dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT32) {
				uint32_t type = 0;

				dbus_message_iter_get_basic(&variant, &type);
				info->type = (int) type;
				info->valid = 1;
			}
		} else if(key && (!strcmp(key, "size") || !strcmp(key, "position"))) {
			DBusMessageIter pair;
			int32_t first = 0, second = 0;

			if(dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRUCT) {
				dbus_message_iter_recurse(&variant, &pair);
				if(dbus_message_iter_get_arg_type(&pair) == DBUS_TYPE_INT32)
					dbus_message_iter_get_basic(&pair, &first);
				if(dbus_message_iter_next(&pair) && dbus_message_iter_get_arg_type(&pair) == DBUS_TYPE_INT32)
					dbus_message_iter_get_basic(&pair, &second);

				if(!strcmp(key, "size")) {
					info->width = first;
					info->height = second;
				} else {
					info->x = first;
					info->y = second;
				}
			}
		}

		dbus_message_iter_next(&props);
	}

	return node_id;
}

static int mkdir_parents(const char *path) {
	char *copy, *p;
	int ret = 0;

	copy = strdup(path);
	if(!copy)
		return -1;

	for(p = copy + 1; *p; p++) {
		if(*p != '/')
			continue;

		*p = 0;
		if(mkdir(copy, 0700) != 0 && errno != EEXIST) {
			ret = -1;
			break;
		}
		*p = '/';
	}

	free(copy);

	return ret;
}

static char *load_restore_token(const char *path) {
	FILE *file;
	char buffer[1024];

	if(!path)
		return NULL;

	file = fopen(path, "r");
	if(!file)
		return NULL;

	if(!fgets(buffer, sizeof(buffer), file)) {
		fclose(file);
		return NULL;
	}
	fclose(file);

	buffer[strcspn(buffer, "\r\n")] = 0;
	if(buffer[0] == 0)
		return NULL;

	return strdup(buffer);
}

static void save_restore_token(const char *path, const char *token) {
	FILE *file;

	if(!path || !token)
		return;

	if(mkdir_parents(path) != 0) {
		fprintf(stderr, "warning: cannot create directories for %s\n", path);
		return;
	}

	file = fopen(path, "w");
	if(!file) {
		fprintf(stderr, "warning: cannot save the restore token to %s\n", path);
		return;
	}
	fprintf(file, "%s\n", token);
	fclose(file);

	chmod(path, 0600);
}

static char *state_path_for(const char *token_path) {
	const char *slash;
	char *path;
	size_t length;

	if(!token_path)
		return NULL;

	slash = strrchr(token_path, '/');
	if(!slash)
		return strdup("wayland-share-bridge.state");

	length = (size_t) (slash - token_path) + 1;
	path = malloc(length + sizeof("state"));
	if(!path)
		return NULL;

	memcpy(path, token_path, length);
	memcpy(path + length, "state", sizeof("state"));

	return path;
}

/* Returns SOURCE_KIND_ANY when nothing was remembered yet. */
static enum source_kind load_remembered_kind(const char *token_path) {
	char *path = state_path_for(token_path);
	FILE *file;
	char buffer[128];
	enum source_kind kind = SOURCE_KIND_ANY;

	if(!path)
		return kind;

	file = fopen(path, "r");
	free(path);
	if(!file)
		return kind;

	while(fgets(buffer, sizeof(buffer), file)) {
		buffer[strcspn(buffer, "\r\n")] = 0;
		if(!strncmp(buffer, "source_kind=", 12)) {
			const char *value = buffer + 12;

			if(!strcmp(value, "screen"))
				kind = SOURCE_KIND_SCREEN;
			else if(!strcmp(value, "window"))
				kind = SOURCE_KIND_WINDOW;
		}
	}
	fclose(file);

	return kind;
}

static void save_remembered_kind(const char *token_path, enum source_kind kind) {
	char *path = state_path_for(token_path);
	FILE *file;

	if(!path)
		return;
	if(kind != SOURCE_KIND_SCREEN && kind != SOURCE_KIND_WINDOW) {
		free(path);
		return;
	}
	if(mkdir_parents(path) != 0) {
		free(path);
		return;
	}

	file = fopen(path, "w");
	free(path);
	if(!file)
		return;

	fprintf(file, "source_kind=%s\n", kind == SOURCE_KIND_SCREEN ? "screen" : "window");
	fclose(file);
}

struct ddata *dhandler_init(enum cursor_mode cursor_mode, enum source_kind source_kind,
			    int persist, const char *token_path) {
	struct ddata *data;
	DBusError error;

	data = calloc(1, sizeof(struct ddata));
	if(!data)
		return NULL;

	data->cursor_mode = cursor_mode;
	data->source_kind = source_kind;
	data->pipewire_fd = -1;
	data->persist = persist;
	data->token_path = token_path;

	dbus_error_init(&error);
	data->con = dbus_bus_get(DBUS_BUS_SESSION, &error);
	if(!data->con) {
		fprintf(stderr, "Cannot connect to the session bus: %s\n", error.message);
		dbus_error_free(&error);
		free(data);
		return NULL;
	}

	dbus_connection_get_unix_fd(data->con, &data->fd);

	/* Make sure the portal's Response signals are delivered to us. */
	dbus_bus_add_match(data->con,
		"type='signal',interface='org.freedesktop.portal.Request',member='Response'",
		&error);
	if(dbus_error_is_set(&error)) {
		fprintf(stderr, "warning: %s\n", error.message);
		dbus_error_free(&error);
	}

	dbus_connection_add_filter(data->con, response_filter, data, NULL);

	if(persist)
		data->restore_token = load_restore_token(token_path);

	if(data->source_kind == SOURCE_KIND_AUTO)
		data->source_kind = load_remembered_kind(token_path);

	if(data->source_kind == SOURCE_KIND_AUTO || data->source_kind > SOURCE_KIND_WINDOW)
		data->source_kind = SOURCE_KIND_ANY;

	return data;
}

void dhandler_set_tick(struct ddata *data, void (*tick)(void *user_data), void *tick_data) {
	if(!data)
		return;

	data->tick = tick;
	data->tick_data = tick_data;
}

void dhandler_set_stop_flag(struct ddata *data, volatile sig_atomic_t *flag) {
	if(data)
		data->stop = flag;
}

void dhandler_set_source_kind(struct ddata *data, enum source_kind kind) {
	if(!data)
		return;

	if(kind == SOURCE_KIND_AUTO)
		kind = load_remembered_kind(data->token_path);
	if(kind == SOURCE_KIND_AUTO || kind > SOURCE_KIND_WINDOW)
		kind = SOURCE_KIND_ANY;

	data->source_kind = kind;
}

const char *dhandler_describe_source(struct ddata *data) {
	static char description[160];

	if(!data || !data->source.valid)
		return "not sharing anything";

	snprintf(description, sizeof(description), "%s %dx%d at %d,%d",
		data->source.type == 1 ? "monitor" : (data->source.type == 2 ? "window" : "source"),
		data->source.width, data->source.height,
		data->source.x, data->source.y);

	return description;
}

uint8_t dhandler_create_session(struct ddata *data) {
	DBusError error;
	DBusMessage *msg, *reply, *response;
	DBusMessageIter iter, dict;
	const char *handle_token, *session_handle_token;
	const char *request;
	char *session;

	handle_token = next_handle_token("create");
	session_handle_token = next_handle_token("session");

	msg = method_call_new("CreateSession");
	if(!msg)
		return 1;

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
	append_dict_entry_string(&dict, "handle_token", handle_token);
	append_dict_entry_string(&dict, "session_handle_token", session_handle_token);
	dbus_message_iter_close_container(&iter, &dict);

	dbus_error_init(&error);
	reply = wait_for_method_reply(data, msg, 10000);
	dbus_message_unref(msg);
	if(!reply) {
		fprintf(stderr, "CreateSession failed: no reply from the portal\n");
		return 1;
	}

	if(!dbus_message_get_args(reply, &error, DBUS_TYPE_OBJECT_PATH, &request, DBUS_TYPE_INVALID)) {
		fprintf(stderr, "CreateSession: %s\n", error.message);
		dbus_error_free(&error);
		dbus_message_unref(reply);
		return 1;
	}
	request = strdup(request);
	dbus_message_unref(reply);
	if(!request)
		return 1;

	response = wait_for_response(data, request);
	free((char *) request);
	if(!response) {
		fprintf(stderr, "CreateSession: no response from the portal\n");
		return 1;
	}

	if(response_code(response) != 0) {
		fprintf(stderr, "CreateSession was rejected by the portal\n");
		dbus_message_unref(response);
		return 1;
	}

	session = dict_lookup_string(response, "session_handle");
	dbus_message_unref(response);
	if(!session) {
		fprintf(stderr, "CreateSession: no session handle in the reply\n");
		return 1;
	}

	data->session = session;

	return 0;
}

static int select_sources(struct ddata *data, int with_cursor, int with_persist) {
	DBusError error;
	DBusMessage *msg, *reply, *response;
	DBusMessageIter iter, dict;
	const char *handle_token;
	const char *request;
	uint32_t types;

	switch(data->source_kind) {
	case SOURCE_KIND_SCREEN:
		types = 1; /* MONITOR */
		break;
	case SOURCE_KIND_WINDOW:
		types = 2; /* WINDOW */
		break;
	default:
		types = 7; /* MONITOR | WINDOW | VIRTUAL */
		break;
	}

	handle_token = next_handle_token("select");

	msg = method_call_new("SelectSources");
	if(!msg)
		return -1;

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &data->session);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
	append_dict_entry_string(&dict, "handle_token", handle_token);
	append_dict_entry_uint(&dict, "types", types);
	if(with_cursor)
		append_dict_entry_uint(&dict, "cursor_mode", (uint32_t) data->cursor_mode);
	if(with_persist && data->restore_token)
		append_dict_entry_string(&dict, "restore_token", data->restore_token);
	if(with_persist && data->persist)
		append_dict_entry_uint(&dict, "persist_mode", 2);
	dbus_message_iter_close_container(&iter, &dict);

	dbus_error_init(&error);
	reply = wait_for_method_reply(data, msg, 10000);
	dbus_message_unref(msg);
	if(!reply) {
		fprintf(stderr, "SelectSources: %s\n", error.message);
		dbus_error_free(&error);
		return -1;
	}

	if(!dbus_message_get_args(reply, &error, DBUS_TYPE_OBJECT_PATH, &request, DBUS_TYPE_INVALID)) {
		fprintf(stderr, "SelectSources: %s\n", error.message);
		dbus_error_free(&error);
		dbus_message_unref(reply);
		return -1;
	}
	request = strdup(request);
	dbus_message_unref(reply);
	if(!request)
		return -1;

	response = wait_for_response(data, request);
	free((char *) request);
	if(!response) {
		fprintf(stderr, "SelectSources: no response from the portal\n");
		return -1;
	}

	if(response_code(response) == 1) {
		dbus_message_unref(response);
		fprintf(stderr, "Screen sharing was cancelled by the user\n");
		return -2;
	}
	if(response_code(response) != 0) {
		dbus_message_unref(response);
		return -1;
	}

	dbus_message_unref(response);

	return 0;
}

uint8_t dhandler_select_sources(struct ddata *data) {
	int ret;

	ret = select_sources(data, 1, data->persist);
	if(ret == -2)
		return 1;
	if(ret == 0)
		return 0;

	/* Some portals don't know/can't do restore tokens or cursor modes. */
	fprintf(stderr, "Retrying SelectSources with default options\n");
	ret = select_sources(data, 1, 0);
	if(ret == -2)
		return 1;
	if(ret == 0)
		return 0;

	ret = select_sources(data, 0, 0);
	if(ret == -2)
		return 1;
	return (ret == 0) ? 0 : 1;
}

uint8_t dhandler_start(struct ddata *data) {
	DBusError error;
	DBusMessage *msg, *reply, *response;
	DBusMessageIter iter, dict;
	const char *handle_token;
	const char *request;
	const char *parent_window = "";
	uint32_t code;

	handle_token = next_handle_token("start");

	msg = method_call_new("Start");
	if(!msg)
		return 1;

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &data->session);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &parent_window);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
	append_dict_entry_string(&dict, "handle_token", handle_token);
	dbus_message_iter_close_container(&iter, &dict);

	dbus_error_init(&error);
	reply = wait_for_method_reply(data, msg, 10000);
	dbus_message_unref(msg);
	if(!reply) {
		fprintf(stderr, "Start failed: no reply from the portal\n");
		return 1;
	}

	if(!dbus_message_get_args(reply, &error, DBUS_TYPE_OBJECT_PATH, &request, DBUS_TYPE_INVALID)) {
		fprintf(stderr, "Start: %s\n", error.message);
		dbus_error_free(&error);
		dbus_message_unref(reply);
		return 1;
	}
	request = strdup(request);
	dbus_message_unref(reply);
	if(!request)
		return 1;

	response = wait_for_response(data, request);
	free((char *) request);
	if(!response) {
		fprintf(stderr, "Start: no response from the portal\n");
		return 1;
	}

	code = response_code(response);
	if(code != 0) {
		dbus_message_unref(response);
		fprintf(stderr, "Screen sharing was cancelled by the user\n");
		return 1;
	}

	data->node_id = parse_stream(response, &data->source);
	if(data->node_id == 0) {
		fprintf(stderr, "Start: the portal did not return a stream node\n");
		dbus_message_unref(response);
		return 1;
	}

	fprintf(stderr, "Sharing a %s\n", dhandler_describe_source(data));

	if(data->token_path && (data->source.type == 1 || data->source.type == 2)) {
		enum source_kind kind = data->source.type == 1 ? SOURCE_KIND_SCREEN : SOURCE_KIND_WINDOW;

		save_remembered_kind(data->token_path, kind);
	}

	if(data->persist) {
		char *token = dict_lookup_string(response, "restore_token");

		if(token) {
			free(data->restore_token);
			data->restore_token = token;
			save_restore_token(data->token_path, token);
			fprintf(stderr, "Remembered the shared source (restore token saved)\n");
		} else {
			fprintf(stderr, "The portal did not return a restore token; the source will have to be picked again next time\n");
		}
	}

	dbus_message_unref(response);

	return 0;
}

uint8_t dhandler_open_pipewire_remote(struct ddata *data) {
	DBusError error;
	DBusMessage *msg, *reply;
	DBusMessageIter iter, dict;

	msg = method_call_new("OpenPipeWireRemote");
	if(!msg)
		return 1;

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &data->session);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
	dbus_message_iter_close_container(&iter, &dict);

	dbus_error_init(&error);
	reply = wait_for_method_reply(data, msg, 10000);
	dbus_message_unref(msg);
	if(!reply) {
		fprintf(stderr, "OpenPipeWireRemote failed: no reply from the portal\n");
		return 1;
	}

	if(!dbus_message_get_args(reply, &error, DBUS_TYPE_UNIX_FD, &data->pipewire_fd, DBUS_TYPE_INVALID)) {
		fprintf(stderr, "OpenPipeWireRemote: %s\n", error.message);
		dbus_error_free(&error);
		dbus_message_unref(reply);
		return 1;
	}

	dbus_message_unref(reply);

	return 0;
}

uint8_t dhandler_routine(struct ddata *data) {
	return !dbus_connection_read_write_dispatch(data->con, 1000);
}

void dhandler_close_session(struct ddata *data) {
	DBusMessage *msg;

	if(!data || !data->session)
		return;

	msg = dbus_message_new_method_call("org.freedesktop.portal.Session", data->session,
		"org.freedesktop.portal.Session", "Close");
	if(msg) {
		dbus_connection_send(data->con, msg, NULL);
		dbus_message_unref(msg);
	}
	dbus_connection_flush(data->con);

	free(data->session);
	data->session = NULL;
	data->node_id = 0;

	if(data->pipewire_fd >= 0) {
		close(data->pipewire_fd);
		data->pipewire_fd = -1;
	}

	memset(&data->source, 0, sizeof(data->source));
}

void dhandler_destroy(struct ddata *data) {
	if(!data)
		return;

	if(data->pending_response)
		dbus_message_unref(data->pending_response);
	data->pending_response = NULL;

	if(data->con) {
		dbus_connection_remove_filter(data->con, response_filter, data);
		dbus_connection_unref(data->con);
	}
	if(data->pipewire_fd >= 0)
		close(data->pipewire_fd);
	free(data->session);
	free(data->restore_token);
	free(data);
}

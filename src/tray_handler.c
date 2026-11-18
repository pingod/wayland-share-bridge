/* SPDX-License-Identifier: EUPL-1.2 */
#define _GNU_SOURCE
/*
 * tray_handler.c - status notifier icon, context menu and control service.
 *
 * The tray icon is a plain StatusNotifierItem with a
 * com.canonical.dbusmenu menu, implemented directly on libdbus so the bridge
 * does not need Qt, GTK or a menu library.  The menu lets the user pick the
 * shared source again (anything, screens only or windows only) without
 * touching a terminal.
 *
 * The same bus connection also exports a small control interface that
 * "--reselect", "--quit" and "--status" talk to.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "i18n.h"
#include "source_spec.h"
#include "tray_handler.h"
#include "util.h"
#include "wayland_capture.h"

#define SNI_IFACE "org.kde.StatusNotifierItem"
#define SNI_PATH "/StatusNotifierItem"
#define MENU_IFACE "com.canonical.dbusmenu"
#define MENU_PATH "/MenuBar"
#define CONTROL_IFACE "org.pingod.WaylandShareBridge.Control"
#define CONTROL_PATH "/Control"
#define CONTROL_NAME_PREFIX "org.pingod.WaylandShareBridge"
#define WATCHER_BUS "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"

#define ITEM_STOP 1
#define ITEM_VISIBLE 2
#define ITEM_SEPARATOR 3
#define ITEM_SEPARATOR_B 6
#define ITEM_PORTAL 4
#define ITEM_QUIT 5
#define ITEM_SCREENS 7
#define ITEM_WINDOWS 8
#define ITEM_CLEAR_SEARCH 9
#define ITEM_NO_SCREENS 999998
#define ITEM_NO_WINDOWS 999999
/* Per-application submenu ids. */
#define ITEM_APP_ID_BASE 0x30000000
/* Ids of selectable sources are derived from a hash of the source id instead
 * of the position in the (frequently reordered) list, so a click from a
 * slightly stale menu still resolves to the same source. */
#define ITEM_SCREEN_ID_BASE 0x10000000
#define ITEM_WINDOW_ID_BASE 0x20000000
#define ITEM_ID_RANGE 0x0FFFFFFF

struct tray {
	DBusConnection *con;
	int fd;

	char *title;
	char *sni_name;
	char *control_name;

	int reselect_pending;
	enum source_kind reselect_kind;
	int quit_pending;

	int selection_pending;
	char selection_spec[256];
	int have_menu;

	int stop_pending;
	int visibility_pending;
	int sharing;
	int visible;
	enum source_kind active_kind;
	char active_id[192];

	int (*source_provider)(enum source_kind kind, struct capture_source **list);
	struct capture_source *screens;
	int screen_count;
	struct capture_source *windows;
	int window_count;
	/* Case-insensitive substring that filters the Windows submenu. */
	char window_filter[128];

	uint32_t menu_revision;

	char version[32];
	char status[256];
};

static void append_dict_entry_string(DBusMessageIter *dict, const char *key, const char *value) {
	DBusMessageIter entry, variant;
	char safe[512];

	/* libdbus aborts the process on invalid UTF-8; never trust the caller. */
	utf8_copy(safe, sizeof(safe), value ? value : "");
	value = safe;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(dict, &entry);
}

static void append_dict_entry_bool(DBusMessageIter *dict, const char *key, int value) {
	DBusMessageIter entry, variant;
	dbus_bool_t boolean = value ? TRUE : FALSE;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &boolean);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(dict, &entry);
}

static void append_dict_entry_int(DBusMessageIter *dict, const char *key, int value) {
	DBusMessageIter entry, variant;
	dbus_int32_t number = value;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "i", &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &number);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(dict, &entry);
}

static void append_menu_item_ex(DBusMessageIter *children, int32_t id, const char *label,
				 int separator, int submenu, int enabled) {
	DBusMessageIter variant, item, props, kids;

	dbus_message_iter_open_container(children, DBUS_TYPE_VARIANT, "(ia{sv}av)", &variant);
	dbus_message_iter_open_container(&variant, DBUS_TYPE_STRUCT, NULL, &item);
	dbus_message_iter_append_basic(&item, DBUS_TYPE_INT32, &id);

	dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "{sv}", &props);
	if(separator) {
		append_dict_entry_string(&props, "type", "separator");
	} else {
		append_dict_entry_string(&props, "label", label);
		append_dict_entry_bool(&props, "enabled", enabled);
		append_dict_entry_bool(&props, "visible", 1);
		if(submenu)
			append_dict_entry_string(&props, "children-display", "submenu");
	}
	dbus_message_iter_close_container(&item, &props);

	dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "v", &kids);
	dbus_message_iter_close_container(&item, &kids);

	dbus_message_iter_close_container(&variant, &item);
	dbus_message_iter_close_container(children, &variant);
}

static int32_t source_menu_id(enum source_kind kind, const char *id);

/* Two different ids can hash to the same menu id; at most one of them can be
 * selected.  Cheap to detect and worth a warning instead of a wrong source. */
static void warn_menu_id_collisions(const struct capture_source *list, int count,
				    enum source_kind kind) {
	int i, j;

	for(i = 0; i < count; i++) {
		for(j = i + 1; j < count; j++) {
			if(source_menu_id(kind, list[i].id) == source_menu_id(kind, list[j].id)) {
				fprintf(stderr, "tray: menu id collision between '%s' and '%s'; "
					"one of them cannot be selected\n",
					list[i].id, list[j].id);
				return;
			}
		}
	}
}

static void refresh_sources(struct tray *tray) {
	free(tray->screens);
	tray->screens = NULL;
	tray->screen_count = 0;
	free(tray->windows);
	tray->windows = NULL;
	tray->window_count = 0;

	if(!tray->source_provider)
		return;

	tray->screen_count = tray->source_provider(SOURCE_KIND_SCREEN, &tray->screens);
	if(tray->screen_count < 0) {
		tray->screens = NULL;
		tray->screen_count = 0;
	}
	tray->window_count = tray->source_provider(SOURCE_KIND_WINDOW, &tray->windows);
	if(tray->window_count < 0) {
		tray->windows = NULL;
		tray->window_count = 0;
	}

	warn_menu_id_collisions(tray->screens, tray->screen_count, SOURCE_KIND_SCREEN);
	warn_menu_id_collisions(tray->windows, tray->window_count, SOURCE_KIND_WINDOW);
}

static uint32_t hash_string(const char *text) {
	uint32_t hash = 2166136261u;
	const unsigned char *p = (const unsigned char *) text;

	while(p && *p) {
		hash ^= *p++;
		hash *= 16777619u;
	}
	return hash;
}

static int32_t source_menu_id(enum source_kind kind, const char *id) {
	int32_t base = kind == SOURCE_KIND_SCREEN ? ITEM_SCREEN_ID_BASE : ITEM_WINDOW_ID_BASE;

	return base + (int32_t)(hash_string(id) % ITEM_ID_RANGE);
}

static int32_t group_menu_id(const char *group) {
	return ITEM_APP_ID_BASE + (int32_t)(hash_string(group) % ITEM_ID_RANGE);
}

static int find_source_index(struct tray *tray, enum source_kind kind, int32_t menu_id,
			     const char **id_out) {
	const struct capture_source *list;
	int count, i;

	if(kind == SOURCE_KIND_SCREEN) {
		list = tray->screens;
		count = tray->screen_count;
	} else {
		list = tray->windows;
		count = tray->window_count;
	}
	for(i = 0; i < count; i++) {
		if(source_menu_id(kind, list[i].id) == menu_id) {
			if(id_out)
				*id_out = list[i].id;
			return i;
		}
	}
	return -1;
}

static void append_menu_item_checked(DBusMessageIter *children, int32_t id, const char *label,
				     int enabled, int checked) {
	DBusMessageIter variant, item, props, kids;

	dbus_message_iter_open_container(children, DBUS_TYPE_VARIANT, "(ia{sv}av)", &variant);
	dbus_message_iter_open_container(&variant, DBUS_TYPE_STRUCT, NULL, &item);
	dbus_message_iter_append_basic(&item, DBUS_TYPE_INT32, &id);

	dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "{sv}", &props);
	append_dict_entry_string(&props, "label", label);
	append_dict_entry_bool(&props, "enabled", enabled);
	append_dict_entry_bool(&props, "visible", 1);
	append_dict_entry_string(&props, "toggle-type", "checkmark");
	append_dict_entry_int(&props, "toggle-state", checked ? 1 : 0);
	dbus_message_iter_close_container(&item, &props);

	dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "v", &kids);
	dbus_message_iter_close_container(&item, &kids);

	dbus_message_iter_close_container(&variant, &item);
	dbus_message_iter_close_container(children, &variant);
}

static int is_active_source(const struct tray *tray, enum source_kind kind, const char *id) {
	return tray->sharing && tray->active_kind == kind && id && !strcmp(tray->active_id, id);
}

static const char *group_label_of(const struct capture_source *src) {
	return src->group[0] ? src->group : "Other";
}

static int window_matches(const struct tray *tray, const struct capture_source *src) {
	const char *query = tray->window_filter;

	if(!query[0])
		return 1;
	return strcasestr(src->title, query) || strcasestr(src->group, query) ||
	       strcasestr(src->label, query) || strcasestr(src->id, query);
}

static int group_window_count(const struct tray *tray, const char *group, int matching_only) {
	int i, count = 0;

	for(i = 0; i < tray->window_count; i++) {
		if(strcmp(group_label_of(&tray->windows[i]), group))
			continue;
		if(matching_only && !window_matches(tray, &tray->windows[i]))
			continue;
		count++;
	}
	return count;
}

static int find_group_by_id(struct tray *tray, int32_t id, char *buffer, size_t size) {
	int i;

	for(i = 0; i < tray->window_count; i++) {
		const char *group = group_label_of(&tray->windows[i]);

		if(group_menu_id(group) == id) {
			snprintf(buffer, size, "%s", group);
			return 1;
		}
	}
	return 0;
}

static void window_leaf_label(const struct capture_source *src, char *buffer, size_t size) {
	const char *text = src->title[0] ? src->title :
			   (src->label[0] ? src->label : src->id);

	ellipsize(text, buffer, size, 46);
}

static void append_screens(DBusMessageIter *children, struct tray *tray) {
	int i;

	for(i = 0; i < tray->screen_count; i++) {
		const struct capture_source *src = &tray->screens[i];
		char label[240];
		char raw[260];

		snprintf(raw, sizeof(raw), tr("Screen: %s", "屏幕: %s"),
			 src->label[0] ? src->label : src->id);
		utf8_copy(label, sizeof(label), raw);
		append_menu_item_checked(children, source_menu_id(SOURCE_KIND_SCREEN, src->id),
					 label, 1,
					 is_active_source(tray, SOURCE_KIND_SCREEN, src->id));
	}
	if(tray->screen_count == 0)
		append_menu_item_ex(children, ITEM_NO_SCREENS, tr("No screen found", "未找到屏幕"), 0, 0, 0);
}

static void append_group_windows(DBusMessageIter *children, struct tray *tray,
				 const char *group) {
	int i;

	for(i = 0; i < tray->window_count; i++) {
		const struct capture_source *src = &tray->windows[i];
		char label[192];

		if(strcmp(group_label_of(src), group) || !window_matches(tray, src))
			continue;
		window_leaf_label(src, label, sizeof(label));
		append_menu_item_checked(children, source_menu_id(SOURCE_KIND_WINDOW, src->id),
					 label, 1,
					 is_active_source(tray, SOURCE_KIND_WINDOW, src->id));
	}
}

static void append_item_struct(DBusMessageIter *iter, struct tray *tray, int32_t id);

static void append_menu_item_variant(DBusMessageIter *children, struct tray *tray, int32_t id) {
	DBusMessageIter variant;

	dbus_message_iter_open_container(children, DBUS_TYPE_VARIANT, "(ia{sv}av)", &variant);
	append_item_struct(&variant, tray, id);
	dbus_message_iter_close_container(children, &variant);
}

/* Windows are grouped by application so a long list stays navigable. */
static void append_windows_menu(DBusMessageIter *children, struct tray *tray) {
	int i = 0, groups = 0;

	if(tray->window_filter[0]) {
		char query[96];
		char label[160];

		ellipsize(tray->window_filter, query, sizeof(query), 30);
		snprintf(label, sizeof(label), tr("Search: %s (clear)", "搜索: %s（清除）"),
			 query);
		append_menu_item_ex(children, ITEM_CLEAR_SEARCH, label, 0, 0, 1);
	}

	while(i < tray->window_count) {
		const char *group = group_label_of(&tray->windows[i]);
		int j = i;

		while(j < tray->window_count &&
		      !strcmp(group_label_of(&tray->windows[j]), group))
			j++;

		if(group_window_count(tray, group, 1) > 0) {
			append_menu_item_variant(children, tray, group_menu_id(group));
			groups++;
		}
		i = j;
	}

	if(groups == 0)
		append_menu_item_ex(children, ITEM_NO_WINDOWS, tr("No window found", "未找到窗口"), 0, 0, 0);
}

static void append_root_items(DBusMessageIter *children, struct tray *tray) {
	const char *toggle = tray->visible ?
		tr("Hide shared window", "隐藏共享窗口") :
		tr("Show shared window", "显示共享窗口");

	append_menu_item_ex(children, ITEM_STOP, tr("Stop sharing", "停止共享"),
			    0, 0, tray->sharing);
	append_menu_item_ex(children, ITEM_VISIBLE, toggle, 0, 0, tray->sharing);
	append_menu_item_ex(children, ITEM_SEPARATOR, NULL, 1, 0, 0);
	append_menu_item_variant(children, tray, ITEM_SCREENS);
	append_menu_item_variant(children, tray, ITEM_WINDOWS);
	append_menu_item_ex(children, ITEM_SEPARATOR_B, NULL, 1, 0, 0);
	append_menu_item_ex(children, ITEM_PORTAL, tr("Share via system portal…", "使用系统门户共享…"), 0, 0, 1);
	append_menu_item_ex(children, ITEM_QUIT, tr("Quit", "退出"), 0, 0, 1);
}

/*
 * Appends one dbusmenu item, including its children for the root, the
 * Screens/Windows submenus and the per-application submenus.  GetLayout(parent)
 * and the root layout both go through here, so the tree has one builder.
 */
static void append_item_struct(DBusMessageIter *iter, struct tray *tray, int32_t id) {
	DBusMessageIter item, props, children;
	char group_buffer[96];
	const char *group = NULL;

	if(id >= ITEM_APP_ID_BASE && id < ITEM_APP_ID_BASE + ITEM_ID_RANGE &&
	   find_group_by_id(tray, id, group_buffer, sizeof(group_buffer)))
		group = group_buffer;

	dbus_message_iter_open_container(iter, DBUS_TYPE_STRUCT, NULL, &item);
	dbus_message_iter_append_basic(&item, DBUS_TYPE_INT32, &id);
	dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "{sv}", &props);

	if(id == 0) {
		append_dict_entry_string(&props, "children-display", "submenu");
	} else if(id == ITEM_SCREENS || id == ITEM_WINDOWS) {
		int windows = id == ITEM_WINDOWS;
		char label[64];

		if(windows && tray->window_filter[0])
			snprintf(label, sizeof(label), "%s",
				 tr("Windows (search)", "窗口（搜索）"));
		else
			snprintf(label, sizeof(label), "%s",
				 windows ? tr("Windows", "窗口") : tr("Screens", "屏幕"));
		append_dict_entry_string(&props, "label", label);
		append_dict_entry_bool(&props, "enabled",
				       windows ? tray->window_count > 0 : tray->screen_count > 0);
		append_dict_entry_bool(&props, "visible", 1);
		append_dict_entry_string(&props, "children-display", "submenu");
	} else if(group) {
		char name[128];
		char label[160];

		ellipsize(group, name, sizeof(name), 28);
		snprintf(label, sizeof(label), "%s (%d)", name,
			 group_window_count(tray, group, 1));
		append_dict_entry_string(&props, "label", label);
		append_dict_entry_bool(&props, "enabled",
				       group_window_count(tray, group, 1) > 0);
		append_dict_entry_bool(&props, "visible", 1);
		append_dict_entry_string(&props, "children-display", "submenu");
	}
	dbus_message_iter_close_container(&item, &props);

	dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "v", &children);
	if(id == 0)
		append_root_items(&children, tray);
	else if(id == ITEM_SCREENS)
		append_screens(&children, tray);
	else if(id == ITEM_WINDOWS)
		append_windows_menu(&children, tray);
	else if(group)
		append_group_windows(&children, tray, group);
	dbus_message_iter_close_container(&item, &children);

	dbus_message_iter_close_container(iter, &item);
}

static void emit_layout_updated(struct tray *tray, int32_t parent) {
	DBusMessage *signal;
	uint32_t revision;

	if(!tray->con || !tray->have_menu)
		return;
	signal = dbus_message_new_signal(MENU_PATH, MENU_IFACE, "LayoutUpdated");
	if(!signal)
		return;
	revision = ++tray->menu_revision;
	dbus_message_append_args(signal, DBUS_TYPE_UINT32, &revision,
				 DBUS_TYPE_INT32, &parent, DBUS_TYPE_INVALID);
	dbus_connection_send(tray->con, signal, NULL);
	dbus_connection_flush(tray->con);
	dbus_message_unref(signal);
}

/*
 * Tray hosts cache the SNI properties, so a changed tooltip/status must be
 * announced with the matching signal or the version and the sharing state
 * never reach the icon.
 */
static void emit_sni_signal(struct tray *tray, const char *member) {
	DBusMessage *signal;

	if(!tray->con || !tray->have_menu)
		return;
	signal = dbus_message_new_signal(SNI_PATH, SNI_IFACE, member);
	if(!signal)
		return;
	dbus_connection_send(tray->con, signal, NULL);
	dbus_connection_flush(tray->con);
	dbus_message_unref(signal);
}

static int append_sni_property_value(DBusMessageIter *iter, struct tray *tray, const char *name) {
	DBusMessageIter variant, item;
	const char *text;
	const char *object = MENU_PATH;
	int32_t window_id = 0;
	dbus_bool_t item_is_menu = TRUE;

	if(!strcmp(name, "Category"))
		text = "ApplicationStatus";
	else if(!strcmp(name, "Id"))
		text = "wayland-share-bridge";
	else if(!strcmp(name, "Title"))
		text = tray->title;
	else if(!strcmp(name, "Status"))
		text = tray->sharing ? "Active" : "Passive";
	else if(!strcmp(name, "IconName"))
		text = "video-display";
	else if(!strcmp(name, "IconThemePath"))
		text = "";
	else if(!strcmp(name, "ToolTip")) {
		const char *icon = "video-display";
		/* The tray host renders the tooltip title as the menu header, so the
		 * version lives here instead of in a menu entry of its own. */
		char tooltip_title_buf[192];
		const char *tooltip_title = tooltip_title_buf;
		char tooltip_buf[320];
		const char *tooltip_body;

		snprintf(tooltip_buf, sizeof(tooltip_buf), "%s v%s",
			 tray->title, tray->version[0] ? tray->version : "?");
		utf8_copy(tooltip_title_buf, sizeof(tooltip_title_buf), tooltip_buf);
		snprintf(tooltip_buf, sizeof(tooltip_buf), "%s",
			 tray->status[0] ? tray->status : tr("Not sharing", "未共享"));
		tooltip_body = tooltip_buf;
		DBusMessageIter array;

		dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
						 "(sa(iiay)ss)", &variant);
		dbus_message_iter_open_container(&variant, DBUS_TYPE_STRUCT, NULL, &item);
		dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &icon);
		dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "(iiay)", &array);
		dbus_message_iter_close_container(&item, &array);
		dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &tooltip_title);
		dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &tooltip_body);
		dbus_message_iter_close_container(&variant, &item);
		dbus_message_iter_close_container(iter, &variant);
		return 0;
	} else if(!strcmp(name, "Menu")) {
		dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "o", &variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_OBJECT_PATH, &object);
		dbus_message_iter_close_container(iter, &variant);
		return 0;
	} else if(!strcmp(name, "ItemIsMenu")) {
		dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "b", &variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &item_is_menu);
		dbus_message_iter_close_container(iter, &variant);
		return 0;
	} else if(!strcmp(name, "WindowId")) {
		dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "i", &variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &window_id);
		dbus_message_iter_close_container(iter, &variant);
		return 0;
	} else {
		return -1;
	}

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &text);
	dbus_message_iter_close_container(iter, &variant);

	return 0;
}

static void append_sni_property_entry(DBusMessageIter *dict, struct tray *tray, const char *name) {
	DBusMessageIter entry;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
	append_sni_property_value(&entry, tray, name);
	dbus_message_iter_close_container(dict, &entry);
}

static const char *sni_property_names[] = {
	"Category", "Id", "Title", "Status", "IconName",
	"IconThemePath", "Menu", "ItemIsMenu", "WindowId", "ToolTip", NULL
};

static void send_reply(DBusConnection *con, DBusMessage *reply) {
	if(!reply)
		return;
	dbus_connection_send(con, reply, NULL);
	dbus_connection_flush(con);
	dbus_message_unref(reply);
}

static int append_menu_property_value(DBusMessageIter *iter, const char *property) {
	if(strcmp(property, "IconThemePath") != 0)
		return -1;
	{
		DBusMessageIter variant, array;

		dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "as", &variant);
		dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &array);
		dbus_message_iter_close_container(&variant, &array);
		dbus_message_iter_close_container(iter, &variant);
	}
	return 0;
}

static void append_menu_property_entry(DBusMessageIter *dict, const char *property) {
	DBusMessageIter entry;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &property);
	append_menu_property_value(&entry, property);
	dbus_message_iter_close_container(dict, &entry);
}

static DBusHandlerResult handle_properties(DBusConnection *con, DBusMessage *msg, struct tray *tray) {
	DBusMessage *reply;
	DBusMessageIter iter;

	if(dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "Get")) {
		const char *iface = NULL, *property = NULL;

		if(!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &iface,
					  DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID))
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		(void) iface;

		reply = dbus_message_new_method_return(msg);
		if(!reply)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		dbus_message_iter_init_append(reply, &iter);
		{
			const char *path = dbus_message_get_path(msg);
			int ok = path && strcmp(path, MENU_PATH) == 0
				? append_menu_property_value(&iter, property)
				: append_sni_property_value(&iter, tray, property);

			if(ok != 0) {
			dbus_message_unref(reply);
				send_reply(con, dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_PROPERTY,
								       "No such property"));
				return DBUS_HANDLER_RESULT_HANDLED;
			}
		}
		send_reply(con, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if(dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "GetAll")) {
		DBusMessageIter dict;
		int i;

		reply = dbus_message_new_method_return(msg);
		if(!reply)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		dbus_message_iter_init_append(reply, &iter);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
		if(dbus_message_get_path(msg) && strcmp(dbus_message_get_path(msg), MENU_PATH) == 0) {
			append_menu_property_entry(&dict, "IconThemePath");
		} else {
			for(i = 0; sni_property_names[i]; i++)
				append_sni_property_entry(&dict, tray, sni_property_names[i]);
		}
		dbus_message_iter_close_container(&iter, &dict);
		send_reply(con, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void request_reselect(struct tray *tray, enum source_kind kind) {
	tray->reselect_pending = 1;
	tray->reselect_kind = kind;
}

static DBusHandlerResult handle_sni(DBusConnection *con, DBusMessage *msg, struct tray *tray) {
	const char *member = dbus_message_get_member(msg);

	(void) tray;

	if(dbus_message_is_method_call(msg, SNI_IFACE, "Activate") ||
	   dbus_message_is_method_call(msg, SNI_IFACE, "SecondaryActivate") ||
	   dbus_message_is_method_call(msg, SNI_IFACE, "ContextMenu")) {
		/* The host opens the menu itself (ItemIsMenu is true); nothing to do. */
		send_reply(con, dbus_message_new_method_return(msg));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if(dbus_message_is_method_call(msg, SNI_IFACE, "Scroll")) {
		send_reply(con, dbus_message_new_method_return(msg));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if(member && !strcmp(member, "ProvideXdgActivationToken")) {
		send_reply(con, dbus_message_new_method_return(msg));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void handle_menu_event(struct tray *tray, int32_t id) {
	const char *source_id = NULL;
	enum source_kind kind;

	if(id >= ITEM_SCREEN_ID_BASE && id < ITEM_SCREEN_ID_BASE + ITEM_ID_RANGE)
		kind = SOURCE_KIND_SCREEN;
	else if(id >= ITEM_WINDOW_ID_BASE && id < ITEM_WINDOW_ID_BASE + ITEM_ID_RANGE)
		kind = SOURCE_KIND_WINDOW;
	else
		kind = SOURCE_KIND_ANY;

	if(kind != SOURCE_KIND_ANY) {
		refresh_sources(tray);
		if(find_source_index(tray, kind, id, &source_id) >= 0) {
			char spec[320];

			tray->selection_pending = 1;
			snprintf(spec, sizeof(spec), "%s:%s",
				 kind == SOURCE_KIND_SCREEN ? "screen" : "window",
				 source_id);
			utf8_copy(tray->selection_spec, sizeof(tray->selection_spec), spec);
		} else {
			thandler_set_status(tray, "This source is gone; reopen the menu");
		}
		return;
	}

	switch(id) {
	case ITEM_STOP:
		tray->stop_pending = 1;
		break;
	case ITEM_VISIBLE:
		tray->visibility_pending = 1;
		break;
	case ITEM_PORTAL:
		request_reselect(tray, SOURCE_KIND_ANY);
		break;
	case ITEM_QUIT:
		tray->quit_pending = 1;
		break;
	case ITEM_CLEAR_SEARCH:
		tray->window_filter[0] = '\0';
		emit_layout_updated(tray, 0);
		break;
	default:
		break;
	}
}


/*
 * Builds one (id, properties) pair for GetGroupProperties.  Unknown ids are
 * dropped before any container is opened.
 */
static void append_group_property(DBusMessageIter *array, struct tray *tray, int32_t id) {
	DBusMessageIter entry, props;
	char label[240];
	char raw[260];
	int known = 1, enabled = 1, separator = 0, submenu_entry = 0;
	enum source_kind kind = SOURCE_KIND_ANY;
	const char *source_id = NULL;

	label[0] = '\0';

	if(id == ITEM_STOP) {
		snprintf(label, sizeof(label), "%s", tr("Stop sharing", "停止共享"));
		enabled = tray->sharing;
	} else if(id == ITEM_VISIBLE) {
		snprintf(label, sizeof(label), "%s",
			 tray->visible ? tr("Hide shared window", "隐藏共享窗口") :
					 tr("Show shared window", "显示共享窗口"));
		enabled = tray->sharing;
	} else if(id == ITEM_PORTAL) {
		snprintf(label, sizeof(label), "%s",
			 tr("Share via system portal…", "使用系统门户共享…"));
	} else if(id == ITEM_QUIT) {
		snprintf(label, sizeof(label), "%s", tr("Quit", "退出"));
	} else if(id == ITEM_SEPARATOR || id == ITEM_SEPARATOR_B) {
		separator = 1;
	} else if(id == ITEM_CLEAR_SEARCH) {
		char query[96];

		ellipsize(tray->window_filter, query, sizeof(query), 30);
		snprintf(label, sizeof(label), tr("Search: %s (clear)", "搜索: %s（清除）"),
			 query);
	} else if(id >= ITEM_APP_ID_BASE && id < ITEM_APP_ID_BASE + ITEM_ID_RANGE) {
		char group[96];

		if(!find_group_by_id(tray, id, group, sizeof(group))) {
			known = 0;
		} else {
			char name[128];

			ellipsize(group, name, sizeof(name), 28);
			snprintf(label, sizeof(label), "%s (%d)", name,
				 group_window_count(tray, group, 1));
			enabled = group_window_count(tray, group, 1) > 0;
			submenu_entry = 1;
		}
	} else if(id == ITEM_SCREENS || id == ITEM_WINDOWS) {
		int count = id == ITEM_SCREENS ? tray->screen_count : tray->window_count;

		snprintf(label, sizeof(label), "%s",
			 id == ITEM_SCREENS ? tr("Screens", "屏幕") : tr("Windows", "窗口"));
		enabled = count > 0;
		submenu_entry = 1;
	} else if(id == ITEM_NO_SCREENS) {
		snprintf(label, sizeof(label), "%s", tr("No screen found", "未找到屏幕"));
		enabled = 0;
	} else if(id == ITEM_NO_WINDOWS) {
		snprintf(label, sizeof(label), "%s", tr("No window found", "未找到窗口"));
		enabled = 0;
	} else if(id >= ITEM_SCREEN_ID_BASE && id < ITEM_SCREEN_ID_BASE + ITEM_ID_RANGE) {
		int index;

		kind = SOURCE_KIND_SCREEN;
		index = find_source_index(tray, kind, id, &source_id);
		if(index < 0) {
			known = 0;
		} else {
			snprintf(raw, sizeof(raw), tr("Screen: %s", "屏幕: %s"),
				 tray->screens[index].label[0] ?
				 tray->screens[index].label : tray->screens[index].id);
			utf8_copy(label, sizeof(label), raw);
		}
	} else if(id >= ITEM_WINDOW_ID_BASE && id < ITEM_WINDOW_ID_BASE + ITEM_ID_RANGE) {
		int index;

		kind = SOURCE_KIND_WINDOW;
		index = find_source_index(tray, kind, id, &source_id);
		if(index < 0) {
			known = 0;
		} else {
			snprintf(raw, sizeof(raw), tr("Window: %s", "窗口: %s"),
				 tray->windows[index].label[0] ?
				 tray->windows[index].label : tray->windows[index].id);
			utf8_copy(label, sizeof(label), raw);
		}
	} else {
		known = 0;
	}

	if(!known)
		return;

	dbus_message_iter_open_container(array, DBUS_TYPE_STRUCT, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_INT32, &id);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &props);
	if(separator) {
		append_dict_entry_string(&props, "type", "separator");
	} else {
		append_dict_entry_string(&props, "label", label);
		append_dict_entry_bool(&props, "enabled", enabled);
		append_dict_entry_bool(&props, "visible", 1);
		if(submenu_entry)
			append_dict_entry_string(&props, "children-display", "submenu");
		if(kind != SOURCE_KIND_ANY) {
			append_dict_entry_string(&props, "toggle-type", "checkmark");
			append_dict_entry_int(&props, "toggle-state",
					      is_active_source(tray, kind, source_id));
		}
	}
	dbus_message_iter_close_container(&entry, &props);
	dbus_message_iter_close_container(array, &entry);
}

/*
 * dbusmenu: an empty id list means "properties of all items", not "none".
 */
static size_t collect_group_ids(struct tray *tray, DBusMessage *msg,
				int32_t *ids, size_t max) {
	DBusMessageIter in, in_ids;
	size_t count = 0;

	if(dbus_message_iter_init(msg, &in) &&
	   dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_ARRAY) {
		dbus_message_iter_recurse(&in, &in_ids);
		while(count < max && dbus_message_iter_get_arg_type(&in_ids) == DBUS_TYPE_INT32) {
			int32_t id = 0;

			dbus_message_iter_get_basic(&in_ids, &id);
			ids[count++] = id;
			dbus_message_iter_next(&in_ids);
		}
		if(count > 0)
			return count;
		/* An empty list means "all items" too. */
	}

	{
		static const int32_t fixed[] = {
			ITEM_STOP, ITEM_VISIBLE, ITEM_SEPARATOR, ITEM_SCREENS,
			ITEM_WINDOWS, ITEM_SEPARATOR_B, ITEM_PORTAL, ITEM_QUIT
		};
		size_t i;
		int have_group = 0;
		int32_t last_group = 0;

		for(i = 0; i < sizeof(fixed) / sizeof(fixed[0]) && count < max; i++)
			ids[count++] = fixed[i];

		for(i = 0; i < (size_t) tray->screen_count && count < max; i++)
			ids[count++] = source_menu_id(SOURCE_KIND_SCREEN, tray->screens[i].id);

		for(i = 0; i < (size_t) tray->window_count && count < max; i++) {
			int32_t group;

			ids[count++] = source_menu_id(SOURCE_KIND_WINDOW, tray->windows[i].id);
			group = group_menu_id(group_label_of(&tray->windows[i]));
			/* Windows are sorted by group, so equal groups are adjacent. */
			if(!have_group || group != last_group) {
				if(count < max)
					ids[count++] = group;
				last_group = group;
				have_group = 1;
			}
		}
	}

	return count;
}

static DBusHandlerResult handle_menu(DBusConnection *con, DBusMessage *msg, struct tray *tray) {
	if(dbus_message_is_method_call(msg, MENU_IFACE, "GetLayout")) {
		DBusMessageIter iter, args;
		DBusMessage *reply = dbus_message_new_method_return(msg);
		uint32_t revision = 1;
		int32_t parent = 0;

		if(dbus_message_iter_init(msg, &args) &&
		   dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_INT32)
			dbus_message_iter_get_basic(&args, &parent);

		if(!reply)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		refresh_sources(tray);
		dbus_message_iter_init_append(reply, &iter);
		revision = ++tray->menu_revision;
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &revision);
		append_item_struct(&iter, tray, parent);
		send_reply(con, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if(dbus_message_is_method_call(msg, MENU_IFACE, "GetGroupProperties")) {
		DBusMessageIter iter, array;
		DBusMessage *reply = dbus_message_new_method_return(msg);
		int32_t ids[1024];
		size_t count, i;

		refresh_sources(tray);

		if(!reply)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		dbus_message_iter_init_append(reply, &iter);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(ia{sv})", &array);

		count = collect_group_ids(tray, msg, ids, sizeof(ids) / sizeof(ids[0]));
		for(i = 0; i < count; i++)
			append_group_property(&array, tray, ids[i]);

		dbus_message_iter_close_container(&iter, &array);
		send_reply(con, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if(dbus_message_is_method_call(msg, MENU_IFACE, "Event")) {
		DBusMessageIter iter;
		int32_t id = 0;
		const char *event = NULL;

		if(dbus_message_iter_init(msg, &iter) &&
		   dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT32) {
			dbus_message_iter_get_basic(&iter, &id);
			if(dbus_message_iter_next(&iter) &&
			   dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING)
				dbus_message_iter_get_basic(&iter, &event);
		}

		if(event && !strcmp(event, "clicked"))
			handle_menu_event(tray, id);

		send_reply(con, dbus_message_new_method_return(msg));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if(dbus_message_is_method_call(msg, MENU_IFACE, "AboutToShow")) {
		DBusMessageIter args;
		int32_t parent = 0;
		dbus_bool_t need_update;
		DBusMessage *reply;

		if(dbus_message_iter_init(msg, &args) &&
		   dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_INT32)
			dbus_message_iter_get_basic(&args, &parent);
		/* The menu is flat: refresh the source list whenever it is opened. */
		refresh_sources(tray);
		need_update = TRUE;

		reply = dbus_message_new_method_return(msg);
		if(!reply)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &need_update, DBUS_TYPE_INVALID);
		send_reply(con, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/*
 * A SelectSource request names a source by the same syntax as --source.
 * Resolve it against the current list before acknowledging: otherwise the
 * CLI reports success and the switch silently fails later.
 */
static int source_spec_exists(struct tray *tray, const char *spec) {
	struct source_request req;
	int i;

	if(parse_source_spec(spec, &req) != 0)
		return 0;
	if(!tray->source_provider)
		return 0;	/* no direct capture: nothing can be switched */

	refresh_sources(tray);

	if(req.kind == SOURCE_KIND_SCREEN) {
		if(tray->screen_count == 0)
			return 0;
		if(!req.id[0])
			return 1;
		for(i = 0; i < tray->screen_count; i++) {
			if(!strcmp(tray->screens[i].id, req.id))
				return 1;
		}
		return 0;
	}

	if(tray->window_count == 0)
		return 0;
	if(!req.id[0])
		return 1;
	for(i = 0; i < tray->window_count; i++) {
		char pair[420];

		snprintf(pair, sizeof(pair), "%s|%s",
			 tray->windows[i].group, tray->windows[i].title);
		if(!strcmp(tray->windows[i].id, req.id) ||
		   !strcmp(pair, req.id) ||
		   !strcmp(tray->windows[i].title, req.id) ||
		   !strcmp(tray->windows[i].group, req.id))
			return 1;
	}

	return 0;
}

static DBusHandlerResult handle_control(DBusConnection *con, DBusMessage *msg, struct tray *tray) {
	if(dbus_message_is_method_call(msg, CONTROL_IFACE, "Reselect")) {
		const char *kind = NULL;

		if(dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &kind, DBUS_TYPE_INVALID)) {
			if(!strcmp(kind, "screen"))
				request_reselect(tray, SOURCE_KIND_SCREEN);
			else if(!strcmp(kind, "window"))
				request_reselect(tray, SOURCE_KIND_WINDOW);
			else
				request_reselect(tray, SOURCE_KIND_ANY);
		} else {
			request_reselect(tray, SOURCE_KIND_ANY);
		}
		send_reply(con, dbus_message_new_method_return(msg));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if(dbus_message_is_method_call(msg, CONTROL_IFACE, "SelectSource")) {
		const char *spec = NULL;

		if(dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &spec,
					 DBUS_TYPE_INVALID) && spec && spec[0]) {
			if(!source_spec_exists(tray, spec)) {
				send_reply(con, dbus_message_new_error(msg,
					"org.pingod.WaylandShareBridge.Error.NoSuchSource",
					"No such source"));
				return DBUS_HANDLER_RESULT_HANDLED;
			}
			utf8_copy(tray->selection_spec, sizeof(tray->selection_spec), spec);
			tray->selection_pending = 1;
		}
		send_reply(con, dbus_message_new_method_return(msg));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if(dbus_message_is_method_call(msg, CONTROL_IFACE, "SearchWindows")) {
		const char *query = NULL;

		if(dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &query,
					 DBUS_TYPE_INVALID) && query) {
			utf8_copy(tray->window_filter, sizeof(tray->window_filter), query);
			emit_layout_updated(tray, 0);
		}
		send_reply(con, dbus_message_new_method_return(msg));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if(dbus_message_is_method_call(msg, CONTROL_IFACE, "Quit")) {
		tray->quit_pending = 1;
		send_reply(con, dbus_message_new_method_return(msg));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if(dbus_message_is_method_call(msg, CONTROL_IFACE, "GetStatus")) {
		DBusMessage *reply = dbus_message_new_method_return(msg);
		char text[352];
		const char *value = text;

		if(!reply)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		/* The status file of the running instance ends with its version,
		 * so --status shows which build is answering. */
		snprintf(text, sizeof(text), "%s\nwayland-share-bridge %s",
			 tray->status[0] ? tray->status : "running",
			 tray->version[0] ? tray->version : "?");
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
		send_reply(con, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult tray_message(DBusConnection *con, DBusMessage *msg, void *user_data) {
	struct tray *tray = user_data;

	if(dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if(dbus_message_has_interface(msg, "org.freedesktop.DBus.Properties")) {
		DBusHandlerResult result = handle_properties(con, msg, tray);

		if(result != DBUS_HANDLER_RESULT_NOT_YET_HANDLED)
			return result;
	}

	if(dbus_message_has_interface(msg, SNI_IFACE))
		return handle_sni(con, msg, tray);
	if(dbus_message_has_interface(msg, MENU_IFACE))
		return handle_menu(con, msg, tray);
	if(dbus_message_has_interface(msg, CONTROL_IFACE))
		return handle_control(con, msg, tray);

	send_reply(con, dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_METHOD,
					       "No such method in wayland-share-bridge"));
	return DBUS_HANDLER_RESULT_HANDLED;
}

static const DBusObjectPathVTable sni_vtable = {
	.message_function = tray_message,
};

static const DBusObjectPathVTable menu_vtable = {
	.message_function = tray_message,
};

static const DBusObjectPathVTable control_vtable = {
	.message_function = tray_message,
};

static void register_sni(struct tray *tray);

/*
 * Tray hosts restart more often than the bridge.  When the
 * StatusNotifierWatcher reappears, register the icon again instead of
 * staying invisible until the bridge is restarted.
 */
static DBusHandlerResult watcher_filter(DBusConnection *con, DBusMessage *msg, void *user_data) {
	struct tray *tray = user_data;
	const char *name = NULL, *old_owner = NULL, *new_owner = NULL;

	(void) con;
	if(!dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if(!dbus_message_get_args(msg, NULL,
				  DBUS_TYPE_STRING, &name,
				  DBUS_TYPE_STRING, &old_owner,
				  DBUS_TYPE_STRING, &new_owner,
				  DBUS_TYPE_INVALID))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if(new_owner && new_owner[0] && (!old_owner || !old_owner[0]))
		register_sni(tray);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static void register_sni(struct tray *tray) {
	DBusError error;
	DBusMessage *msg, *reply;
	const char *argument;

	dbus_error_init(&error);
	msg = dbus_message_new_method_call(WATCHER_BUS, WATCHER_PATH, WATCHER_BUS,
					   "RegisterStatusNotifierItem");
	if(!msg)
		return;

	argument = tray->sni_name;
	dbus_message_append_args(msg, DBUS_TYPE_STRING, &argument, DBUS_TYPE_INVALID);
	reply = dbus_connection_send_with_reply_and_block(tray->con, msg, 1000, &error);
	dbus_message_unref(msg);

	if(!reply) {
		/* Some watchers want the object path instead of the bus name. */
		dbus_error_free(&error);
		dbus_error_init(&error);
		msg = dbus_message_new_method_call(WATCHER_BUS, WATCHER_PATH, WATCHER_BUS,
						   "RegisterStatusNotifierItem");
		if(!msg)
			return;
		argument = SNI_PATH;
		dbus_message_append_args(msg, DBUS_TYPE_STRING, &argument, DBUS_TYPE_INVALID);
		reply = dbus_connection_send_with_reply_and_block(tray->con, msg, 1000, &error);
		dbus_message_unref(msg);
	}

	if(!reply) {
		fprintf(stderr, "tray: the StatusNotifierWatcher did not accept the icon (%s); "
				"the tray menu will not be available\n",
			error.message ? error.message : "no reply");
		dbus_error_free(&error);
		return;
	}

	dbus_message_unref(reply);
	dbus_error_free(&error);
}

struct tray *thandler_init(const char *title, int want_tray) {
	struct tray *tray;
	DBusError error;
	char service[64];

	tray = calloc(1, sizeof(*tray));
	if(!tray)
		return NULL;

	tray->title = strdup(title ? title : "Wayland Share Bridge");
	if(!tray->title) {
		free(tray);
		return NULL;
	}
	tray->reselect_kind = SOURCE_KIND_ANY;

	dbus_error_init(&error);
	tray->con = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
	if(!tray->con) {
		fprintf(stderr, "tray: cannot connect to the session bus (%s)\n",
			error.message ? error.message : "no bus");
		dbus_error_free(&error);
		free(tray->title);
		free(tray);
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect(tray->con, FALSE);
	tray->fd = -1;
	if(!dbus_connection_get_unix_fd(tray->con, &tray->fd))
		tray->fd = -1;

	snprintf(service, sizeof(service), "org.kde.StatusNotifierItem-%d-1", (int) getpid());
	tray->sni_name = strdup(service);
	snprintf(service, sizeof(service), CONTROL_NAME_PREFIX "-%d-1", (int) getpid());
	tray->control_name = strdup(service);

	if(!tray->sni_name || !tray->control_name) {
		thandler_destroy(tray);
		return NULL;
	}

	/* The control interface exists even with --no-tray, so --status,
	 * --quit, --reselect and --source keep working. */
	dbus_bus_request_name(tray->con, tray->control_name, DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
	if(dbus_error_is_set(&error)) {
		fprintf(stderr, "tray: cannot own %s (%s)\n", tray->control_name, error.message);
		dbus_error_free(&error);
	}
	dbus_error_init(&error);
	if(!dbus_connection_register_object_path(tray->con, CONTROL_PATH, &control_vtable, tray))
		fprintf(stderr, "tray: cannot export " CONTROL_PATH "\n");

	if(want_tray) {
		dbus_bus_request_name(tray->con, tray->sni_name, DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
		dbus_error_free(&error);
		dbus_error_init(&error);

		if(!dbus_connection_register_object_path(tray->con, SNI_PATH, &sni_vtable, tray))
			fprintf(stderr, "tray: cannot export " SNI_PATH "\n");
		if(!dbus_connection_register_object_path(tray->con, MENU_PATH, &menu_vtable, tray))
			fprintf(stderr, "tray: cannot export " MENU_PATH "\n");

		tray->have_menu = 1;

		dbus_bus_add_match(tray->con,
			"type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
			"arg0='" WATCHER_BUS "'", &error);
		if(dbus_error_is_set(&error)) {
			fprintf(stderr, "tray: cannot watch the StatusNotifierWatcher (%s)\n",
				error.message);
			dbus_error_free(&error);
		}
		dbus_error_init(&error);
		dbus_connection_add_filter(tray->con, watcher_filter, tray, NULL);
		register_sni(tray);
	}

	snprintf(tray->status, sizeof(tray->status), "waiting for a source");

	return tray;
}

int thandler_fd(struct tray *tray) {
	return tray ? tray->fd : -1;
}

int thandler_routine(struct tray *tray) {
	if(!tray)
		return 0;

	if(!dbus_connection_read_write(tray->con, 0))
		return 1;
	while(dbus_connection_dispatch(tray->con) == DBUS_DISPATCH_DATA_REMAINS)
		;

	return 0;
}

int thandler_take_reselect(struct tray *tray, enum source_kind *kind) {
	if(!tray || !tray->reselect_pending)
		return -1;

	tray->reselect_pending = 0;
	if(kind)
		*kind = tray->reselect_kind;

	return 0;
}

int thandler_take_quit(struct tray *tray) {
	if(!tray || !tray->quit_pending)
		return 0;

	tray->quit_pending = 0;

	return 1;
}

void thandler_set_status(struct tray *tray, const char *status) {
	if(!tray || !status)
		return;

	utf8_copy(tray->status, sizeof(tray->status), status);
	emit_sni_signal(tray, "NewToolTip");
	emit_layout_updated(tray, 0);
}

void thandler_set_state(struct tray *tray, int sharing, int visible,
			enum source_kind kind, const char *id) {
	if(!tray)
		return;

	tray->sharing = sharing ? 1 : 0;
	tray->visible = visible ? 1 : 0;
	tray->active_kind = kind;
	utf8_copy(tray->active_id, sizeof(tray->active_id), id ? id : "");
	emit_sni_signal(tray, "NewStatus");
	emit_sni_signal(tray, "NewToolTip");
	emit_layout_updated(tray, 0);
}

int thandler_take_stop(struct tray *tray) {
	if(!tray || !tray->stop_pending)
		return 0;

	tray->stop_pending = 0;
	return 1;
}

int thandler_take_visibility(struct tray *tray) {
	if(!tray || !tray->visibility_pending)
		return 0;

	tray->visibility_pending = 0;
	return 1;
}

void thandler_set_source_provider(struct tray *tray,
				  int (*provider)(enum source_kind, struct capture_source **)) {
	if(!tray)
		return;

	tray->source_provider = provider;
	refresh_sources(tray);
	emit_layout_updated(tray, 0);
}

int thandler_take_selection(struct tray *tray, const char **spec) {
	if(!tray || !tray->selection_pending)
		return 0;

	tray->selection_pending = 0;
	if(spec)
		*spec = tray->selection_spec;
	return 1;
}

void thandler_destroy(struct tray *tray) {
	if(!tray)
		return;

	if(tray->con) {
		dbus_connection_remove_filter(tray->con, watcher_filter, tray);
		if(tray->have_menu) {
			dbus_connection_unregister_object_path(tray->con, SNI_PATH);
			dbus_connection_unregister_object_path(tray->con, MENU_PATH);
			if(tray->sni_name)
				dbus_bus_release_name(tray->con, tray->sni_name, NULL);
		}
		dbus_connection_unregister_object_path(tray->con, CONTROL_PATH);
		if(tray->control_name)
			dbus_bus_release_name(tray->con, tray->control_name, NULL);
		dbus_connection_close(tray->con);
		dbus_connection_unref(tray->con);
	}

	free(tray->screens);
	free(tray->windows);
	free(tray->title);
	free(tray->sni_name);
	free(tray->control_name);
	free(tray);
}

static char *find_control_name(DBusConnection *con) {
	DBusError error;
	DBusMessage *msg, *reply;
	DBusMessageIter iter, names;
	char *found = NULL;

	dbus_error_init(&error);
	msg = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
					   "org.freedesktop.DBus", "ListNames");
	if(!msg)
		return NULL;
	reply = dbus_connection_send_with_reply_and_block(con, msg, 2000, &error);
	dbus_message_unref(msg);
	if(!reply) {
		dbus_error_free(&error);
		return NULL;
	}

	if(dbus_message_iter_init(reply, &iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
		dbus_message_iter_recurse(&iter, &names);
		while(dbus_message_iter_get_arg_type(&names) == DBUS_TYPE_STRING) {
			const char *name = NULL;

			dbus_message_iter_get_basic(&names, &name);
			if(name && !strncmp(name, CONTROL_NAME_PREFIX "-", strlen(CONTROL_NAME_PREFIX "-"))) {
				found = strdup(name);
				break;
			}
			dbus_message_iter_next(&names);
		}
	}

	dbus_message_unref(reply);

	return found;
}

int tcontrol_is_running(void) {
	DBusError error;
	DBusConnection *con;
	char *name;
	int found = 0;

	dbus_error_init(&error);
	con = dbus_bus_get(DBUS_BUS_SESSION, &error);
	if(!con) {
		dbus_error_free(&error);
		return 0;
	}

	name = find_control_name(con);
	if(name) {
		found = 1;
		free(name);
	}

	dbus_connection_unref(con);

	return found;
}

int tcontrol_request(const char *method, const char *argument, char **reply_text) {
	DBusError error;
	DBusConnection *con;
	DBusMessage *msg, *reply;
	char *name;
	int result = 1;

	if(reply_text)
		*reply_text = NULL;

	dbus_error_init(&error);
	con = dbus_bus_get(DBUS_BUS_SESSION, &error);
	if(!con) {
		fprintf(stderr, "Cannot connect to the session bus: %s\n",
			error.message ? error.message : "no bus");
		dbus_error_free(&error);
		return 1;
	}

	name = find_control_name(con);
	if(!name) {
		fprintf(stderr, "No running wayland-share-bridge instance found\n");
		dbus_connection_unref(con);
		return 1;
	}

	msg = dbus_message_new_method_call(name, CONTROL_PATH, CONTROL_IFACE, method);
	if(msg && argument) {
		dbus_message_append_args(msg, DBUS_TYPE_STRING, &argument, DBUS_TYPE_INVALID);
	}

	if(msg) {
		reply = dbus_connection_send_with_reply_and_block(con, msg, 5000, &error);
		dbus_message_unref(msg);
		if(!reply) {
			fprintf(stderr, "%s failed: %s\n", method,
				error.message ? error.message : "no reply");
			dbus_error_free(&error);
		} else {
			if(reply_text) {
				const char *text = NULL;

				if(dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &text,
							 DBUS_TYPE_INVALID))
					*reply_text = strdup(text);
			}
			dbus_message_unref(reply);
			result = 0;
		}
	}

	free(name);
	dbus_connection_unref(con);

	return result;
}

int tcontrol_select_source(const char *spec) {
	if(!spec || !spec[0])
		return 1;

	return tcontrol_request("SelectSource", spec, NULL);
}

void thandler_set_version(struct tray *tray, const char *version) {
	if(!tray)
		return;
	snprintf(tray->version, sizeof(tray->version), "%s",
		 version ? version : "");
	emit_layout_updated(tray, 0);
}

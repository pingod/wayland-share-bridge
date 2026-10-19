/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * wayland-share-bridge - make Wayland screen content shareable from X11 apps.
 *
 * The bridge captures a Wayland screen or window - either directly through
 * the compositor's image copy capture protocols or, as a fallback, through
 * the desktop portal - and paints the frames into a normal X11 window.  X11
 * clients (WPS Office, Zoom, Feishu, Electron applications, ...) can share
 * that window like any other, even though they cannot see native Wayland
 * windows at all.
 */

#include <fcntl.h>
#include <getopt.h>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "dbus_handler.h"
#include "pipewire_handler.h"
#include "tray_handler.h"
#include "wayland_capture.h"
#include "x11_handler.h"

#define PROGRAM_NAME "wayland-share-bridge"
#define PROGRAM_VERSION "1.0.0"

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t reselect_requested = 0;
static volatile sig_atomic_t tray_reselect = 0;
static enum source_kind tray_reselect_kind = SOURCE_KIND_ANY;
static struct tray *g_tray = NULL;
static struct xdata *g_xdata = NULL;
static char g_state_file[4096] = "";
static int g_remember = 1;
static enum source_kind g_last_direct_kind = SOURCE_KIND_ANY;
static char g_last_direct_id[192] = "";
static int g_visible = 0;
static int g_sharing = 0;
static enum source_kind g_share_kind = SOURCE_KIND_ANY;
static char g_share_label[192] = "";

/*
 * Hyprland ignores _NET_WM_WINDOW_OPACITY for XWayland windows, so the window
 * is hidden with a window rule instead.  Re-evaluating a rule with the same
 * name replaces it, which makes runtime toggling possible.
 */
static void hyprland_set_visibility(int visible) {
	pid_t pid;
	char rule[640];

	if(!getenv("HYPRLAND_INSTANCE_SIGNATURE"))
		return;

	snprintf(rule, sizeof(rule),
		 "hl.window_rule({ name = \"wayland-share-bridge-hidden\", "
		 "match = { class = \"^wayland-share-bridge$\" }, opacity = %s, "
		 "float = true, no_blur = true })",
		 visible ? "1.0" : "0.0");

	if(visible) {
		char raise_rule[512];

		snprintf(raise_rule, sizeof(raise_rule),
			 "hl.dispatch(hl.dsp.window.alter_zorder({ window = "
			 "\"class:^wayland-share-bridge$\", mode = \"top\" }))");
		pid = fork();
		if(pid == 0) {
			int null_fd = open("/dev/null", O_WRONLY);

			if(null_fd >= 0) {
				dup2(null_fd, STDOUT_FILENO);
				dup2(null_fd, STDERR_FILENO);
				close(null_fd);
			}
			execlp("hyprctl", "hyprctl", "eval", raise_rule,
			       (char *) NULL);
			_exit(127);
		} else if(pid > 0) {
			waitpid(pid, NULL, 0);
		}
	}

	pid = fork();
	if(pid == 0) {
		int null_fd = open("/dev/null", O_WRONLY);

		if(null_fd >= 0) {
			dup2(null_fd, STDOUT_FILENO);
			dup2(null_fd, STDERR_FILENO);
			close(null_fd);
		}
		execlp("hyprctl", "hyprctl", "eval", rule, (char *) NULL);
		_exit(127);
	} else if(pid > 0) {
		waitpid(pid, NULL, 0);
	}
}

static void set_window_visibility(int visible) {
	g_visible = visible ? 1 : 0;
	if(g_xdata)
		xhandler_set_visible(g_xdata, g_visible);
	hyprland_set_visibility(g_visible);
	if(g_tray)
		thandler_set_state(g_tray, g_sharing, g_visible, g_share_kind, g_share_label);
}

static void update_tray_state(void) {
	if(g_tray)
		thandler_set_state(g_tray, g_sharing, g_visible, g_share_kind, g_share_label);
}

static void set_idle_status(const char *text) {
	char label[288];

	if(!g_tray)
		return;
	if(wlcap_active() && wlcap_current_label()[0]) {
		snprintf(label, sizeof(label), "Sharing: %s", wlcap_current_label());
		thandler_set_status(g_tray, label);
		return;
	}
	thandler_set_status(g_tray, text ? text : "Not sharing (pick a screen or window from this menu)");
}

static void set_sharing_status(const char *label) {
	char text[288];

	if(!g_tray)
		return;
	if(label && label[0]) {
		snprintf(text, sizeof(text), "Sharing: %s", label);
		thandler_set_status(g_tray, text);
	} else {
		set_idle_status(NULL);
	}
}

static void tick(void *user_data) {
	enum source_kind kind;

	(void) user_data;

	if(!g_tray)
		return;

	if(thandler_routine(g_tray)) {
		fprintf(stderr, "Lost the tray D-Bus connection; continuing without the tray\n");
		thandler_destroy(g_tray);
		g_tray = NULL;
		return;
	}

	if(thandler_take_quit(g_tray)) {
		running = 0;
		return;
	}

	if(thandler_take_reselect(g_tray, &kind) == 0) {
		tray_reselect_kind = kind;
		tray_reselect = 1;
	}
}

static void on_terminate(int signum) {
	(void) signum;
	running = 0;
}

static void on_hangup(int signum) {
	(void) signum;
	reselect_requested = 1;
}

static void usage(FILE *out) {
	fprintf(out,
		"Usage: " PROGRAM_NAME " [options]\n"
		"       " PROGRAM_NAME " --list-sources\n"
		"       " PROGRAM_NAME " --reselect[=KIND]\n"
		"       " PROGRAM_NAME " --status\n"
		"       " PROGRAM_NAME " --quit\n"
		"\n"
		"Share Wayland screens or windows with X11 applications.\n"
		"The bridge opens an invisible X11 window (\"Wayland Share Bridge\")\n"
		"and mirrors the chosen screen or window into it.  Pick the source\n"
		"from the tray menu; the application you are sharing from only has to\n"
		"share the \"Wayland Share Bridge\" window.\n"
		"\n"
		"Options:\n"
		"  -c, --cursor=MODE     cursor mode for the portal backend: hidden,\n"
		"                        embedded, metadata (default: embedded)\n"
		"  -k, --source-kind=KIND\n"
		"                        fallback source kind for the portal backend\n"
		"                        (auto, any, screen, window; default: auto)\n"
		"  -S, --source=SPEC     capture SPEC right away: screen:NAME or\n"
		"                        window:APP_ID or window:APP_ID|TITLE\n"
		"  -L, --list-sources    list the screens and windows that can be shared\n"
		"      --portal          always use the desktop portal (system picker)\n"
		"  -r, --remember        remember the last source (default)\n"
		"  -n, --no-remember     do not remember the source\n"
		"  -s, --show            show the bridge window (debugging)\n"
		"  -t, --title=TEXT      window title (default: Wayland Share Bridge)\n"
		"  -f, --token-file=PATH restore token file used by the portal backend\n"
		"                        (default: $XDG_STATE_HOME/" PROGRAM_NAME "/restore_token)\n"
		"      --no-tray         do not create the tray icon\n"
		"      --allow-multiple  allow more than one running instance\n"
		"  -h, --help            show this help\n"
		"  -V, --version         show the version\n"
		"\n"
		"Controlling a running instance:\n"
		"      --reselect[=KIND] pick a source through the portal picker\n"
		"      --status          print what the running instance is sharing\n"
		"      --quit            stop the running instance\n"
		"\n"
		"Signals:\n"
		"  SIGHUP                ask the running instance for a new source\n"
		"  SIGINT, SIGTERM       stop the bridge\n"
		"\n"
		"Examples:\n"
		"  " PROGRAM_NAME "\n"
		"  " PROGRAM_NAME " --list-sources\n"
		"  " PROGRAM_NAME " --source=screen:DP-2\n"
		"  " PROGRAM_NAME " --source=window:org.mozilla.firefox\n"
		"  " PROGRAM_NAME " --portal --cursor=hidden\n");
}

static char *join_path(const char *directory, const char *name) {
	size_t length = strlen(directory) + strlen(name) + 2;
	char *path = malloc(length);

	if(path)
		snprintf(path, length, "%s/%s", directory, name);

	return path;
}

static char *state_dir(void) {
	const char *state_home = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");

	if(state_home && state_home[0])
		return join_path(state_home, PROGRAM_NAME);

	if(home && home[0])
		return join_path(home, ".local/state/" PROGRAM_NAME);

	return NULL;
}

static char *dirname_dup(const char *path) {
	const char *slash = strrchr(path, '/');
	size_t length;

	if(!slash)
		return strdup(".");
	length = (size_t) (slash - path);
	if(length == 0)
		length = 1;
	{
		char *copy = malloc(length + 1);

		if(copy) {
			memcpy(copy, path, length);
			copy[length] = '\0';
		}
		return copy;
	}
}

static int parse_cursor_mode(const char *value, enum cursor_mode *out) {
	if(!strcmp(value, "hidden") || !strcmp(value, "none")) {
		*out = CURSOR_HIDDEN;
		return 0;
	}
	if(!strcmp(value, "embedded") || !strcmp(value, "always")) {
		*out = CURSOR_EMBEDDED;
		return 0;
	}
	if(!strcmp(value, "metadata") || !strcmp(value, "on-demand")) {
		*out = CURSOR_METADATA;
		return 0;
	}

	return -1;
}

static int parse_source_kind(const char *value, enum source_kind *out) {
	if(!strcmp(value, "auto")) {
		*out = SOURCE_KIND_AUTO;
		return 0;
	}
	if(!strcmp(value, "any")) {
		*out = SOURCE_KIND_ANY;
		return 0;
	}
	if(!strcmp(value, "screen") || !strcmp(value, "monitor")) {
		*out = SOURCE_KIND_SCREEN;
		return 0;
	}
	if(!strcmp(value, "window")) {
		*out = SOURCE_KIND_WINDOW;
		return 0;
	}

	return -1;
}

/* "screen:NAME" or "window:APP_ID" / "window:APP_ID|TITLE" */
static int parse_source_spec(const char *spec, enum source_kind *kind, const char **id) {
	if(!strncmp(spec, "screen:", 7)) {
		*kind = SOURCE_KIND_SCREEN;
		*id = spec + 7;
		return 0;
	}
	if(!strncmp(spec, "window:", 7)) {
		*kind = SOURCE_KIND_WINDOW;
		*id = spec + 7;
		return 0;
	}

	return -1;
}

static void remember_source(enum source_kind kind, const char *id) {
	FILE *file;

	if(!g_remember || !g_state_file[0])
		return;
	file = fopen(g_state_file, "w");
	if(!file)
		return;
	fprintf(file, "source=%s:%s\n", kind == SOURCE_KIND_SCREEN ? "screen" : "window", id);
	fclose(file);
}

static int load_remembered_source(enum source_kind *kind, char *id, size_t size) {
	FILE *file;
	char line[512];
	int found = 0;

	if(!g_state_file[0])
		return 0;
	file = fopen(g_state_file, "r");
	if(!file)
		return 0;
	while(fgets(line, sizeof(line), file)) {
		char *value = line;

		if(strncmp(line, "source=", 7) != 0)
			continue;
		value = line + 7;
		value[strcspn(value, "\n")] = '\0';
		if(!strncmp(value, "screen:", 7)) {
			*kind = SOURCE_KIND_SCREEN;
			snprintf(id, size, "%s", value + 7);
			found = 1;
		} else if(!strncmp(value, "window:", 7)) {
			*kind = SOURCE_KIND_WINDOW;
			snprintf(id, size, "%s", value + 7);
			found = 1;
		}
	}
	fclose(file);
	return found;
}

static int start_direct(enum source_kind kind, const char *id) {
	if(wlcap_start(kind, id, g_xdata) != 0) {
		set_idle_status(NULL);
		fprintf(stderr, "Cannot share that source; pick another one from the tray menu.\n");
		return -1;
	}
	remember_source(kind, id);
	g_last_direct_kind = kind;
	snprintf(g_last_direct_id, sizeof(g_last_direct_id), "%s", id);
	g_sharing = 1;
	g_share_kind = kind;
	snprintf(g_share_label, sizeof(g_share_label), "%s", id);
	update_tray_state();
	set_sharing_status(wlcap_current_label());
	fprintf(stderr, "Sharing %s (window class %s).\n",
		wlcap_current_label(), "wayland-share-bridge");
	return 0;
}

static int start_portal(struct options *options, struct ddata *ddata,
			struct pdata **pdata, enum source_kind kind) {
	(void) options;

	if(*pdata) {
		phandler_destroy(*pdata);
		*pdata = NULL;
	}
	dhandler_close_session(ddata);

	dhandler_set_source_kind(ddata, kind);

	if(dhandler_create_session(ddata) ||
	   dhandler_select_sources(ddata) ||
	   dhandler_start(ddata) ||
	   dhandler_open_pipewire_remote(ddata)) {
		dhandler_close_session(ddata);
		set_idle_status(NULL);
		fprintf(stderr, "No source selected; waiting for a new request.\n");
		return -1;
	}

	*pdata = phandler_init(ddata->pipewire_fd, ddata->node_id, g_xdata);
	if(!*pdata) {
		dhandler_close_session(ddata);
		set_idle_status(wlcap_active() ? NULL : "Portal capture failed; pick another source");
		fprintf(stderr, "The portal capture could not be started; pick another source.\n");
		return -1;
	}

	phandler_clear_connection_lost();
	g_sharing = 2;
	g_share_kind = ddata->source.type == 1 ? SOURCE_KIND_SCREEN :
		       ddata->source.type == 2 ? SOURCE_KIND_WINDOW : SOURCE_KIND_ANY;
	g_share_label[0] = '\0';
	update_tray_state();
	set_sharing_status(dhandler_describe_source(ddata));
	fprintf(stderr, "Bridge is running. Share the \"%s\" window from your application.\n",
		"Wayland Share Bridge");
	return 0;
}

static void stop_sharing(struct ddata **ddata_ptr, struct pdata **pdata_ptr,
			 int *active, int *direct_active) {
	if(*pdata_ptr) {
		phandler_destroy(*pdata_ptr);
		*pdata_ptr = NULL;
	}
	if(*ddata_ptr)
		dhandler_close_session(*ddata_ptr);
	wlcap_stop();
	*active = 0;
	*direct_active = 0;
	g_sharing = 0;
	g_share_kind = SOURCE_KIND_ANY;
	g_share_label[0] = '\0';
	update_tray_state();
	set_idle_status(NULL);
	fprintf(stderr, "Sharing stopped.\n");
}

int main(int argc, char *argv[]) {
	struct options options = {
		.title = "Wayland Share Bridge",
		.class_name = PROGRAM_NAME,
		.cursor = CURSOR_EMBEDDED,
		.remember = 1,
		.show = 0,
		.tray = 1,
		.source_kind = SOURCE_KIND_AUTO,
		.token_file = NULL,
	};
	enum {
		OPT_NO_TRAY = 1000,
		OPT_ALLOW_MULTIPLE,
		OPT_RESELECT,
		OPT_STATUS,
		OPT_QUIT,
		OPT_PORTAL,
		OPT_LIST_SOURCES,
	};
	char *owned_state_dir = NULL;
	char *owned_token_path = NULL;
	char *owned_source_dir = NULL;
	const char *control_method = NULL;
	const char *control_argument = NULL;
	const char *initial_source = NULL;
	struct ddata *ddata = NULL;
	struct pdata *pdata = NULL;
	int exit_code = 1;
	int active = 0;
	int direct = 0;
	int direct_active = 0;
	int list_sources = 0;
	int force_portal = 0;
	int attempt = 0;
	int allow_multiple = 0;
	int opt;

	static const struct option long_options[] = {
		{ "cursor", required_argument, NULL, 'c' },
		{ "source-kind", required_argument, NULL, 'k' },
		{ "source", required_argument, NULL, 'S' },
		{ "list-sources", no_argument, NULL, 'L' },
		{ "portal", no_argument, NULL, OPT_PORTAL },
		{ "remember", no_argument, NULL, 'r' },
		{ "no-remember", no_argument, NULL, 'n' },
		{ "show", no_argument, NULL, 's' },
		{ "title", required_argument, NULL, 't' },
		{ "token-file", required_argument, NULL, 'f' },
		{ "no-tray", no_argument, NULL, OPT_NO_TRAY },
		{ "allow-multiple", no_argument, NULL, OPT_ALLOW_MULTIPLE },
		{ "reselect", optional_argument, NULL, OPT_RESELECT },
		{ "status", no_argument, NULL, OPT_STATUS },
		{ "quit", no_argument, NULL, OPT_QUIT },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ NULL, 0, NULL, 0 },
	};

	pw_init(&argc, &argv);

	while((opt = getopt_long(argc, argv, "c:k:S:Lrnst:f:hV", long_options, NULL)) != -1) {
		switch(opt) {
		case 'c':
			if(parse_cursor_mode(optarg, &options.cursor) != 0) {
				fprintf(stderr, "Unknown cursor mode '%s' (expected hidden, embedded or metadata)\n", optarg);
				goto out;
			}
			break;
		case 'k':
			if(parse_source_kind(optarg, &options.source_kind) != 0) {
				fprintf(stderr, "Unknown source kind '%s' (expected auto, any, screen or window)\n", optarg);
				goto out;
			}
			break;
		case 'S':
			initial_source = optarg;
			break;
		case 'L':
			list_sources = 1;
			break;
		case OPT_PORTAL:
			force_portal = 1;
			break;
		case 'r':
			options.remember = 1;
			break;
		case 'n':
			options.remember = 0;
			break;
		case 's':
			options.show = 1;
			break;
		case 't':
			options.title = optarg;
			break;
		case 'f':
			options.token_file = optarg;
			break;
		case OPT_NO_TRAY:
			options.tray = 0;
			break;
		case OPT_ALLOW_MULTIPLE:
			allow_multiple = 1;
			break;
		case OPT_RESELECT:
			control_method = "Reselect";
			control_argument = optarg;
			break;
		case OPT_STATUS:
			control_method = "GetStatus";
			break;
		case OPT_QUIT:
			control_method = "Quit";
			break;
		case 'h':
			usage(stdout);
			exit_code = 0;
			goto out;
		case 'V':
			printf("%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
			exit_code = 0;
			goto out;
		default:
			usage(stderr);
			goto out;
		}
	}

	if(control_method) {
		char *text = NULL;
		int result = tcontrol_request(control_method, control_argument, &text);

		if(text) {
			printf("%s\n", text);
			free(text);
		}

		exit_code = result;
		goto out;
	}

	/* Direct capture: no portal, no picker, sources are listed by us. */
	if(!force_portal) {
		if(wlcap_init() == 0) {
			direct = 1;
		} else {
			fprintf(stderr, "Falling back to the desktop portal for screen capture.\n");
		}
	}

	if(list_sources) {
		struct capture_source *list = NULL;
		int count = direct ? wlcap_list(SOURCE_KIND_ANY, &list) : 0;
		int i;

		for(i = 0; i < count; i++)
			printf("%s:%s\t%s\n",
			       list[i].kind == SOURCE_KIND_SCREEN ? "screen" : "window",
			       list[i].id, list[i].label);
		free(list);
		if(!direct)
			fprintf(stderr, "Direct capture is not available on this compositor.\n");
		exit_code = 0;
		goto out;
	}

	if(!allow_multiple && tcontrol_is_running()) {
		fprintf(stderr,
			"Another wayland-share-bridge instance is already running.\n"
			"Use --reselect to pick a new source, --status to inspect it, or\n"
			"--allow-multiple to start an additional bridge.\n");
		goto out;
	}

	if(!options.token_file && options.remember) {
		owned_state_dir = state_dir();
		if(owned_state_dir) {
			owned_token_path = join_path(owned_state_dir, "restore_token");
			if(!owned_token_path) {
				fprintf(stderr, "warning: cannot allocate the state path, not remembering the source\n");
				options.remember = 0;
			} else {
				options.token_file = owned_token_path;
			}
		} else {
			fprintf(stderr, "warning: cannot determine the state directory, not remembering the source\n");
			options.remember = 0;
		}
	}

	if(options.token_file) {
		owned_source_dir = dirname_dup(options.token_file);
		if(owned_source_dir) {
			char *state_file = join_path(owned_source_dir, "state");

			if(state_file) {
				snprintf(g_state_file, sizeof(g_state_file), "%s", state_file);
				free(state_file);
			}
		}
	}
	g_remember = options.remember;
	g_visible = options.show ? 1 : 0;

	g_xdata = xhandler_init(&options);
	if(!g_xdata)
		goto out;

	/* Hide the mirror window on compositors where the X11 property is ignored. */
	hyprland_set_visibility(g_visible);

	if(options.tray)
		g_tray = thandler_init(options.title);
	if(g_tray) {
		thandler_set_version(g_tray, PROGRAM_VERSION);
		set_idle_status(direct ? NULL : "Waiting for the system portal…");
		if(direct)
			thandler_set_source_provider(g_tray, wlcap_list);
		update_tray_state();
	}

	signal(SIGINT, on_terminate);
	signal(SIGTERM, on_terminate);
	signal(SIGHUP, on_hangup);

	fprintf(stderr, "Bridge window: \"%s\" (class %s)\n", options.title, options.class_name);
	if(direct)
		fprintf(stderr, "Direct capture enabled; use the tray menu to pick a screen or window.\n");
	fprintf(stderr, "Use --reselect, --status or the tray icon to control a running instance.\n");

	if(direct) {
		enum source_kind kind;
		char source_id[192];

		if(initial_source) {
			const char *id = NULL;

			if(parse_source_spec(initial_source, &kind, &id) == 0) {
				if(start_direct(kind, id) == 0)
					direct_active = 1;
			} else {
				fprintf(stderr, "Invalid --source '%s' (expected screen:NAME or window:APP_ID)\n",
					initial_source);
			}
		} else if(load_remembered_source(&kind, source_id, sizeof(source_id))) {
			if(start_direct(kind, source_id) == 0)
				direct_active = 1;
		}

		if(!direct_active && !ddata) {
			/* The portal is only created on demand. */
			attempt = 0;
		}
	}

	/* The portal backend needs its session objects even if unused. */
	if((!direct || force_portal) && !ddata) {
		ddata = dhandler_init(options.cursor, options.source_kind,
			options.remember && options.token_file != NULL,
			options.token_file);
		if(!ddata)
			goto out;
		dhandler_set_tick(ddata, tick, NULL);
		dhandler_set_stop_flag(ddata, &running);
		if(!direct)
			attempt = 1;
	}

	while(running) {
		struct pollfd polls[4];
		nfds_t count = 2;

		if(attempt) {
			enum source_kind kind = options.source_kind;

			attempt = 0;
			if(ddata) {
				if(thandler_take_reselect(g_tray, &kind) == 0)
					kind = tray_reselect_kind;
				if(reselect_requested) {
					reselect_requested = 0;
					kind = SOURCE_KIND_ANY;
				}
				wlcap_stop();
				direct_active = 0;
				active = 0;
				if(start_portal(&options, ddata, &pdata, kind) == 0) {
					active = 1;
				} else if(direct && g_last_direct_id[0] &&
					  start_direct(g_last_direct_kind, g_last_direct_id) == 0) {
					direct_active = 1;
				}
			}
		}

		polls[0].fd = g_xdata->fd;
		polls[0].events = POLLIN;
		polls[0].revents = 0;
		polls[1].fd = g_tray ? thandler_fd(g_tray) : -1;
		polls[1].events = POLLIN;
		polls[1].revents = 0;
		count = 2;

		if(ddata) {
			polls[count].fd = ddata->fd;
			polls[count].events = POLLIN;
			polls[count].revents = 0;
			count++;
		}

		if(poll(polls, count, direct ? 16 : 200) < 0)
			break;

		/* Keep the tray responsive even when no fd fires. */
		tick(NULL);

		if(g_tray && g_sharing && thandler_take_stop(g_tray))
			stop_sharing(&ddata, &pdata, &active, &direct_active);
		if(g_tray && g_sharing && thandler_take_visibility(g_tray))
			set_window_visibility(!g_visible);

		if(direct) {
			enum source_kind kind;
			const char *id = NULL;

			if(wlcap_dispatch() != 0)
				fprintf(stderr, "capture: lost the Wayland connection\n");
			if(thandler_take_selection(g_tray, &kind, &id)) {
				char copy[192];

				snprintf(copy, sizeof(copy), "%s", id);
				if(active && pdata) {
					phandler_destroy(pdata);
					pdata = NULL;
					dhandler_close_session(ddata);
					active = 0;
				}
				direct_active = start_direct(kind, copy) == 0;
			}
			if(direct_active && !wlcap_active()) {
				fprintf(stderr, "capture: the source closed; pick another one.\n");
				wlcap_stop();
				direct_active = 0;
				g_sharing = 0;
				g_share_kind = SOURCE_KIND_ANY;
				g_share_label[0] = '\0';
				update_tray_state();
				set_idle_status("The source was closed; pick another one");
			}
			if(direct_active)
				wlcap_tick();
			if(reselect_requested) {
				reselect_requested = 0;
				if(!ddata) {
					ddata = dhandler_init(options.cursor, options.source_kind,
						options.remember && options.token_file != NULL,
						options.token_file);
					if(ddata) {
						dhandler_set_tick(ddata, tick, NULL);
						dhandler_set_stop_flag(ddata, &running);
					}
				}
				if(ddata)
					attempt = 1;
			}
		}

		if(polls[0].revents & POLLIN) {
			if(xhandler_routine(g_xdata)) {
				fprintf(stderr, "Lost the X11 connection\n");
				break;
			}
		}
		if((polls[0].revents & (POLLERR | POLLHUP)) && !(polls[0].revents & POLLIN)) {
			fprintf(stderr, "Lost the X11 connection\n");
			break;
		}

		if(ddata && (polls[count - 1].revents & POLLIN)) {
			if(dhandler_routine(ddata)) {
				fprintf(stderr, "Lost the D-Bus connection to the portal\n");
				break;
			}
		}

		if(active && phandler_connection_lost()) {
			fprintf(stderr, "The PipeWire stream was closed; waiting for a new source.\n");
			phandler_destroy(pdata);
			pdata = NULL;
			dhandler_close_session(ddata);
			active = 0;
			set_idle_status("The stream was closed; pick another one");
		}

		if(tray_reselect) {
			tray_reselect = 0;
			if(!ddata) {
				ddata = dhandler_init(options.cursor, options.source_kind,
					options.remember && options.token_file != NULL,
					options.token_file);
				if(ddata) {
					dhandler_set_tick(ddata, tick, NULL);
					dhandler_set_stop_flag(ddata, &running);
				}
			}
			if(ddata) {
				dhandler_set_source_kind(ddata, tray_reselect_kind);
				attempt = 1;
			}
		}
	}

	if(active && pdata) {
		phandler_destroy(pdata);
		pdata = NULL;
	}
	if(ddata)
		dhandler_close_session(ddata);
	wlcap_stop();
	exit_code = 0;

out:
	if(g_tray) {
		thandler_destroy(g_tray);
		g_tray = NULL;
	}
	if(pdata)
		phandler_destroy(pdata);
	if(ddata)
		dhandler_destroy(ddata);
	if(direct)
		wlcap_destroy();
	if(g_xdata)
		xhandler_destroy(g_xdata);
	free(owned_token_path);
	free(owned_state_dir);
	free(owned_source_dir);
	pw_deinit();

	return exit_code;
}

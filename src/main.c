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

#include <errno.h>
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
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "dbus_handler.h"
#include "i18n.h"
#include "pipewire_handler.h"
#include "source_spec.h"
#include "state_file.h"
#include "tray_handler.h"
#include "version.h"
#include "util.h"
#include "wayland_capture.h"
#include "x11_handler.h"

#define PROGRAM_NAME "wayland-share-bridge"

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t reselect_requested = 0;
static struct tray *g_tray = NULL;
static struct xdata *g_xdata = NULL;
static char g_state_file[4096] = "";
static int g_remember = 1;
static int g_direct = 0;
static int g_direct_stalled = 0;
static char g_last_spec[256] = "";
static int g_visible = 0;
static int g_sharing = 0;
static enum source_kind g_share_kind = SOURCE_KIND_ANY;
static char g_share_label[192] = "";
static const char *g_tray_title = NULL;
static const char *g_class_name = PROGRAM_NAME;
static int g_want_tray = 1;
static uint32_t g_tray_retry_ms;

static void clear_sharing_state(void);

static uint32_t now_ms(void) {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t) (ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

/*
 * Hyprland ignores _NET_WM_WINDOW_OPACITY for XWayland windows, so the window
 * is hidden with a window rule instead.  Re-evaluating a rule with the same
 * name replaces it, which makes runtime toggling possible.
 */
/*
 * Runs hyprctl synchronously; the rule is small and this only happens when
 * the window is shown or hidden.  Returns 0 when hyprctl exited cleanly.
 */
static int run_hyprctl(char *const argv[]) {
	pid_t pid;
	int status;

	pid = fork();
	if(pid == 0) {
		int null_fd = open("/dev/null", O_WRONLY);

		if(null_fd >= 0) {
			dup2(null_fd, STDOUT_FILENO);
			dup2(null_fd, STDERR_FILENO);
			close(null_fd);
		}
		execvp("hyprctl", argv);
		_exit(127);
	}
	if(pid < 0)
		return -1;
	if(waitpid(pid, &status, 0) < 0)
		return -1;

	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/*
 * Fire-and-forget variant for cosmetic calls: the z-order change must not
 * stall the frame loop, so the child is not waited for.  reap_children()
 * collects it from the event loop.
 */
static int run_hyprctl_async(char *const argv[]) {
	pid_t pid;

	pid = fork();
	if(pid == 0) {
		int null_fd = open("/dev/null", O_WRONLY);

		if(null_fd >= 0) {
			dup2(null_fd, STDOUT_FILENO);
			dup2(null_fd, STDERR_FILENO);
			close(null_fd);
		}
		execvp("hyprctl", argv);
		_exit(127);
	}

	return pid > 0 ? 0 : -1;
}

static void reap_children(void) {
	int status;

	while(waitpid(-1, &status, WNOHANG) > 0)
		;
}

/*
 * Hyprland ignores _NET_WM_WINDOW_OPACITY for XWayland windows, so the
 * window is hidden with a window rule instead.  Re-evaluating a rule with
 * the same name replaces it, which makes runtime toggling possible.  Older
 * Hyprland releases only have the legacy parser, hence the fallback.
 */
static void hyprland_set_visibility(int visible) {
	char rule[640];
	char opacity[96];
	char *eval_argv[4];
	char *rule_argv[5];
	char *raise_argv[4];

	if(!getenv("HYPRLAND_INSTANCE_SIGNATURE"))
		return;

	snprintf(rule, sizeof(rule),
		 "hl.window_rule({ name = \"wayland-share-bridge-hidden\", "
		 "match = { class = \"^wayland-share-bridge$\" }, opacity = %s, "
		 "float = true, no_blur = true, no_focus = true })",
		 visible ? "1.0" : "0.0");

	eval_argv[0] = "hyprctl";
	eval_argv[1] = "eval";
	eval_argv[2] = rule;
	eval_argv[3] = NULL;

	if(run_hyprctl(eval_argv) != 0) {
		/* The legacy parser has no named rules, so the newest rule wins.
		 * The launcher adds the static (float/no_blur) rules. */
		snprintf(opacity, sizeof(opacity),
			 "opacity %s, class:^wayland-share-bridge$",
			 visible ? "1.0" : "0.0");
		rule_argv[0] = "hyprctl";
		rule_argv[1] = "keyword";
		rule_argv[2] = "windowrulev2";
		rule_argv[3] = opacity;
		rule_argv[4] = NULL;
		if(run_hyprctl(rule_argv) != 0)
			fprintf(stderr, "warning: cannot update the Hyprland window rule\n");
	}

	if(visible) {
		char raise_rule[512];

		snprintf(raise_rule, sizeof(raise_rule),
			 "hl.dispatch(hl.dsp.window.alter_zorder({ window = "
			 "\"class:^wayland-share-bridge$\", mode = \"top\" }))");
		raise_argv[0] = "hyprctl";
		raise_argv[1] = "eval";
		raise_argv[2] = raise_rule;
		raise_argv[3] = NULL;
		/* Purely cosmetic: never block the frame loop on it. */
		run_hyprctl_async(raise_argv);
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
	thandler_set_status(g_tray, text ? text :
			tr("Not sharing (pick a screen or window from this menu)",
			   "未共享（请从托盘子菜单选择屏幕或窗口）"));
}

static void set_sharing_status(const char *label) {
	char text[288];

	if(!g_tray)
		return;
	if(label && label[0]) {
		snprintf(text, sizeof(text), tr("Sharing: %s", "共享中: %s"), label);
		thandler_set_status(g_tray, text);
	} else {
		set_idle_status(NULL);
	}
}

static void tick(void *user_data) {
	(void) user_data;

	/* The session bus may come back (or appear late); retry with a backoff. */
	if(!g_tray && g_tray_title && (int32_t) (now_ms() - g_tray_retry_ms) >= 0) {
		g_tray = thandler_init(g_tray_title, g_want_tray);
		if(g_tray) {
			thandler_set_version(g_tray, PROGRAM_VERSION);
			if(g_direct)
				thandler_set_source_provider(g_tray, wlcap_list);
			thandler_set_state(g_tray, g_sharing, g_visible,
					   g_share_kind, g_share_label);
			set_idle_status(NULL);
			fprintf(stderr, "Reconnected to the session bus\n");
		} else {
			g_tray_retry_ms = now_ms() + 5000;
		}
	}

	if(!g_tray)
		return;

	if(thandler_routine(g_tray)) {
		fprintf(stderr, "Lost the tray D-Bus connection; continuing without the tray\n");
		thandler_destroy(g_tray);
		g_tray = NULL;
		g_tray_retry_ms = now_ms() + 5000;
		return;
	}

	if(thandler_take_quit(g_tray))
		running = 0;
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
		"       " PROGRAM_NAME " --search=QUERY\n"
		"\n"
		"Share Wayland screens or windows with X11 applications.\n"
		"The bridge opens an invisible X11 window (\"Wayland Share Bridge\")\n"
		"and mirrors the chosen screen or window into it.  Pick the source\n"
		"from the tray menu; the application you are sharing from only has to\n"
		"share the \"Wayland Share Bridge\" window.\n"
		"\n"
		"Options:\n"
		"  -c, --cursor=MODE     cursor mode for the portal backend: hidden,\n"
		"                        embedded, metadata (sent as stream metadata,\n"
		"                        not painted by the mirror; default: embedded)\n"
		"  -k, --source-kind=KIND\n"
		"                        fallback source kind for the portal backend\n"
		"                        (auto, any, screen, window; default: auto)\n"
		"  -S, --source=SPEC     capture SPEC right away: screen:NAME,\n"
		"                        window:APP_ID, window:APP_ID|TITLE, or\n"
		"                        region:NAME,X,Y,W,H (direct capture only);\n"
		"                        with a running instance, switches it to SPEC\n"
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
		"      --search=QUERY    filter the Windows submenu by QUERY (empty\n"
		"                        clears the filter)\n"
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

static int start_direct(const char *spec) {
	struct source_request req;

	if(parse_source_spec(spec, &req) != 0) {
		fprintf(stderr, "Invalid source '%s' (expected screen:NAME, window:APP_ID or "
			"region:NAME,X,Y,W,H)\n", spec);
		set_idle_status(NULL);
		return -1;
	}
	if(req.cropped && !g_direct) {
		fprintf(stderr, "Region capture needs direct capture\n");
		set_idle_status(NULL);
		return -1;
	}

	if(wlcap_start(&req, g_xdata) != 0) {
		/* The old session is only kept when the request could not be
		 * resolved; otherwise nothing is captured any more. */
		if(!wlcap_active())
			clear_sharing_state();
		set_idle_status(NULL);
		fprintf(stderr, "Cannot share that source; pick another one from the tray menu.\n");
		return -1;
	}

	{
		/* spec may point at g_last_spec (retry paths); never snprintf a
		 * buffer onto itself. */
		char spec_copy[sizeof(g_last_spec)];

		snprintf(spec_copy, sizeof(spec_copy), "%s", spec);
		if(g_remember && g_state_file[0])
			state_write(g_state_file, "source", spec_copy);
		memcpy(g_last_spec, spec_copy, sizeof(g_last_spec));
	}

	g_direct_stalled = 0;
	g_sharing = 1;
	g_share_kind = req.kind;
	utf8_copy(g_share_label, sizeof(g_share_label), req.cropped ? "" : req.id);
	update_tray_state();
	set_sharing_status(wlcap_current_label());
	fprintf(stderr, "Sharing %s (window class %s).\n",
		wlcap_current_label(), g_class_name);
	return 0;
}

static int start_portal(struct ddata *ddata, struct pdata **pdata,
			enum source_kind kind) {
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

	phandler_clear_connection_lost(*pdata);
	g_sharing = 2;
	g_share_kind = ddata->source.type == 1 ? SOURCE_KIND_SCREEN :
		       ddata->source.type == 2 ? SOURCE_KIND_WINDOW : SOURCE_KIND_ANY;
	g_share_label[0] = '\0';
	update_tray_state();
	set_sharing_status(dhandler_describe_source(ddata));
	fprintf(stderr, "Bridge is running. Share the \"%s\" window from your application.\n",
		g_tray_title ? g_tray_title : "Wayland Share Bridge");
	return 0;
}

static void clear_sharing_state(void) {
	g_sharing = 0;
	g_share_kind = SOURCE_KIND_ANY;
	g_share_label[0] = '\0';
	update_tray_state();
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
	g_direct_stalled = 0;
	g_last_spec[0] = '\0';
	clear_sharing_state();
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
		OPT_SEARCH,
		OPT_PORTAL,
		OPT_LIST_SOURCES,
	};
	char *owned_state_dir = NULL;
	char *owned_token_path = NULL;
	char *owned_state_path = NULL;
	const char *control_method = NULL;
	const char *control_argument = NULL;
	const char *initial_source = NULL;
	struct ddata *ddata = NULL;
	struct pdata *pdata = NULL;
	struct source_request initial_req;
	int exit_code = 1;
	int active = 0;
	int direct_active = 0;
	int direct_lost = 0;
	uint32_t direct_retry_ms = 0;
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
		{ "search", required_argument, NULL, OPT_SEARCH },
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
		case OPT_SEARCH:
			control_method = "SearchWindows";
			control_argument = optarg;
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

	if(initial_source && parse_source_spec(initial_source, &initial_req) != 0) {
		fprintf(stderr, "Invalid --source '%s' (expected screen:NAME, window:APP_ID "
			"or region:NAME,X,Y,W,H)\n", initial_source);
		goto out;
	}

	/* Direct capture: no portal, no picker, sources are listed by us. */
	if(!force_portal) {
		if(wlcap_init(options.title) == 0) {
			g_direct = 1;
		} else {
			fprintf(stderr, "Falling back to the desktop portal for screen capture.\n");
		}
	}

	if(list_sources) {
		struct capture_source *list = NULL;
		int count = g_direct ? wlcap_list(SOURCE_KIND_ANY, &list) : 0;
		int i;

		for(i = 0; i < count; i++)
			printf("%s:%s\t%s\n",
			       list[i].kind == SOURCE_KIND_SCREEN ? "screen" : "window",
			       list[i].id, list[i].label);
		free(list);
		if(!g_direct) {
			fprintf(stderr, "Direct capture is not available on this compositor.\n");
			exit_code = 1;
		} else {
			exit_code = 0;
		}
		goto out;
	}

	if(!allow_multiple && tcontrol_is_running()) {
		if(initial_source) {
			/* --source switches the running instance instead of refusing
			 * to start. */
			if(tcontrol_select_source(initial_source) == 0) {
				printf("Switched the running instance to %s\n", initial_source);
				exit_code = 0;
			} else {
				exit_code = 1;
			}
			goto out;
		}
		fprintf(stderr,
			"Another wayland-share-bridge instance is already running.\n"
			"Use --source to switch it, --reselect to pick a new source,\n"
			"--status to inspect it, or --allow-multiple to start an\n"
			"additional bridge.\n");
		goto out;
	}

	if(initial_source && initial_req.cropped && !g_direct) {
		fprintf(stderr, "Region capture needs direct capture\n");
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

	owned_state_path = state_path_for_token(options.token_file);
	if(owned_state_path)
		snprintf(g_state_file, sizeof(g_state_file), "%s", owned_state_path);
	g_remember = options.remember;
	g_visible = options.show ? 1 : 0;

	g_tray_title = options.title;
	g_want_tray = options.tray;

	g_xdata = xhandler_init(&options);
	if(!g_xdata)
		goto out;

	/* Hide the mirror window on compositors where the X11 property is ignored. */
	hyprland_set_visibility(g_visible);

	g_tray = thandler_init(options.title, options.tray);
	if(g_tray) {
		thandler_set_version(g_tray, PROGRAM_VERSION);
		set_idle_status(g_direct ? NULL : tr("Waiting for the system portal…", "等待系统门户…"));
		if(g_direct)
			thandler_set_source_provider(g_tray, wlcap_list);
		update_tray_state();
	}

	{
		struct sigaction action;

		memset(&action, 0, sizeof(action));
		sigemptyset(&action.sa_mask);
		action.sa_handler = on_terminate;
		sigaction(SIGINT, &action, NULL);
		sigaction(SIGTERM, &action, NULL);
		action.sa_handler = on_hangup;
		sigaction(SIGHUP, &action, NULL);
	}

	fprintf(stderr, "Bridge window: \"%s\" (class %s)\n", options.title, options.class_name);
	if(g_direct)
		fprintf(stderr, "Direct capture enabled; use the tray menu to pick a screen or window.\n");
	fprintf(stderr, "Use --reselect, --status or the tray icon to control a running instance.\n");

	if(g_direct) {
		if(initial_source) {
			if(start_direct(initial_source) == 0)
				direct_active = 1;
		} else if(g_remember && g_state_file[0]) {
			char *remembered = state_read(g_state_file, "source");

			if(remembered) {
				if(start_direct(remembered) == 0)
					direct_active = 1;
				free(remembered);
			}
		}

	}

	/* The portal backend needs its session objects even if unused. */
	if((!g_direct || force_portal) && !ddata) {
		ddata = dhandler_init(options.cursor, options.source_kind,
			options.remember && options.token_file != NULL,
			options.token_file);
		if(!ddata)
			goto out;
		dhandler_set_tick(ddata, tick, NULL);
		dhandler_set_stop_flag(ddata, &running);
		if(!g_direct)
			attempt = 1;
	}

	while(running) {
		struct pollfd polls[4];
		nfds_t count;
		int ddata_poll = -1;
		int reselect = 0;
		enum source_kind kind = options.source_kind;

		/* A reselect request (SIGHUP, --reselect, the portal menu entry)
		 * is resolved once, before the portal attempt. */
		if(reselect_requested) {
			reselect_requested = 0;
			reselect = 1;
			kind = SOURCE_KIND_ANY;
		} else if(thandler_take_reselect(g_tray, &kind) == 0) {
			reselect = 1;
		}
		if(reselect) {
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
				dhandler_set_source_kind(ddata, kind);
				attempt = 1;
			}
		}

		if(attempt) {
			attempt = 0;
			if(ddata) {
				wlcap_stop();
				direct_active = 0;
				active = 0;
				clear_sharing_state();
				if(start_portal(ddata, &pdata, ddata->source_kind) == 0)
					active = 1;
				else if(g_direct && g_last_spec[0] && start_direct(g_last_spec) == 0)
					direct_active = 1;

				/* Clicks made while the picker was open must not start a
				 * second picker. */
				(void) thandler_take_reselect(g_tray, NULL);
				reselect_requested = 0;
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
			ddata_poll = (int) count;
			polls[count].fd = ddata->fd;
			polls[count].events = POLLIN;
			polls[count].revents = 0;
			count++;
		}

		if(poll(polls, count, g_direct ? 16 : 200) < 0) {
			if(errno == EINTR)
				continue;	/* SIGHUP/SIGTERM: handled at the top */
			fprintf(stderr, "poll() failed: %s\n", strerror(errno));
			exit_code = 1;
			break;
		}

		/* Keep the tray responsive even when no fd fires. */
		tick(NULL);
		reap_children();

		if(g_tray && g_sharing && thandler_take_stop(g_tray))
			stop_sharing(&ddata, &pdata, &active, &direct_active);
		if(g_tray && g_sharing && thandler_take_visibility(g_tray))
			set_window_visibility(!g_visible);

		if(g_direct && wlcap_dispatch() != 0) {
			fprintf(stderr, "capture: lost the Wayland connection\n");
			wlcap_destroy();
			clear_sharing_state();
			direct_active = 0;
			g_direct = 0;
			direct_lost = 1;
			direct_retry_ms = now_ms() + 2000;
		}

		{
			const char *spec = NULL;

			if(thandler_take_selection(g_tray, &spec)) {
				if(active && pdata) {
					phandler_destroy(pdata);
					pdata = NULL;
					if(ddata)
						dhandler_close_session(ddata);
					active = 0;
				}
				if(g_direct) {
					/* A stale menu entry must not freeze a live capture:
					 * start_direct() leaves the old session running when
					 * the new source cannot be resolved. */
					direct_active = start_direct(spec) == 0 ||
							(wlcap_active() ? 1 : 0);
				} else {
					fprintf(stderr, "Cannot share that source; direct capture is "
						"not available on this compositor.\n");
					set_idle_status(tr("Direct capture is not available on this compositor",
				   "此合成器不支持直连抓屏"));
				}
			}
		}

		if(g_direct && direct_active && !wlcap_active()) {
			fprintf(stderr, "capture: the source closed; pick another one.\n");
			wlcap_stop();
			direct_active = 0;
			g_direct_stalled = 0;
			clear_sharing_state();
			set_idle_status(tr("The source was closed; pick another one",
				   "共享源已关闭，请重新选择"));
		}

		if(g_direct && direct_active) {
			int stalled;

			wlcap_tick();
			stalled = wlcap_stalled();
			if(stalled != g_direct_stalled) {
				g_direct_stalled = stalled;
				if(g_tray) {
					if(stalled)
						thandler_set_status(g_tray,
							"Sharing: no frames from the compositor "
							"(is the source off-screen or on a hidden workspace?)");
					else
						set_sharing_status(wlcap_current_label());
				}
			}
		}

		/* Reconnect after the compositor or its socket went away. */
		if(direct_lost && (int32_t) (now_ms() - direct_retry_ms) >= 0) {
			if(wlcap_init(options.title) == 0) {
				g_direct = 1;
				direct_lost = 0;
				thandler_set_source_provider(g_tray, wlcap_list);
				if(g_last_spec[0])
					direct_active = start_direct(g_last_spec) == 0;
			} else {
				direct_retry_ms = now_ms() + 5000;
				fprintf(stderr, "capture: the compositor is still unavailable; "
					"retrying in 5 s\n");
			}
		}

		if(polls[0].revents & POLLIN)
			xhandler_routine(g_xdata);
		if((polls[0].revents & (POLLERR | POLLHUP)) && !(polls[0].revents & POLLIN)) {
			fprintf(stderr, "Lost the X11 connection\n");
			exit_code = 1;
			break;
		}

		if(ddata_poll >= 0 && ddata && (polls[ddata_poll].revents & POLLIN)) {
			if(dhandler_routine(ddata)) {
				fprintf(stderr, "Lost the session bus; the portal is unavailable\n");
				if(pdata) {
					phandler_destroy(pdata);
					pdata = NULL;
				}
				dhandler_destroy(ddata);
				ddata = NULL;
				active = 0;
				clear_sharing_state();
				set_idle_status("Lost the session bus; the portal is unavailable");
			}
		}

		if(active && pdata && phandler_connection_lost(pdata)) {
			fprintf(stderr, "The PipeWire stream was closed; waiting for a new source.\n");
			phandler_destroy(pdata);
			pdata = NULL;
			if(ddata)
				dhandler_close_session(ddata);
			active = 0;
			clear_sharing_state();
			set_idle_status("The stream was closed; pick another one");
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
	if(g_direct)
		wlcap_destroy();
	if(g_xdata)
		xhandler_destroy(g_xdata);
	free(owned_token_path);
	free(owned_state_dir);
	free(owned_state_path);
	pw_deinit();

	return exit_code;
}

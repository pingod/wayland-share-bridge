# wayland-share-bridge

[![build](https://github.com/pingod/wayland-share-bridge/actions/workflows/build.yml/badge.svg)](https://github.com/pingod/wayland-share-bridge/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/pingod/wayland-share-bridge)](https://github.com/pingod/wayland-share-bridge/releases)

Share **Wayland** screens and windows with **X11 applications**.

Many applications still run through XWayland (or are plain X11 clients) and
therefore cannot see native Wayland windows at all: their screen-share dialog
lists only other XWayland windows and shows a black thumbnail for whole
screens.  `wayland-share-bridge` fixes that by mirroring a screen or a window
into a normal X11 window that every X11 application can see and share.

```
                 Wayland compositor
   ext-image-copy-capture-v1 | ext-foreign-toplevel-list-v1
                             v
              wayland-share-bridge --- draws every frame ---> invisible X11 window
                             ^                                 ("Wayland Share Bridge")
                             |                                        |
      (fallback) desktop portal ScreenCast + PipeWire                v
                                   X11 / XWayland apps (WPS, Zoom, Feishu,
                                                     Electron, browsers, OBS, ...)
```

The source is picked from the tray icon's menu - one click for a screen, one
click for a window - and the choice is remembered for the next start.  The
desktop portal is still available via **Share via system portal...** (`--portal`) as a
fallback for compositors without the direct capture protocols.

The X11 window is invisible and click-through while it waits, so it does not
get in your way for an application to capture it.

## Why

- WPS Office, WPS 协作/会议, Zoom, Feishu and many Electron applications are
  X11/XWayland clients.  Under Wayland their "share screen" pickers either do
  not list native Wayland windows or capture a black screen.
- The Wayland-native implementations of these applications use the
  `org.freedesktop.portal.ScreenCast` portal.  X11 applications do not.
- This bridge gives X11 applications something they *can* capture: an X11
  window that contains a live copy of the Wayland content.

## How it works

Two capture backends are available:

1. **Direct capture (default).**  The bridge talks to the compositor itself
   with `ext-image-copy-capture-v1`; the screens and windows to share are
   enumerated with `wl_output` and `ext-foreign-toplevel-list-v1`.  No portal,
   no system picker: the tray menu lists the sources and switching is one
   click.
2. **Portal fallback (`--portal`).**  The bridge creates a ScreenCast session
   through `xdg-desktop-portal`, receives PipeWire frames and paints them into
   the same X11 window.  Useful when the compositor does not support the
   direct capture protocols; the system picker appears instead of the
   built-in menu.

When a portal session is active, `--reselect` asks the portal for a new source
(one click in the tray menu: **Share via system portal...**).  Source picks made through
direct capture are stored in `$XDG_STATE_HOME/wayland-share-bridge/state`
(`source=screen:DP-2`, `source=window:firefox|Title`) and restored at the next
start.

## Requirements

Runtime:

- PipeWire 0.3.44 or newer (the stream targets its node by object serial).
- A Wayland compositor with **XDG Desktop Portal ScreenCast** support
  (PipeWire + session bus), or, for direct capture, one that advertises
  `ext-image-copy-capture-v1`, `ext-image-capture-source-v1` and
  `ext-foreign-toplevel-list-v1`.  Hyprland 0.56+ supports all of them.
- **XWayland** (for the X11 side of the bridge).
- A compositor that lets you hide or ignore an X11 window; see
  [Making the bridge window invisible](#making-the-bridge-window-invisible).

Build dependencies (Arch package names in parentheses):

- C compiler and `meson` + `ninja`
- `dbus-1` (`dbus`)
- `libpipewire-0.3` (`pipewire`)
- `wayland-client` (`wayland`)
- `wayland-scanner` (`wayland`)
- `x11` (`libx11`), `xext` (`libxext`), `xfixes` (`libxfixes`)

The three Wayland protocol XML files are vendored in `protocols/`, so no
particular `wayland-protocols` version is required.

## Build and install

```sh
meson setup build
ninja -C build
meson test -C build      # unit tests for the pure helpers
sudo ninja -C build install
```

This installs:

- `/usr/local/bin/wayland-share-bridge` – the bridge
- `/usr/local/bin/wayland-share-bridge-launch` – a small launcher that also
  sets up the Hyprland rules (see below)
- `/usr/local/share/applications/wayland-share-bridge.desktop` – menu entry
- `/usr/local/share/man/man1/wayland-share-bridge.1` – manual page (`man wayland-share-bridge`)
- `/usr/local/share/systemd/user/wayland-share-bridge.service` – systemd
  user unit (`systemctl --user enable --now wayland-share-bridge`)
- `/usr/local/share/metainfo/wayland-share-bridge.metainfo.xml` – AppStream data

## Quick start

1. Start the bridge (menu entry **Wayland Share Bridge** or from a terminal):

   ```sh
   wayland-share-bridge-launch
   ```

2. An invisible window called **Wayland Share Bridge** is created, and a tray
   icon appears.  Right-click it and pick a source from the flat menu:
   - `Screen: DP-2 (2560x1440)` — mirror that monitor
   - `Window: firefox — ...` — mirror that native Wayland window
   - **Show shared window** — temporarily make the mirror window visible, so
     you can see on your own screen what you are sharing; a shown window
     accepts the pointer, so you can move it with your compositor's usual
     mouse binding (e.g. Super+left drag).  **Hide** makes it click-through
     again
   - **Stop sharing** — stop mirroring and hide the window, without quitting
     the bridge
   - **Share via system portal...** — the portal picker, for compositors without
     direct capture
3. In the application you want to share from, open its screen-share dialog and
   choose the **Wayland Share Bridge** window:

   - WPS 会议/协作: **共享 → 应用窗口 → Wayland Share Bridge**
   - Zoom: **Share Screen → Window → Wayland Share Bridge**
   - OBS: add a **Window Capture** source and pick **Wayland Share Bridge**
   - Browsers: pick **Wayland Share Bridge** in their window list

Whole-screen entries in those applications stay black: they capture the X11
root window, which is empty under Wayland.  Always share the bridge window.

Switching sources later is a single tray-menu click; no picker, no restart.

## Usage

```
Usage: wayland-share-bridge [options]
       wayland-share-bridge --list-sources
       wayland-share-bridge --reselect[=KIND]
       wayland-share-bridge --status
       wayland-share-bridge --quit
       wayland-share-bridge --search=QUERY

Options:
  -c, --cursor=MODE     cursor mode for the portal backend: hidden,
                        embedded, metadata (default: embedded)
  -k, --source-kind=KIND
                        fallback source kind for the portal backend
                        (auto, any, screen, window; default: auto)
  -S, --source=SPEC     capture SPEC right away: screen:NAME,
                        window:APP_ID, window:APP_ID|TITLE, or
                        region:NAME,X,Y,W,H (direct capture only);
                        with a running instance, switches it to SPEC
  -L, --list-sources    list the screens and windows that can be shared
                        (exits 1 when direct capture is unavailable)
      --portal          always use the desktop portal (system picker)
  -r, --remember        remember the last source (default)
  -n, --no-remember     do not remember the source
  -s, --show            show the bridge window (debugging)
  -t, --title=TEXT      window title (default: Wayland Share Bridge)
  -f, --token-file=PATH restore token file used by the portal backend
      --no-tray         do not create the tray icon (the D-Bus control
                        interface stays, so --status/--quit/--reselect/
                        --source still work)
      --allow-multiple  allow more than one running instance
  -h, --help            show this help
  -V, --version         show the version
```

Examples:

```sh
wayland-share-bridge --list-sources
wayland-share-bridge --source=screen:DP-2
wayland-share-bridge --source='window:firefox|DeepSeek' --no-tray
wayland-share-bridge --portal --cursor=hidden
wayland-share-bridge --search firefox   # filter the running instance's Windows menu
```

## Tray menu

| Entry | Action |
| --- | --- |
| **Stop sharing** | Stop mirroring and hide the window again; the bridge keeps running. |
| **Show/Hide shared window** | Toggle the mirror window between visible and hidden. Hidden it is invisible and click-through; shown it accepts the pointer so the compositor can drag it (Super+drag in Hyprland). It hides itself again when sharing stops. |
| **Screens ▸** | Submenu with one `Screen: NAME (WxH)` entry per monitor. |
| **Windows ▸** | Submenu grouped by application: `firefox (3) ▸ window title`. |
| **Share via system portal...** | Portal picker (same as `--reselect`). |
| **Quit** | Stop the bridge. |
| Header row (`Wayland Share Bridge vVERSION`) | The tray host renders the SNI tooltip title here; the version is part of it. |

The current source is marked with a checkmark.  The version is in the menu
header (the SNI tooltip title), not in a menu entry of its own.  The source
lists are refreshed from the compositor's current outputs and toplevels every
time a submenu is opened.

dbusmenu has no text input field, so the search is driven from a terminal (or
a key binding):

```sh
wayland-share-bridge --search firefox   # only windows matching "firefox"
wayland-share-bridge --search ""        # clear the filter
```

While a filter is active, the first entry of **Windows ▸** reads
`Search: firefox (clear)`; clicking it clears the filter.  Matching is a
case-insensitive substring over the app id, window title and identifier.
Long titles are ellipsized and applications are grouped into submenus, so
menu entries stay short enough for narrow tray popups.

## Making the bridge window invisible

The bridge window carries `_NET_WM_WINDOW_OPACITY=0`; compositors that honour
it (KDE Plasma, GNOME) hide the window automatically.  Hyprland ignores that
property, so the installed launcher
(`wayland-share-bridge-launch`) adds a window rule:

```sh
hyprctl eval 'hl.window_rule({ name = "wayland-share-bridge-hidden",
  match = { class = "^wayland-share-bridge$" },
  opacity = 0.0, float = true, no_blur = true, no_initial_focus = true, no_focus = true })'
```

It falls back to `hyprctl keyword windowrulev2 ...` on Hyprland versions with
the legacy parser.  See [docs/hyprland.md](docs/hyprland.md).

## Log file

Started from the desktop entry there is no terminal, so
`wayland-share-bridge-launch` appends the bridge's stderr to
`$XDG_STATE_HOME/wayland-share-bridge/bridge.log` (truncated when it grows
past 1 MiB).  Run the binary directly to see the messages on the terminal.

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `Direct capture is not available` | The compositor lacks `ext-image-copy-capture-v1`; use `--portal`. |
| Tray menu is empty | No tray host (StatusNotifierWatcher) is running; use `--list-sources` and `--source=`. |
| Menu entries appear after a few seconds | The menu is refreshed when it opens; reopen it. |
| WPS still shows a black thumbnail | You are sharing the X11 screen, not the bridge window. Pick **Wayland Share Bridge**. |
| Window is visible on screen | Use the launcher, or add the Hyprland rule manually. |
| Cursor missing in the mirror | Only monitor capture paints the cursor; window capture does not. |
| Black mirror for one window | The window is off-screen or on a hidden workspace; Hyprland does not render frames for it. Bring it into view; the bridge retries every 2 s and says so in the tray tooltip/`--status`. |
| Colors swapped | Report the compositor/shm format; only XRGB8888/ARGB8888 are used. |
| Portal keeps asking every time | Hyprland does not return a restore token; direct capture does not need one. |

More: [docs/troubleshooting.md](docs/troubleshooting.md) and
[docs/hyprland.md](docs/hyprland.md).

## Limitations

- Region capture needs direct capture: `--source=region:DP-2,X,Y,W,H` mirrors
  just that rectangle of the screen.  The portal backend cannot crop.
- Audio is out of scope; share audio with your application's own options.
- Window capture mirrors the window contents, not its decorations.
- The frame loop paints at ~30 Hz.  Damage rectangles are only pushed to the
  X server; the mirror window uses MIT-SHM when the server supports it, so a
  4K share no longer copies every frame through the X11 socket.
- Rotated outputs are un-rotated according to the transform the compositor
  reports; a rotated monitor should be verified once after a compositor
  upgrade.
- The tray menu and the status text follow `LC_ALL`/`LC_MESSAGES`/`LANG`
  (English or Chinese).
- A remembered window source is a specific window: if the application is
  restarted, the remembered source no longer exists and the bridge starts
  idle instead.  Remembered *screen* sources survive restarts.
- The portal fallback blocks the bridge while the system picker is open; the
  direct capture backend never does.

## Project layout

```
src/            bridge sources (capture backends, X11 window, tray, control bus)
protocols/      vendored Wayland protocol XML files
data/           launcher script and desktop entry
docs/           compositor notes, scenarios, troubleshooting
man/            manual page
CHANGELOG.md    release history
```

## License and credits

EUPL-1.2.  The original idea and the first implementation come from
[video_bridge](https://github.com/lmarz/video_bridge) by lmarz; see
[NOTICE](NOTICE) for the list of changes.

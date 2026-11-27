# Changelog

All notable changes to this project are documented here.

## 1.5.0 - 2026-11-27

- **Quality and frame-rate settings.**  The tray menu gained
  **Quality ▸** (`Source (sharpest)`, `1080p`, `720p`, `480p`) and
  **Frame rate ▸** (60, 30, 15, 5 fps), also available as
  `--quality=PRESET` and `--fps=RATE`.  Presets cap the mirror resolution with
  a box-filter downscale (aspect ratio preserved, never upscaled) and are
  remembered in the state file.  `Source (sharpest)` keeps the old lossless
  1:1 mirror; the lower presets trade sharpness for CPU, X11 traffic and
  encoder bandwidth.
- **Resizing the shared window stopped the stream and left the sharing app
  with a black picture.**  When the captured window is resized while a frame
  is in flight, the compositor fails that frame with `buffer_constraints` and
  follows it with fresh constraints.  The bridge treated every failed frame as
  fatal: the direct capture was stopped, the mirror window was unmapped, and
  **Show shared window** turned itself off.  A buffer-constraints failure is
  now handled as what it is - a resize - by waiting for the new constraints,
  reallocating the buffer and continuing to capture.  The first frame into a
  new buffer is also presented in full, so a resize cannot leave undamaged
  (black) parts of the window behind.

## 1.4.3 - 2026-11-19

- **Stopping sharing or quitting left an invisible window on the desktop.**
  Hiding the mirror only set opacity 0 and an empty input region; the window
  stayed *mapped*, so the compositor kept listing it as a client (window
  lists, overviews, `hyprctl clients`, Alt-Tab).  The window is now unmapped
  whenever nothing is mirrored - at start-up, after **Stop sharing**, after a
  closed source, a lost stream, a lost session bus, and right before the
  process exits - and mapped again when the next stream starts.  A mapped
  window is required for capture, so this only happens while sharing.
- The Hyprland rule is reset to its hidden state on exit, so a bridge that was
  quit while the window was shown no longer leaves a rule that makes the next
  window visible for a moment.

## 1.4.2 - 2026-11-18

- **Show shared window could not be dragged on Hyprland, and the window
  swallowed clicks even while hidden.**  The Hyprland window rule always
  carried `no_focus = true`.  That is a *dynamic* effect: Hyprland's hit test
  skips every window that carries it (`ViewHitTester::windowAt`), and
  Super+drag (`movewindow`) finds its target through the same hit test.  A
  shown window could therefore never be grabbed, and the pointer went
  straight through it.  `no_focus` now follows the visibility (hidden: on,
  shown: off) and `no_initial_focus` keeps the window from taking the
  keyboard when it maps.  The 1.4.1 input-region change was not the cause.
- **The mirror window stayed on screen after sharing stopped.**  The tray
  **Stop sharing** item, a closed source, a lost PipeWire stream or a lost
  session bus all left the last frame floating over the desktop with a live
  input region.  Every path that really ends a stream now hides the window
  again; a reselect or a failed start does not.
- A **Show/Hide** or **Stop** click that arrived in the same event-loop tick
  as the stream ending stayed pending and fired on the next share.  Pending
  tray requests are now always consumed.
- The interactive input region follows the frame size: resizing the window
  used to leave a region sized for the previous frame, so a window that grew
  could only be grabbed over part of its area.
- The Hyprland rule string moved to `src/hypr_rule.c` with a unit test that
  pins both states, because getting `no_focus` wrong is invisible in a build.

## 1.4.1 - 2026-11-18

- Fix a regression from 1.4.0: the mirror window kept an empty input region
  when it was shown, so the compositor could no longer target it and
  **Show shared window** could not be dragged any more.  Shown now means a
  full input region again; hidden stays click-through.

## 1.4.0 - 2026-11-18

- Crash fix: a window title or status text that was cut in the middle of a
  UTF-8 code point made libdbus abort the whole bridge.  All fixed-size
  buffers are now filled with a UTF-8 safe copy, and D-Bus string properties
  are sanitised at the boundary.
- `SIGHUP` now asks the running instance for a new source again: `poll()`'s
  `EINTR` is no longer treated as a fatal error, and the signal handlers are
  installed with `sigaction`.
- The portal session is actually closed: `Session.Close` was sent to the
  interface name instead of `org.freedesktop.portal.Desktop`, so sessions
  and the compositor's "sharing" indicator stayed alive.
- A PipeWire stream that failed to be created, connected or negotiated is
  reported as a failure instead of showing a black "sharing" window.
- Frame presentation uses MIT-SHM when the server supports it and only pushes
  the damage rectangles the compositor reported; rotated outputs are handled
  through the reported transform, and XBGR8888/ABGR8888 frames are accepted.
- The source list refreshes once per menu open instead of twice.
- `--show` and the tray toggle keep the mirror window click-through; the
  window name is also exported as `_NET_WM_NAME` (UTF-8).
- The tray reconnects when the session bus returns and re-registers the icon
  when the StatusNotifierWatcher restarts; tooltip/status changes emit
  `NewToolTip`/`NewStatus`.
- dbusmenu fixes: unique separator ids, `GetGroupProperties` with an empty id
  list returns all items, and menu id collisions are reported.
- Hyprland: the binary falls back to the legacy rule syntax when the Lua
  parser is unavailable; the cosmetic z-order call is fire-and-forget, so
  showing the window no longer blocks the frame loop, and exited `hyprctl`
  children are reaped from the event loop.
- The state file is written with `mkstemp`+`flock`, is fsynced, and carries a
  schema version; the portal restore token is created with mode 0600.
- A source switch that fails clears the stale tray state; the remembered
  source is no longer wiped by a self-overlapping `snprintf`.
- `wayland-share-bridge-launch` appends diagnostics to
  `$XDG_STATE_HOME/wayland-share-bridge/bridge.log` when started without a
  terminal.
- New: unit tests (`meson test`), ASan/UBSan CI job, systemd user unit,
  AppStream metadata, and English/Chinese menu strings.
- The release workflow packages the generated desktop file and man page from
  the build directory again.

## 1.3.1 - 2026-11-18

- The version moved from a menu entry of its own into the menu header
  (`Wayland Share Bridge vVERSION`, the SNI tooltip title), so the bottom of
  the menu no longer has a version row.

## 1.3.0 - 2026-11-18

- The Windows submenu is grouped by application (`firefox (3) ▸ title`) and
  sorted, so a long list stays navigable and entries stay short enough for
  narrow tray popups; long titles are ellipsized.
- Window search: `--search QUERY` (or the `SearchWindows` control method)
  filters the Windows submenu; the menu shows `Search: QUERY (clear)` while
  active, and clicking it clears the filter.

## 1.2.0 - 2026-11-18

- The tray menu is categorised: **Screens** and **Windows** are submenus
  instead of one long flat list.
- The version is visible in the application menu entry, the tray menu (as an
  enabled entry, since some hosts hide disabled ones), the tooltip,
  `--version` and `--status`.
- No more silent black mirror: when the compositor does not complete a frame
  within 2 s (an off-screen window or a hidden workspace) the bridge drops the
  stuck frame, retries, and reports the stall in the tray/`--status`.

## 1.1.0 - 2026-11-18

- Version management: the version lives only in `meson.build`; the binary and
  the manual page are generated from it.  The running version is shown in the
  tray menu, the tray tooltip and `--status`.
- Capture rate no longer drops to ~10 Hz while the tray exists: the D-Bus
  dispatch is non-blocking.
- Region capture: `--source=region:NAME,X,Y,W,H` mirrors a rectangle of a
  screen (direct capture only).
- `--source=SPEC` against a running instance now switches that instance
  instead of refusing to start.
- The D-Bus control interface (`--status`, `--quit`, `--reselect`,
  `--source`) works under `--no-tray`.
- `window:APP_ID` now matches a window by its app id; a non-empty id that
  matches nothing fails instead of sharing an arbitrary window.
- Fixed: corrupt `GetGroupProperties` replies, double close of the portal's
  PipeWire descriptor, portal cursor modes (`cursor_mode` 1/2/4), the two
  backends erasing each other's state file, stale tray state after a failed
  capture, and Xlib use from the PipeWire thread without `XInitThreads()`.
- PipeWire node failures are reported instead of showing a black window
  forever; closed windows and unplugged outputs leave the source list.

## 1.0.0 - 2026-10-20

First public release.

- Share Wayland screens and windows with X11/XWayland applications (WPS
  Office, WPS Meeting, Zoom, browsers, ...) by mirroring the captured frames
  into an invisible X11 window.
- Direct capture backend built on `ext-image-copy-capture-v1`,
  `ext-image-capture-source-v1` and `ext-foreign-toplevel-list-v1`: picking a
  screen or window from the tray switches the shared source instantly, with no
  portal picker in between.
- Desktop portal `ScreenCast` backend kept as a fallback for compositors
  without the direct-capture protocols (`--portal`).
- Tray icon (StatusNotifierItem + com.canonical.dbusmenu) with a flat menu:
  **Stop sharing**, **Show/Hide shared window**, one entry per screen and
  window (the active one is checked), **Share via system portal...**, **Quit**,
  and a disabled entry showing the running version.
- The mirror window stays invisible and click-through (`XFixes` input region)
  while hidden; **Show shared window** reveals it as a normal floating window
  so it can be inspected and moved with the compositor's native drag.
- Remembers the last shared source and restores it on the next start.
- Single-instance control over D-Bus: `--reselect`, `--status`, `--quit`,
  `--show`, `--allow-multiple`.
- Command line: `--source=screen:NAME|window:APP_ID`, `--list-sources`,
  `--cursor=hidden|embedded|metadata`, `--remember/--no-remember`,
  `--token-file`, `--title`, `--portal`.
- Manual page, desktop entry, launcher script that installs the Hyprland
  window rule, CI and release workflows.

Licensed under the EUPL-1.2.

# Changelog

All notable changes to this project are documented here.

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

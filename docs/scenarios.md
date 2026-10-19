# Scenarios

All scenarios follow the same recipe:

1. Start `wayland-share-bridge-launch` (starts hidden, tray icon appears).
2. Right-click the tray icon and pick `Screen: DP-2 (2560x1440)` or
   `Window: firefox — ...`.  One click, flat menu, no system picker.
3. In the application, share the **Wayland Share Bridge** window.

If the compositor does not support direct capture, start the bridge with
`--portal`; the portal picker then appears instead of the built-in menus.

## WPS Office / WPS 协作 / WPS 会议

WPS Office and WPS 协作 (`xiezuo`, Electron 22) are X11/XWayland
applications.  Their share dialog only lists XWayland windows and captures a
black screen for monitors.

- Open the meeting, click **共享**.
- Switch to **应用窗口** (Application window).
- Choose **Wayland Share Bridge**.
- Do not use **屏幕 1/屏幕 2**; those capture the empty X11 root window.

If you need to share a single sentence/paragraph/area instead, use WPS's
document sharing or share the bridge window showing that document.

## WPS Office / WPS 协作 / WPS 会议: sharing a real Wayland window

WPS 会议 cannot see native Wayland windows, so:

1. Pick the window in the bridge's tray menu: `Window: firefox — ...`.
2. In WPS choose **Wayland Share Bridge**.
3. Switch the bridge to another window later without touching WPS: the X11
   window keeps existing, WPS keeps capturing it.

## Zoom (X11 client)

**Share Screen → Window → Wayland Share Bridge**.  Zoom's Wayland-native
build can use the portal directly and does not need the bridge.

## Feishu / Lark

**Share Screen → Window → Wayland Share Bridge**.  Whole-screen entries are
black for the same reason.

## OBS (window capture + virtual camera)

OBS runs natively on Wayland and does not need the bridge for *recording*
(use its PipeWire screen capture).  The bridge is useful when a meeting tool
can only capture X11:

- **Window Capture** source → **Wayland Share Bridge**, or
- use OBS itself as the source: add a **屏幕采集(PipeWire)** source, start
  the **Virtual Camera**, and select **OBS Virtual Camera** in the meeting
  tool (works for tools that can pick cameras but not screens).

## Browsers

Chrome/Chromium are XWayland by default; pick **Wayland Share Bridge** in
their window list.  Firefox native Wayland and Chrome with `--ozone-platform=wayland`
can use the portal picker directly instead.

## Recording

The bridge is not a recorder; use `wf-recorder`, OBS (PipeWire + portal) or
`gpu-screen-recorder` for recording.  The bridge exists to *re-expose*
Wayland content to X11 consumers.

## Multiple simultaneous shares

One bridge instance mirrors one stream.  Start more instances with distinct
titles:

```sh
wayland-share-bridge --title="Bridge - monitor 1" --allow-multiple &
wayland-share-bridge --title="Bridge - window" --allow-multiple --no-remember &
```

In the X11 application each window appears with its own title.  The second
instance needs `--allow-multiple`; `--no-remember` keeps the remembered source
of the first one untouched.

## Switching sources without restarting

Switching is the whole point of the 1.2 tray menu:

- **Tray menu → `Screen: DP-2 (2560x1440)`** - mirror a monitor.
- **Tray menu → `Window: firefox — ...`** - mirror a window.
- **Tray menu → Share via system portal...** - fall back to the portal picker.
- CLI: `wayland-share-bridge --source=window:kitty` (a running instance is
  controlled through the tray/control interface; for a one-shot switch use
  `--reselect=window`).
- Signal: `kill -HUP $(pgrep -x wayland-share-bridge)` opens the portal
  picker.

The same X11 window keeps its identity, so applications that already capture
it pick up the new content without re-selecting the window.

## Automation: start hidden, bridge on demand

`--no-tray` keeps only the control interface, so a hotkey or a script can
drive the bridge:

```sh
wayland-share-bridge --no-tray --source=screen:DP-2 &
wayland-share-bridge --status
wayland-share-bridge --quit
```

For a headless one-shot share of a window:

```sh
wayland-share-bridge --no-tray --source='window:firefox|DeepSeek' --allow-multiple &
```

The source names come from:

```sh
wayland-share-bridge --list-sources
```

## Restoring a source after login

With the default `--remember`, the last source is stored in
`$XDG_STATE_HOME/wayland-share-bridge/state` and restored on the next start.
Disable it per instance with `--no-remember`.

Note that a remembered *window* source is matched by `APP_ID|TITLE`; if the
application title changes between sessions, pick the window again once.

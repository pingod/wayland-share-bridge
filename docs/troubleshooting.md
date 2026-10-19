# Troubleshooting

## First checks

```sh
# Is direct capture supported?
wayland-info | grep -E 'ext-image-copy-capture|ext-foreign-toplevel-list|ext-output-image-capture'

# What can be shared right now?
wayland-share-bridge --list-sources

# Is an instance running, and what is it sharing?
wayland-share-bridge --status

# Tray host present?
busctl --user list | grep -i StatusNotifierWatcher
```

## `Direct capture is not available`

The compositor does not advertise `ext-image-copy-capture-v1` (or the
companion source/toplevel protocols).  Options:

- start the bridge with `--portal` and use **Share via system portal...**;
- upgrade the compositor (Hyprland 0.56+ supports everything);
- keep using the portal permanently: `--portal`.

The fallback is automatic: when `--portal` is passed, or the protocols are
missing, the bridge prints a note and uses the portal.

## Tray menu is empty or lists no sources

- There must be a StatusNotifierWatcher (DMS, waybar, KDE, GNOME extension...).
  Without one the icon cannot appear: use `--list-sources` + `--source=`.
- The source list is filled when the menu is opened.  If it is stale,
  close and reopen the menu.
- If the compositor reports no toplevels, check `wayland-info | grep
  ext_foreign_toplevel`.
- If another instance is running, the new one exits:

  ```
  Another wayland-share-bridge instance is already running.
  ```

  Use `--allow-multiple`, or control the running one with `--status`,
  `--reselect`, `--quit`.

## The menu opens but clicking an entry does nothing

- The bridge must still run (`--status`); clicking **Quit** ends it.
- Check the journal for `capture: no matching source` - the window was closed
  between opening the menu and clicking it; reopen the menu.

## Black frames / no image

- You are sharing the X11 screen instead of the bridge window.  In the
  application pick the **Wayland Share Bridge** window.
- Is the bridge window still alive? `wayland-share-bridge --status` and
  `hyprctl clients | grep wayland-share-bridge`.
- For window sources, the window must stay mapped; minimized/hidden windows
  may report a stop.
- With the portal backend, check the PipeWire stream:
  `pw-dump | grep -i 'wayland-share-bridge'`.

## Wrong colours

The bridge paints XRGB8888/ARGB8888.  If the compositor sends a different
format the image looks swapped or garbage; report the compositor version
together with `wayland-info` output.

## The window is visible (black rectangle)

The X11 window carries `_NET_WM_WINDOW_OPACITY=0`, which Hyprland ignores.
Start the bridge through `wayland-share-bridge-launch` so the Hyprland rule is
installed, or add it manually:

```sh
hyprctl eval 'hl.window_rule({ name = "wayland-share-bridge-hidden",
  match = { class = "^wayland-share-bridge$" },
  opacity = 0.0, float = true, no_blur = true, no_focus = true })'
```

See [hyprland.md](hyprland.md).

## The X11 application cannot find the window

- The window class is `wayland-share-bridge`, the title `Wayland Share
  Bridge`.  Some applications filter by title: pass `--title="..."`.
- WPS lists only *toplevel* XWayland windows; the bridge window is a normal
  toplevel, so it should appear.  If not, restart the application after the
  bridge window exists.
- While sharing, the window must not be unmapped: keep the bridge running.

## High CPU / latency

- Frame rate is capped at ~30 Hz; cost grows with resolution.
- Mirror a window instead of a 4K monitor if possible.
- Use `--source=screen:...` with the smallest monitor you need.

## `Could not register app ID` warning

Only relevant to the portal backend; harmless (the tray connection registers a
name first).  Direct capture does not use the portal.

## `--status` hangs or times out

- No instance is running: `--status` waits for a reply.  Check `pgrep -f
  wayland-share-bridge`.
- The instance is stuck in a portal picker: cancel the picker, or
  `kill -TERM` it; it replies to control requests between portal steps.

## `--reselect` does nothing

`--reselect` asks the *running* instance for a new portal source.  Use the
tray menu instead for a one-click direct switch, or check `--status` first.

## No tray icon

- No StatusNotifierWatcher: use `--no-tray` plus `--source=`/`--list-sources`.
- Some hosts show the icon only after a few seconds; check
  `busctl --user list | grep StatusNotifierItem`.
- DMS/Quickshell requires valid dbusmenu signatures; version 1.2 provides
  them.  If the menu is missing after an upgrade, restart the bridge.

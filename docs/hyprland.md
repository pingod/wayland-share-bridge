# Hyprland notes

The bridge needs a mapped X11 window so XWayland applications can enumerate
and capture it.  That window should be invisible and must never take input.
Input is handled by the bridge (empty XFixes input region), visibility is a
compositor concern.

Hyprland ignores `_NET_WM_WINDOW_OPACITY` for XWayland windows, so the window
would otherwise show up as a small black rectangle.  Use a window rule:

```lua
hl.window_rule({
    name = "wayland-share-bridge-hidden",
    match = { class = "^wayland-share-bridge$" },
    opacity = 0.0,
    float = true,
    no_blur = true,
    no_focus = true,
})
```

- `opacity = 0.0` makes it invisible.
- `float = true` keeps it out of tiling and lets the client choose its own
  size (the bridge resizes the window to the captured frame).
- `no_blur = true` avoids unnecessary GPU work.
- `no_focus = true` keeps it from stealing keyboard focus if it ever maps
  before the rule is applied.

The launcher `wayland-share-bridge-launch` runs the equivalent of

```sh
hyprctl eval 'hl.window_rule({ name = "wayland-share-bridge-hidden", match = { class = "^wayland-share-bridge$" }, opacity = 0.0, float = true, no_blur = true, no_focus = true })'
```

before starting the bridge (it discovers `HYPRLAND_INSTANCE_SIGNATURE` from
`$XDG_RUNTIME_DIR/hypr/*/.socket.sock` when the variable is not exported) and
falls back to the legacy `windowrulev2` syntax when the Lua parser is not in
use.

## Which parser am I using?

Run:

```sh
hyprctl keyword windowrulev2 "float, class:^wayland-share-bridge$"
```

- `ok` → legacy `hyprland.conf` parser.  Use `windowrulev2` lines.
- `keyword can't work with non-legacy parsers. Use eval.` → Lua parser.  Use
  `hl.window_rule({...})` (or `hyprctl eval`).

## Verify the rule

```sh
hyprctl clients | grep -A 12 wayland-share-bridge
```

Look for `opacity: 0.000000` or the rule name in the output.  `xwininfo -id
<window-id>` shows the X11 side (size, map state, class).

## Notes

- The launcher adds the rule on every start; if you restart it many times the
  rule list grows with identical entries.  That is harmless, but if you prefer
  a single permanent rule, add it to your Hyprland configuration instead of
  using the launcher.
- `--show` still applies the click-through input region, so a visible bridge
  window does not steal clicks either.
- If window rules are not your thing, a small alternative is to keep the
  window off-screen by using a dedicated scratchpad workspace only for the
  bridge, but then X11 clients cannot capture it reliably; the rule is the
  recommended way.

## Direct capture on Hyprland

Hyprland 0.56+ advertises `ext-image-copy-capture-v1`,
`ext-image-capture-source-v1` and `ext-foreign-toplevel-list-v1`, so the
default backend needs no portal and no picker: the flat tray menu lists
`hyprctl monitors`-style outputs and the foreign toplevels.  Check with:

```sh
wayland-info | grep -E 'ext-image-copy-capture|ext-foreign-toplevel-list'
```

If those globals are missing, run the bridge with `--portal`; the Hyprland
window rule above stays the same.

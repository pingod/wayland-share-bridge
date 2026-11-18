/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * hypr_rule.c - the Hyprland window rule that hides the mirror window.
 *
 * Hyprland ignores _NET_WM_WINDOW_OPACITY for XWayland windows, so the window
 * is hidden with a window rule instead.  Re-evaluating a rule with the same
 * name replaces it, which makes runtime toggling possible.
 *
 * The rule is built in its own file so the unit tests can check both states
 * without a compositor: getting no_focus wrong is invisible in a build and
 * only shows up as "the window cannot be dragged", which is exactly how this
 * broke before.
 */

#include <stdio.h>

#include "hypr_rule.h"

int hypr_visibility_rule(char *buffer, size_t size, int visible) {
	int written;

	if(!buffer || size == 0)
		return -1;

	/*
	 * no_focus is a *dynamic* effect and Hyprland's hit test skips every
	 * window that carries it (ViewHitTester::windowAt), so it has to
	 * follow the visibility: hidden windows must be skipped so clicks go
	 * through, a shown window must stay targetable or Super+drag cannot
	 * grab it.  no_initial_focus keeps the window from stealing the
	 * keyboard when it maps.
	 */
	written = snprintf(buffer, size,
			   "hl.window_rule({ name = \"wayland-share-bridge-hidden\", "
			   "match = { class = \"^wayland-share-bridge$\" }, opacity = %s, "
			   "float = true, no_blur = true, no_initial_focus = true, "
			   "no_focus = %s })",
			   visible ? "1.0" : "0.0", visible ? "false" : "true");

	if(written < 0 || (size_t) written >= size)
		return -1;

	return 0;
}

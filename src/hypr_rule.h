/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef HYPR_RULE_H
#define HYPR_RULE_H

#include <stddef.h>

/*
 * Builds the Hyprland window rule that makes the mirror window invisible
 * (and pointer-transparent) while hidden, and visible and grabbable while
 * shown.  Writes into buffer and returns 0, or -1 when the rule does not fit.
 *
 * no_focus must follow the visibility: Hyprland's hit test skips every window
 * that carries this dynamic effect, and Super+drag finds its target through
 * the same hit test.  A shown window with no_focus can never be dragged.
 */
int hypr_visibility_rule(char *buffer, size_t size, int visible);

#endif /* HYPR_RULE_H */

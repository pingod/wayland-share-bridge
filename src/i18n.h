/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef I18N_H
#define I18N_H

/*
 * Minimal translation for the handful of user visible strings in the tray
 * menu and the status text.  The project ships an English and a Chinese
 * README; the UI follows the same two languages instead of pulling in
 * gettext for a few dozen strings.
 */
const char *tr(const char *english, const char *chinese);

#endif /* I18N_H */

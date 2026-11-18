/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * i18n.c - picks English or Chinese from the locale environment.
 *
 * POSIX precedence applies: LC_ALL wins over LC_MESSAGES, which wins over
 * LANG.  A "C" locale therefore suppresses LANG=zh_CN.UTF-8, as expected.
 */

#include <stdlib.h>
#include <string.h>

#include "i18n.h"

static int chinese = -1;

static int detect_chinese(void) {
	static const char *const variables[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
	size_t i;

	for(i = 0; i < sizeof(variables) / sizeof(variables[0]); i++) {
		const char *value = getenv(variables[i]);

		if(value && value[0])
			return strncmp(value, "zh", 2) == 0;
	}

	return 0;
}

const char *tr(const char *english, const char *chinese_text) {
	if(chinese < 0)
		chinese = detect_chinese();

	return chinese ? chinese_text : english;
}

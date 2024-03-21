/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "functions/common.h"
#include "functions/file.h"
#include "lang/typecheck.h"
#include "log.h"

bool
file_is_linkable(struct workspace *wk, obj file)
{
	const struct str *s = get_str(wk, *get_obj_file(wk, file));

	const char *suffs[] = { ".a", ".dll", ".lib", ".so", ".dylib", NULL };
	uint32_t i;
	for (i = 0; suffs[i]; ++i) {
		if (str_endswith(s, &WKSTR(suffs[i]))) {
			return true;
		}
	}

	return false;
}

static bool
func_file_full_path(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = *get_obj_file(wk, self);
	return true;
}

const struct func_impl impl_tbl_file[] = {
	{ "full_path", func_file_full_path, tc_string },
	{ NULL, NULL },
};

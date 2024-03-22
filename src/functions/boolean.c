/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "functions/boolean.h"
#include "functions/common.h"
#include "lang/typecheck.h"
#include "log.h"

static bool
func_boolean_to_string(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { obj_string, .optional = true }, { obj_string, .optional = true }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	if (get_obj_bool(wk, self)) {
		*res = an[0].set ? an[0].val : make_str(wk, "true");
	} else {
		*res = an[1].set ? an[1].val : make_str(wk, "false");
	}

	return true;
}

static bool
func_boolean_to_int(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	int32_t val = get_obj_bool(wk, self) ? 1 : 0;
	make_obj(wk, res, obj_number);
	set_obj_number(wk, *res, val);
	return true;
}

const struct func_impl impl_tbl_boolean[] = {
	{ "to_int", func_boolean_to_int, tc_number },
	{ "to_string", func_boolean_to_string, tc_string },
	{ NULL, NULL },
};

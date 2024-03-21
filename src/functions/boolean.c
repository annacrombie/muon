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
func_boolean_to_string(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm ao[] = { { obj_string }, { obj_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	if (get_obj_bool(wk, rcvr)) {
		*res = ao[0].set ? ao[0].val : make_str(wk, "true");
	} else {
		*res = ao[1].set ? ao[1].val : make_str(wk, "false");
	}

	return true;
}

static bool
func_boolean_to_int(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	int32_t val = get_obj_bool(wk, rcvr) ? 1 : 0;
	make_obj(wk, res, obj_number);
	set_obj_number(wk, *res, val);
	return true;
}

const struct func_impl impl_tbl_boolean[] = {
	{ "to_int", func_boolean_to_int, tc_number },
	{ "to_string", func_boolean_to_string, tc_string },
	{ NULL, NULL },
};

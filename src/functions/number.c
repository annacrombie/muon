/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <inttypes.h>

#include "functions/common.h"
#include "functions/number.h"
#include "lang/typecheck.h"
#include "log.h"

static bool
func_number_is_odd(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, (get_obj_number(wk, self) & 1) != 0);
	return true;
}

static bool
func_number_is_even(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, (get_obj_number(wk, self) & 1) == 0);
	return true;
}

static bool
func_number_to_string(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = make_strf(wk, "%" PRId64, get_obj_number(wk, self));
	return true;
}

const struct func_impl impl_tbl_number[] = {
	{ "to_string", func_number_to_string, tc_string },
	{ "is_even", func_number_is_even, tc_bool },
	{ "is_odd", func_number_is_odd, tc_bool },
	{ NULL, NULL },
};

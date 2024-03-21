/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "functions/array.h"
#include "functions/common.h"
#include "lang/typecheck.h"
#include "log.h"

static bool
func_array_length(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_number);
	set_obj_number(wk, *res, get_obj_array(wk, self)->len);
	return true;
}

static bool
func_array_get(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_number }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { tc_any }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	int64_t i = get_obj_number(wk, an[0].val);

	if (!bounds_adjust(get_obj_array(wk, self)->len, &i)) {
		if (ao[0].set) {
			*res = ao[0].val;
		} else {
			vm_error_at(wk, an[0].node, "index out of bounds");
			return false;
		}
	} else {
		obj_array_index(wk, self, i, res);
	}

	return true;
}

struct array_contains_ctx {
	obj item;
	bool found;
};

static enum iteration_result
array_contains_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct array_contains_ctx *ctx = _ctx;

	if (get_obj_type(wk, val) == obj_array) {
		obj_array_foreach(wk, val, ctx, array_contains_iter);
		if (ctx->found) {
			return ir_done;
		}
	}

	if (obj_equal(wk, val, ctx->item)) {
		ctx->found = true;
		return ir_done;
	}

	return ir_cont;
}

static bool
func_array_contains(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { tc_any }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	struct array_contains_ctx ctx = { .item = an[0].val };
	obj_array_foreach(wk, self, &ctx, array_contains_iter);

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, ctx.found);
	return true;
}

static bool
func_array_delete(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { tc_number }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	int64_t idx = get_obj_number(wk, an[0].val);
	if (!boundscheck(wk, an[0].node, get_obj_array(wk, self)->len, &idx)) {
		return false;
	}

	obj_array_del(wk, self, idx);
	return true;
}

const struct func_impl impl_tbl_array[] = {
	{ "length", func_array_length, tc_number, true },
	{ "get", func_array_get, tc_any, true },
	{ "contains", func_array_contains, tc_bool, true },
	{ NULL, NULL },
};

const struct func_impl impl_tbl_array_internal[] = {
	{ "length", func_array_length, tc_number, true },
	{ "get", func_array_get, tc_any, true },
	{ "contains", func_array_contains, tc_bool, true },
	{ "delete", func_array_delete, },
	{ NULL, NULL },
};

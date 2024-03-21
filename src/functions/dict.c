/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "functions/common.h"
#include "functions/dict.h"
#include "lang/typecheck.h"
#include "log.h"

static enum iteration_result
dict_keys_iter(struct workspace *wk, void *_ctx, obj k, obj v)
{
	obj *arr = _ctx;

	obj_array_push(wk, *arr, k);

	return ir_cont;
}

static bool
func_dict_keys(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_array);
	obj_dict_foreach(wk, self, res, dict_keys_iter);

	return true;
}

static bool
func_dict_has_key(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, obj_dict_in(wk, self, an[0].val));
	return true;
}

static bool
func_dict_get(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { tc_any }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	if (!obj_dict_index(wk, self, an[0].val, res)) {
		if (ao[0].set) {
			*res = ao[0].val;
		} else {
			vm_error_at(wk, an[0].node, "key not in dictionary: '%s'", get_cstr(wk, an[0].val));
			return false;
		}
	}
	return true;
}

static bool
func_dict_delete(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	obj_dict_del(wk, self, an[0].val);
	return true;
}

const struct func_impl impl_tbl_dict[] = {
	{ "keys", func_dict_keys, tc_array, true },
	{ "has_key", func_dict_has_key, tc_bool, true },
	{ "get", func_dict_get, tc_any, true },
	{ NULL, NULL },
};

const struct func_impl impl_tbl_dict_internal[] = {
	{ "keys", func_dict_keys, tc_array, true },
	{ "has_key", func_dict_has_key, tc_bool, true },
	{ "get", func_dict_get, tc_any, true },
	{ "delete", func_dict_delete },
	{ NULL, NULL },
};

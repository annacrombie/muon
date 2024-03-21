/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "lang/typecheck.h"
#include "functions/common.h"
#include "functions/configuration_data.h"
#include "log.h"

static bool
func_configuration_data_set_quoted(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_description, // TODO
	};
	struct args_kw akw[] = {
		[kw_description] = { "description", obj_string, },
		0
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	obj dict = get_obj_configuration_data(wk, rcvr)->dict;

	const char *s = get_cstr(wk, an[1].val);
	obj str = make_str(wk, "\"");

	for (; *s; ++s) {
		if (*s == '"') {
			str_app(wk, &str, "\\");
		}

		str_appn(wk, &str, s, 1);
	}

	str_app(wk, &str, "\"");

	obj_dict_set(wk, dict, an[0].val, str);
	return true;
}

static bool
func_configuration_data_set(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, { tc_string | tc_number | tc_bool }, ARG_TYPE_NULL };
	enum kwargs {
		kw_description, // ingnored
	};
	struct args_kw akw[] = {
		[kw_description] = { "description", obj_string, },
		0
	};
	if (!pop_args(wk, an, akw)) {
		return false;
	}

	obj dict = get_obj_configuration_data(wk, rcvr)->dict;

	obj_dict_set(wk, dict, an[0].val, an[1].val);

	return true;
}

static bool
func_configuration_data_set10(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, { obj_bool }, ARG_TYPE_NULL };
	enum kwargs {
		kw_description, // ignored
	};
	struct args_kw akw[] = {
		[kw_description] = { "description", obj_string, },
		0
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	obj dict = get_obj_configuration_data(wk, rcvr)->dict;

	obj n;
	make_obj(wk, &n, obj_number);
	set_obj_number(wk, n, get_obj_bool(wk, an[1].val) ? 1 : 0);
	obj_dict_set(wk, dict, an[0].val, n);

	return true;
}

static bool
configuration_data_get(struct workspace *wk, uint32_t err_node, obj conf,
	obj key, obj def, obj *res)
{
	obj dict = get_obj_configuration_data(wk, conf)->dict;

	if (!obj_dict_index(wk, dict, key, res)) {
		if (def) {
			*res = def;
		} else {
			vm_error_at(wk, err_node, "key '%s' not found", get_cstr(wk, key));
			return false;
		}
	}

	return true;
}

static bool
func_configuration_data_get(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { tc_any }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	return configuration_data_get(wk, an[0].node, rcvr, an[0].val, ao[0].val, res);
}

static bool
func_configuration_data_get_unquoted(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { tc_any }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	obj v;
	if (!configuration_data_get(wk, an[0].node, rcvr, an[0].val, ao[0].val, &v)) {
		return false;
	}

	const char *s = get_cstr(wk, v);
	uint32_t l = strlen(s);

	if (l >= 2 && s[0] == '"' && s[l - 1] == '"') {
		*res = make_strn(wk, &s[1], l - 2);
	} else {
		*res = v;
	}

	return true;
}

static enum iteration_result
obj_dict_keys_iter(struct workspace *wk, void *_ctx, obj k, obj _v)
{
	obj *res = _ctx;

	obj_array_push(wk, *res, k);

	return ir_cont;
}

static bool
func_configuration_data_keys(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	obj dict = get_obj_configuration_data(wk, rcvr)->dict;

	make_obj(wk, res, obj_array);
	obj_dict_foreach(wk, dict, res, obj_dict_keys_iter);
	return true;
}

static bool
func_configuration_data_has(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	obj _, dict = get_obj_configuration_data(wk, rcvr)->dict;
	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, obj_dict_index(wk, dict, an[0].val, &_));
	return true;
}

static bool
func_configuration_data_merge_from(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_configuration_data }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	obj_dict_merge_nodup(wk, get_obj_configuration_data(wk, rcvr)->dict,
		get_obj_configuration_data(wk, an[0].val)->dict
		);
	return true;
}

const struct func_impl impl_tbl_configuration_data[] = {
	{ "get", func_configuration_data_get, tc_any },
	{ "get_unquoted", func_configuration_data_get_unquoted, tc_any },
	{ "has", func_configuration_data_has, tc_bool },
	{ "keys", func_configuration_data_keys, tc_array },
	{ "merge_from", func_configuration_data_merge_from },
	{ "set", func_configuration_data_set },
	{ "set10", func_configuration_data_set10 },
	{ "set_quoted", func_configuration_data_set_quoted },
	{ NULL, NULL },
};

/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <stdlib.h>

#include "error.h"
#include "functions/common.h"
#include "functions/environment.h"
#include "lang/typecheck.h"
#include "log.h"
#include "platform/path.h"

static enum iteration_result
evironment_to_dict_iter(struct workspace *wk, void *_ctx, obj action)
{
	obj env = *(obj *)_ctx, mode_num, key, val, sep;

	obj_array_index(wk, action, 0, &mode_num);
	obj_array_index(wk, action, 1, &key);
	obj_array_index(wk, action, 2, &val);
	obj_array_index(wk, action, 3, &sep);

	enum environment_set_mode mode = get_obj_number(wk, mode_num);

	if (mode == environment_set_mode_set) {
		obj_dict_set(wk, env, key, val);
		return ir_cont;
	}

	const char *oval;
	obj v;
	if (obj_dict_index(wk, env, key, &v)) {
		oval = get_cstr(wk, v);
	} else {
		if (!(oval = getenv(get_cstr(wk, key)))) {
			obj_dict_set(wk, env, key, val);
			return ir_cont;
		}
	}

	obj str;

	switch (mode) {
	case environment_set_mode_append:
		str = make_strf(wk, "%s%s%s", oval, get_cstr(wk, sep), get_cstr(wk, val));
		break;
	case environment_set_mode_prepend:
		str = make_strf(wk, "%s%s%s", get_cstr(wk, val), get_cstr(wk, sep), oval);
		break;
	default:
		UNREACHABLE;
	}

	obj_dict_set(wk, env, key, str);
	return ir_cont;
}

bool
environment_to_dict(struct workspace *wk, obj env, obj *res)
{
	if (get_obj_type(wk, env) == obj_dict) {
		*res = env;
		return true;
	}

	make_obj(wk, res, obj_dict);

	return obj_array_foreach(wk, get_obj_environment(wk, env)->actions, res, evironment_to_dict_iter);
}

static void
environment_or_dict_set(struct workspace *wk, obj env, const char *key, const char *val)
{
	switch (get_obj_type(wk, env)) {
	case obj_dict:
		obj_dict_set(wk, env, make_str(wk, key), make_str(wk, val));
		break;
	case obj_environment:
		environment_set(wk, env, environment_set_mode_set, make_str(wk, key), make_str(wk, val), 0);
		break;
	default:
		UNREACHABLE;
	}
}

void
set_default_environment_vars(struct workspace *wk, obj env, bool set_subdir)
{
	if (wk->argv0) {
		// argv0 may not be set, e.g. during `muon install`
		environment_or_dict_set(wk, env, "MUON_PATH", wk->argv0);
	}
	environment_or_dict_set(wk, env, "MESON_BUILD_ROOT", wk->build_root);
	environment_or_dict_set(wk, env, "MESON_SOURCE_ROOT", wk->source_root);

	if (set_subdir) {
		SBUF(subdir);
		path_relative_to(wk, &subdir, wk->source_root, get_cstr(wk, current_project(wk)->cwd));
		environment_or_dict_set(wk, env, "MESON_SUBDIR", subdir.buf);
	}
}

bool
environment_set(struct workspace *wk, obj env, enum environment_set_mode mode, obj key, obj vals, obj sep)
{
	if (!sep) {
		sep = make_str(wk, ENV_PATH_SEP_STR);
	}

	obj joined;
	if (get_obj_type(wk, vals) == obj_string) {
		joined = vals;
	} else {
		if (!obj_array_join(wk, false, vals, sep, &joined)) {
			return false;
		}
	}

	obj elem, mode_num;
	make_obj(wk, &mode_num, obj_number);
	set_obj_number(wk, mode_num, mode);
	make_obj(wk, &elem, obj_array);
	obj_array_push(wk, elem, mode_num);
	obj_array_push(wk, elem, key);
	obj_array_push(wk, elem, joined);
	obj_array_push(wk, elem, sep);

	obj_array_push(wk, get_obj_environment(wk, env)->actions, elem);
	return true;
}

static bool
func_environment_set_common(struct workspace *wk, obj rcvr, uint32_t args_node, enum environment_set_mode mode)
{
	struct args_norm an[] = { { obj_string }, { TYPE_TAG_GLOB | obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_separator,
	};
	struct args_kw akw[] = {
		[kw_separator] = { "separator", obj_string },
		0
	};
	if (!pop_args(wk, an, akw)) {
		return false;
	}

	if (!get_obj_array(wk, an[1].val)->len) {
		vm_error_at(wk, an[1].node, "you must pass at least one value");
		return false;
	}

	return environment_set(wk, rcvr, mode, an[0].val, an[1].val, akw[kw_separator].val);
}

static bool
func_environment_set(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	return func_environment_set_common(wk, rcvr, args_node, environment_set_mode_set);
}

static bool
func_environment_append(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	return func_environment_set_common(wk, rcvr, args_node, environment_set_mode_append);
}

static bool
func_environment_prepend(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	return func_environment_set_common(wk, rcvr, args_node, environment_set_mode_prepend);
}

const struct func_impl impl_tbl_environment[] = {
	{ "set", func_environment_set },
	{ "append", func_environment_append },
	{ "prepend", func_environment_prepend },
	{ NULL, NULL },
};

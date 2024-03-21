/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "error.h"
#include "functions/common.h"
#include "functions/source_set.h"
#include "lang/typecheck.h"
#include "log.h"

static enum iteration_result
source_set_freeze_nested_iter(struct workspace *wk, void *_ctx, obj v)
{
	if (get_obj_type(wk, v) == obj_source_set) {
		get_obj_source_set(wk, v)->frozen = true;
	}
	return ir_cont;
}

static bool
source_set_add_rule(struct workspace *wk,
	obj self,
	struct args_norm *posargs,
	struct args_kw *kw_when,
	struct args_kw *kw_if_true,
	struct args_kw *kw_if_false)
{
	obj when = 0, if_true, if_false = 0;

	if (get_obj_array(wk, posargs->val)->len) {
		if (kw_when->set || kw_if_true->set || (kw_if_false && kw_if_false->set)) {
			vm_error_at(wk, posargs->node, "posargs not allowed when kwargs are used");
			return false;
		}

		if_true = posargs->val;
	} else {
		when = kw_when->val;
		if_true = kw_if_true->val;

		if (kw_if_false) {
			if_false = kw_if_false->val;
		}
	}

	if (if_true) {
		obj_array_foreach(wk, if_true, NULL, source_set_freeze_nested_iter);
	}

	obj rule;
	make_obj(wk, &rule, obj_array);
	obj_array_push(wk, rule, when);
	obj_array_push(wk, rule, if_true);
	obj_array_push(wk, rule, if_false);

	obj_array_push(wk, get_obj_source_set(wk, self)->rules, rule);
	return true;
}

static bool
source_set_check_not_frozen(struct workspace *wk, obj self)
{
	if (get_obj_source_set(wk, self)->frozen) {
		vm_error(wk, "cannot modify frozen source set");
		return false;
	}

	return true;
}

static bool
func_source_set_add(struct workspace *wk, obj self, obj *res)
{
	const type_tag tc_ss_sources = tc_string | tc_file | tc_custom_target | tc_generated_list;

	struct args_norm an[] = { { TYPE_TAG_GLOB | tc_ss_sources | tc_dependency }, ARG_TYPE_NULL };
	enum kwargs {
		kw_when,
		kw_if_true,
		kw_if_false,
	};
	struct args_kw akw[] = {
		[kw_when] = { "when", TYPE_TAG_LISTIFY | tc_string | tc_dependency },
		[kw_if_true] = { "if_true", TYPE_TAG_LISTIFY | tc_ss_sources | tc_dependency },
		[kw_if_false] = { "if_false", TYPE_TAG_LISTIFY | tc_ss_sources },
		0,
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	if (!source_set_check_not_frozen(wk, self)) {
		return false;
	}

	return source_set_add_rule(wk, self, &an[0], &akw[kw_when], &akw[kw_if_true], &akw[kw_if_false]);
}

static bool
func_source_set_add_all(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | tc_source_set }, ARG_TYPE_NULL };
	enum kwargs {
		kw_when,
		kw_if_true,
	};
	struct args_kw akw[] = { [kw_when] = { "when", TYPE_TAG_LISTIFY | tc_string | tc_dependency },
		[kw_if_true] = { "if_true", TYPE_TAG_LISTIFY | tc_source_set },
		0 };

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	if (!source_set_check_not_frozen(wk, self)) {
		return false;
	}

	return source_set_add_rule(wk, self, &an[0], &akw[kw_when], &akw[kw_if_true], NULL);
}

enum source_set_collect_mode {
	source_set_collect_sources,
	source_set_collect_dependencies,
};

struct source_set_collect_ctx {
	enum source_set_collect_mode mode;
	bool strict;

	obj conf;
	obj res;

	// for rule_match_iter
	uint32_t err_node;
	bool match;
};

static enum iteration_result source_set_collect_rules_iter(struct workspace *wk, void *_ctx, obj v);

static enum iteration_result
source_set_collect_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct source_set_collect_ctx *ctx = _ctx;

	switch (get_obj_type(wk, v)) {
	case obj_string:
	case obj_file:
	case obj_custom_target:
	case obj_generated_list:
		if (ctx->mode == source_set_collect_sources) {
			obj_array_push(wk, ctx->res, v);
		}
		break;
	case obj_dependency:
		if (ctx->mode == source_set_collect_dependencies) {
			obj_array_push(wk, ctx->res, v);
		}
		break;
	case obj_source_set:
		if (!obj_array_foreach(wk, get_obj_source_set(wk, v)->rules, ctx, source_set_collect_rules_iter)) {
			return ir_err;
		}
		break;
	default: UNREACHABLE;
	}

	return ir_cont;
}

static enum iteration_result
source_set_rule_match_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct source_set_collect_ctx *ctx = _ctx;

	enum obj_type t = get_obj_type(wk, v);
	if (!ctx->conf && t != obj_dependency) {
		return ir_cont;
	}

	switch (t) {
	case obj_dependency:
		if (!(get_obj_dependency(wk, v)->flags & dep_flag_found)) {
			ctx->match = false;
			return ir_done;
		}
		break;
	case obj_string: {
		obj idx;
		if (!obj_dict_index(wk, ctx->conf, v, &idx)) {
			if (ctx->strict) {
				vm_error_at(wk, ctx->err_node, "key %o not in configuration", v);
				return ir_err;
			}

			ctx->match = false;
			return ir_done;
		}

		bool bv = false;
		switch (get_obj_type(wk, idx)) {
		case obj_bool: bv = get_obj_bool(wk, idx); break;
		case obj_string: bv = get_str(wk, idx)->len > 0; break;
		case obj_number: bv = get_obj_number(wk, idx) > 0; break;
		default: UNREACHABLE;
		}

		if (!bv) {
			ctx->match = false;
			return ir_done;
		}
		break;
	}
	default: UNREACHABLE;
	}

	return ir_cont;
}

static enum iteration_result
source_set_collect_when_deps_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct source_set_collect_ctx *ctx = _ctx;
	if (get_obj_type(wk, v) == obj_dependency) {
		obj_array_push(wk, ctx->res, v);
	}
	return ir_cont;
}

static enum iteration_result
source_set_collect_rules_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct source_set_collect_ctx *ctx = _ctx;

	obj when, if_true, if_false;
	obj_array_index(wk, v, 0, &when);
	obj_array_index(wk, v, 1, &if_true);
	obj_array_index(wk, v, 2, &if_false);

	ctx->match = true;
	if (when && !obj_array_foreach_flat(wk, when, ctx, source_set_rule_match_iter)) {
		return ir_err;
	}

	if (ctx->match && if_true) {
		if (when && ctx->mode == source_set_collect_dependencies) {
			obj_array_foreach_flat(wk, when, ctx, source_set_collect_when_deps_iter);
		}

		obj_array_foreach_flat(wk, if_true, ctx, source_set_collect_iter);
	}

	if ((!ctx->conf || !ctx->match) && if_false) {
		obj_array_foreach_flat(wk, if_false, ctx, source_set_collect_iter);
	}
	return ir_cont;
}

static bool
source_set_collect(struct workspace *wk,
	uint32_t err_node,
	obj self,
	obj conf,
	enum source_set_collect_mode mode,
	bool strict,
	obj *res)
{
	obj arr;
	make_obj(wk, &arr, obj_array);
	struct source_set_collect_ctx ctx = {
		.mode = mode,
		.conf = conf,
		.strict = strict,
		.res = arr,
	};

	struct obj_source_set *ss = get_obj_source_set(wk, self);

	if (!obj_array_foreach(wk, ss->rules, &ctx, source_set_collect_rules_iter)) {
		return false;
	}

	obj_array_dedup(wk, arr, res);
	return true;
}

static bool
func_source_set_all_sources(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	return source_set_collect(wk, 0, self, 0, source_set_collect_sources, true, res);
}

static bool
func_source_set_all_dependencies(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	return source_set_collect(wk, 0, self, 0, source_set_collect_dependencies, true, res);
}

static bool
func_source_set_apply(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_configuration_data | tc_dict }, ARG_TYPE_NULL };
	enum kwargs {
		kw_strict,
	};
	struct args_kw akw[] = { [kw_strict] = { "strict", tc_bool }, 0 };

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	struct obj_source_set *ss = get_obj_source_set(wk, self);
	ss->frozen = true;

	obj dict = 0;
	switch (get_obj_type(wk, an[0].val)) {
	case obj_dict: dict = an[0].val; break;
	case obj_configuration_data: dict = get_obj_configuration_data(wk, an[0].val)->dict; break;
	default: UNREACHABLE;
	}

	bool strict = akw[kw_strict].set ? get_obj_bool(wk, akw[kw_strict].val) : true;

	make_obj(wk, res, obj_source_configuration);
	struct obj_source_configuration *sc = get_obj_source_configuration(wk, *res);

	if (!source_set_collect(
		    wk, akw[kw_strict].node, self, dict, source_set_collect_sources, strict, &sc->sources)) {
		return false;
	}

	if (!source_set_collect(
		    wk, akw[kw_strict].node, self, dict, source_set_collect_dependencies, strict, &sc->dependencies)) {
		return false;
	}

	return true;
}

const struct func_impl impl_tbl_source_set[] = {
	{ "add", func_source_set_add, 0, true },
	{ "add_all", func_source_set_add_all, 0, true },
	{ "all_sources", func_source_set_all_sources, tc_array, true },
	{ "all_dependencies", func_source_set_all_dependencies, tc_array, true },
	{ "apply", func_source_set_apply, tc_source_configuration, true },
	{ NULL, NULL },
};

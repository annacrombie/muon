/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "backend/output.h"
#include "embedded.h"
#include "error.h"
#include "lang/serial.h"
#include "lang/typecheck.h"
#include "lang/workspace.h"
#include "log.h"
#include "options.h"
#include "platform/filesystem.h"
#include "platform/path.h"

bool initializing_builtin_options = false;

static const char *build_option_type_to_s[build_option_type_count] = {
	[op_string] = "string",
	[op_boolean] = "boolean",
	[op_combo] = "combo",
	[op_integer] = "integer",
	[op_array] = "array",
	[op_feature] = "feature",
};

static bool set_option(struct workspace *wk, uint32_t node, obj opt, obj new_val,
	enum option_value_source source, bool coerce);

static bool
parse_config_string(struct workspace *wk, const struct str *ss, struct option_override *oo)
{
	if (str_has_null(ss)) {
		LOG_E("option cannot contain NUL");
		return false;
	}

	struct str subproject = { 0 }, key = { 0 }, val, cur = { 0 };

	cur.s = ss->s;
	bool reading_key = true, have_subproject = false;

	uint32_t i;
	for (i = 0; i < ss->len; ++i) {
		if (reading_key) {
			if (ss->s[i] == ':') {
				if (have_subproject) {
					LOG_E("multiple ':' in option '%s'", ss->s);
					return false;
				}

				have_subproject = true;
				subproject = cur;
				cur.s = &ss->s[i + 1];
				cur.len = 0;
				continue;
			} else if (ss->s[i] == '=') {
				key = cur;
				cur.s = &ss->s[i + 1];
				cur.len = 0;
				reading_key = false;
				continue;
			}
		}

		++cur.len;
	}

	val = cur;

	if (!key.len) {
		LOG_E("expected '=' in option '%s'", ss->s);
		return false;
	} else if (have_subproject && !subproject.len) {
		LOG_E("missing subproject in option '%s'", ss->s);
		return false;
	}

	oo->name = make_strn(wk, key.s, key.len);
	oo->val = make_strn(wk, val.s, val.len);
	if (have_subproject) {
		oo->proj = make_strn(wk, subproject.s, subproject.len);
	}

	return true;
}

static bool
subproj_name_matches(struct workspace *wk, const char *name, const char *test)
{
	if (test) {
		return name && strcmp(test, name) == 0;
	} else {
		return !name;
	}
}

static void
log_option_override(struct workspace *wk, struct option_override *oo)
{
	log_plain("'");
	if (oo->proj) {
		log_plain("%s:", get_cstr(wk, oo->proj));
	}

	obj_fprintf(wk, log_file(), "%s=%#o", get_cstr(wk, oo->name), oo->val);
	log_plain("'");
}

bool
check_invalid_subproject_option(struct workspace *wk)
{
	uint32_t i, j;
	struct option_override *oo;
	struct project *proj;
	bool found, ret = true;

	for (i = 0; i < wk->option_overrides.len; ++i) {
		oo = arr_get(&wk->option_overrides, i);
		if (!oo->proj || oo->source < option_value_source_commandline) {
			continue;
		}

		found = false;

		for (j = 1; j < wk->projects.len; ++j) {
			proj = arr_get(&wk->projects, j);
			if (proj->not_ok) {
				continue;
			}

			if (strcmp(get_cstr(wk, proj->subproject_name), get_cstr(wk, oo->proj)) == 0) {
				found = true;
				break;
			}
		}

		if (!found) {
			LLOG_E("invalid option: ");
			log_option_override(wk, oo);
			log_plain(" (no such subproject)\n");
			ret = false;
		}
	}

	return ret;
}

struct check_array_opt_ctx {
	obj choices;
	uint32_t node;
};

static enum iteration_result
check_array_opt_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct check_array_opt_ctx *ctx = _ctx;

	if (!obj_array_in(wk, ctx->choices, val)) {
		vm_error_at(wk, ctx->node, "array element %o is not one of %o", val, ctx->choices);
		return ir_err;
	}

	return ir_cont;
}

static bool
coerce_feature_opt(struct workspace *wk, uint32_t node, const struct str *val, obj *res)
{
	enum feature_opt_state f;

	if (str_eql(val, &WKSTR("auto"))) {
		f = feature_opt_auto;
	} else if (str_eql(val, &WKSTR("enabled"))) {
		f = feature_opt_enabled;
	} else if (str_eql(val, &WKSTR("disabled"))) {
		f = feature_opt_disabled;
	} else {
		vm_error_at(wk, node, "unable to coerce '%s' into a feature", val->s);
		return false;
	}

	make_obj(wk, res, obj_feature_opt);
	set_obj_feature_opt(wk, *res, f);
	return true;
}

struct check_deprecated_option_ctx {
	struct obj_option *opt;
	obj *val;
	obj sval;
	uint32_t err_node;
};

static enum iteration_result
check_deprecated_option_iter(struct workspace *wk, void *_ctx, obj old, obj new)
{
	struct check_deprecated_option_ctx *ctx = _ctx;

	switch (ctx->opt->type) {
	case op_array: {
		uint32_t idx;
		if (obj_array_index_of(wk, *ctx->val, old, &idx)) {
			vm_warning_at(wk, ctx->err_node, "option value %o is deprecated", old);

			if (new) {
				obj_array_set(wk, *ctx->val, idx, new);
			}
		}
		break;
	}
	default:
		if (str_eql(get_str(wk, ctx->sval), get_str(wk, old))) {
			vm_warning_at(wk, ctx->err_node, "option value %o is deprecated", old);

			if (new) {
				*ctx->val = new;
			}
		}
	}

	return ir_cont;
}

static bool
check_deprecated_option(struct workspace *wk, uint32_t err_node,
	struct obj_option *opt, obj sval, obj *val)
{
	struct check_deprecated_option_ctx ctx = {
		.val = val,
		.sval = sval,
		.opt = opt,
		.err_node = err_node
	};

	switch (get_obj_type(wk, opt->deprecated)) {
	case obj_bool:
		if (get_obj_bool(wk, opt->deprecated)) {
			vm_warning_at(wk, err_node, "option %o is deprecated", ctx.opt->name);
		}
		break;
	case obj_string: {
		struct project *cur_proj = current_project(wk);

		vm_warning_at(wk, err_node, "option %o is deprecated to %o", opt->name, opt->deprecated);

		obj newopt;
		if (get_option(wk, cur_proj, get_str(wk, opt->deprecated), &newopt)) {
			set_option(wk, err_node, newopt, sval, option_value_source_deprecated_rename, true);
		} else {
			struct option_override oo = {
				.proj = current_project(wk)->cfg.name,
				.name = opt->deprecated,
				.val = sval,
				.source = option_value_source_deprecated_rename,
			};

			arr_push(&wk->option_overrides, &oo);
		}
		break;
	}
	case obj_dict:
	case obj_array:
		obj_iterable_foreach(wk, opt->deprecated, &ctx, check_deprecated_option_iter);
		break;
	default:
		UNREACHABLE;
	}

	return true;
}

static bool
coerce_option_override(struct workspace *wk, uint32_t node, struct obj_option *opt, obj sval, obj *res)
{
	const struct str *val = get_str(wk, sval);
	*res = 0;
	if (opt->type == op_array) {
		// coerce array early so that its elements may be checked for deprecation
		if (!val->len) {
			// make -Doption= equivalent to an empty list
			make_obj(wk, res, obj_array);
		} else if (val->s[0] == '[') {
			if (!eval_str(wk, val->s, eval_mode_repl, res)) {
				LOG_E("malformed array option value '%s'", val->s);
				return false;
			}
		} else {
			*res = str_split(wk, val, &WKSTR(","));
		}
	}

	if (opt->deprecated) {
		if (!check_deprecated_option(wk, node, opt, sval, res)) {
			return false;
		}

		if (*res) {
			if (get_obj_type(wk, *res) == obj_string) {
				sval = *res;
				val = get_str(wk, *res);
			} else {
				return true;
			}
		}
	}

	switch (opt->type) {
	case op_combo:
	case op_string:
		*res = sval;
		break;
	case op_boolean: {
		bool b;
		if (str_eql(val, &WKSTR("true"))) {
			b = true;
		} else if (str_eql(val, &WKSTR("false"))) {
			b = false;
		} else {
			vm_error_at(wk, node, "unable to coerce '%s' into a boolean", val->s);
			return false;
		}

		make_obj(wk, res, obj_bool);
		set_obj_bool(wk, *res, b);
		break;
	}
	case op_integer: {
		int64_t num;
		char *endptr;
		num = strtol(val->s, &endptr, 10);

		if (!val->len || *endptr) {
			vm_error_at(wk, node, "unable to coerce '%s' into a number", val->s);
			return false;
		}

		make_obj(wk, res, obj_number);
		set_obj_number(wk, *res, num);
		break;
	}
	case op_array: {
		// do nothing, array values were already coerced above
		break;
	}
	case op_feature:
		return coerce_feature_opt(wk, node, val, res);
	default:
		UNREACHABLE;
	}

	return true;
}

static bool
typecheck_opt(struct workspace *wk, uint32_t err_node, obj val, enum build_option_type type, obj *res)
{
	enum obj_type expected_type;

	if (type == op_feature && get_obj_type(wk, val) == obj_string) {
		if (!coerce_feature_opt(wk, err_node, get_str(wk, val), res)) {
			return false;
		}

		val = *res;
	}

	switch (type) {
	case op_feature: expected_type = obj_feature_opt; break;
	case op_string: expected_type = obj_string; break;
	case op_boolean: expected_type = obj_bool; break;
	case op_combo: expected_type = obj_string; break;
	case op_integer: expected_type = obj_number; break;
	case op_array: expected_type = obj_array; break;
	default:
		UNREACHABLE_RETURN;
	}

	if (!typecheck(wk, err_node, val, expected_type)) {
		return false;
	}

	*res = val;
	return true;
}

struct prefix_dir_opts_ctx {
	const struct str *prefix;
};

static enum iteration_result
prefix_dir_opts_iter(struct workspace *wk, void *_ctx, obj _k, obj v)
{
	struct prefix_dir_opts_ctx *ctx = _ctx;

	struct obj_option *opt = get_obj_option(wk, v);

	if (opt->kind != build_option_kind_prefixed_dir) {
		return ir_cont;
	}

	const char *path = get_cstr(wk, opt->val);
	if (!path_is_absolute(path)) {
		return ir_cont;
	}

	SBUF(new_path);
	if (path_is_subpath(ctx->prefix->s, path)) {
		path_relative_to(wk, &new_path, ctx->prefix->s, path);
	} else {
		path_join(wk, &new_path, ctx->prefix->s, path);
	}

	opt->val = sbuf_into_str(wk, &new_path);
	return ir_cont;
}

bool
prefix_dir_opts(struct workspace *wk)
{
	obj prefix;
	get_option_value(wk, NULL, "prefix", &prefix);

	return obj_dict_foreach(wk, wk->global_opts, &(struct prefix_dir_opts_ctx) {
		.prefix = get_str(wk, prefix),
	}, prefix_dir_opts_iter);
}

static void
extend_array_option(struct workspace *wk, obj opt, obj new_val,
	enum option_value_source source)
{
	struct obj_option *o = get_obj_option(wk, opt);

	if (o->source > source) {
		return;
	}
	o->source = source;

	obj_array_extend_nodup(wk, o->val, new_val);
}

static bool
set_option(struct workspace *wk, uint32_t node, obj opt, obj new_val,
	enum option_value_source source, bool coerce)
{
	struct obj_option *o = get_obj_option(wk, opt);

	// Only set options that haven't set from a source with higher
	// precedence.  This means that option precedence doesn't have to rely
	// on the order in which options are set.  This means that e.g.
	// command-line set options can be set before the main meson.build file
	// has even been parsed.
	//
	// This mostly follows meson's behavior, except that deprecated options
	// cannot override command line options.

	/* { */
	/* 	const char *sourcenames[] = { */
	/* 		[option_value_source_unset] = "unset", */
	/* 		[option_value_source_default] = "default", */
	/* 		[option_value_source_environment] = "environment", */
	/* 		[option_value_source_default_options] = "default_options", */
	/* 		[option_value_source_subproject_default_options] = "subproject_default_options", */
	/* 		[option_value_source_yield] = "yield", */
	/* 		[option_value_source_commandline] = "commandline", */
	/* 		[option_value_source_deprecated_rename] = "deprecated rename", */
	/* 		[option_value_source_override_options] = "override_options", */
	/* 	}; */
	/* 	obj_fprintf(wk, log_file(), */
	/* 		"%s option %o to %o from %s, last set by %s\n", */
	/* 		o->source > source */
	/* 		? "\033[31mnot setting\033[0m" */
	/* 		: "\033[32msetting\033[0m", */
	/* 		o->name, new_val, sourcenames[source], sourcenames[o->source]); */
	/* } */

	if (o->source > source) {
		return true;
	}
	o->source = source;

	if (get_obj_type(wk, o->deprecated) == obj_bool && get_obj_bool(wk, o->deprecated)) {
		vm_warning_at(wk, node, "option %o is deprecated", o->name);
	}

	if (coerce) {
		obj coerced;
		if (!coerce_option_override(wk, node, o, new_val, &coerced)) {
			return false;
		}
		new_val = coerced;
	}

	if (!typecheck_opt(wk, node, new_val, o->type, &new_val)) {
		return false;
	}

	switch (o->type) {
	case op_combo: {
		if (!obj_array_in(wk, o->choices, new_val)) {
			vm_error_at(wk, node, "'%o' is not one of %o", new_val, o->choices);
			return false;
		}
		break;
	}
	case op_integer: {
		int64_t num = get_obj_number(wk, new_val);

		if ((o->max && num > get_obj_number(wk, o->max))
		    || (o->min && num < get_obj_number(wk, o->min)) ) {
			vm_error_at(wk, node, "value %" PRId64 " is out of range (%" PRId64 "..%" PRId64 ")",
				get_obj_number(wk, new_val),
				(o->min ? get_obj_number(wk, o->min) : INT64_MIN),
				(o->max ? get_obj_number(wk, o->max) : INT64_MAX)
				);
			return false;
		}
		break;
	}
	case op_array: {
		if (o->choices) {
			if (!obj_array_foreach(wk, new_val, &(struct check_array_opt_ctx) {
					.choices = o->choices,
					.node = node,
				}, check_array_opt_iter)) {
				return false;
			}
		}
		break;
	}
	case op_string:
	case op_feature:
	case op_boolean:
		break;
	default:
		UNREACHABLE_RETURN;
	}

	o->val = new_val;
	return true;
}

bool
create_option(struct workspace *wk, uint32_t node, obj opts, obj opt, obj val)
{
	if (!set_option(wk, node, opt, val, option_value_source_default, false)) {
		return false;
	}

	struct obj_option *o = get_obj_option(wk, opt);

	if (initializing_builtin_options) {
		o->builtin = true;
	}

	obj _;
	struct project *proj = NULL;
	if (wk->projects.len) {
		proj = current_project(wk);
	}

	const struct str *name = get_str(wk, o->name);
	if (str_has_null(name)
	    || strchr(name->s, ':')) {
		vm_error_at(wk, node, "invalid option name %o", o->name);
		return false;
	} else if (get_option(wk, proj, name, &_)) {
		vm_error_at(wk, node, "duplicate option %o", o->name);
		return false;
	}

	obj_dict_set(wk, opts, o->name, opt);
	return true;
}

bool
get_option_overridable(struct workspace *wk, const struct project *proj, obj overrides,
	const struct str *name, obj *res)
{
	if (overrides && obj_dict_index_strn(wk, overrides, name->s, name->len, res)) {
		return true;
	} else if (proj && obj_dict_index_strn(wk, proj->opts, name->s, name->len, res)) {
		return true;
	} else if (obj_dict_index_strn(wk, wk->global_opts, name->s, name->len, res)) {
		return true;
	} else {
		return false;
	}
}

bool
get_option(struct workspace *wk, const struct project *proj,
	const struct str *name, obj *res)
{
	return get_option_overridable(wk, proj, 0, name, res);
}

void
get_option_value_overridable(struct workspace *wk, const struct project *proj, obj overrides, const char *name, obj *res)
{
	obj opt;
	if (!get_option_overridable(wk, proj, overrides, &WKSTR(name), &opt)) {
		LOG_E("attempted to get unknown option '%s'", name);
		UNREACHABLE;
	}

	struct obj_option *o = get_obj_option(wk, opt);
	*res = o->val;
}

void
get_option_value(struct workspace *wk, const struct project *proj, const char *name, obj *res)
{
	get_option_value_overridable(wk, proj, 0, name, res);
}

static void
set_binary_from_env(struct workspace *wk, const char *envvar, const char *dest)
{
	obj opt;
	if (!get_option(wk, NULL, &WKSTR(dest), &opt)) {
		UNREACHABLE;
	}

	const char *v;
	if (!(v = getenv(envvar)) || !*v) {
		return;
	}

	// TODO: implement something like shlex.split()
	obj cmd = str_split(wk, &WKSTR(v), NULL);
	set_option(wk, 0, opt, cmd, option_value_source_environment, false);
}

static void
set_compile_opt_from_env(struct workspace *wk, const char *name, const char *flags, const char *extra)
{
	obj opt;
	if (!get_option(wk, NULL, &WKSTR(name), &opt)) {
		UNREACHABLE;
	}

	if ((flags = getenv(flags)) && *flags) {
		extend_array_option(wk, opt, str_split(wk, &WKSTR(flags), NULL),
			option_value_source_environment);
	}

	if ((extra = getenv(extra)) && *extra) {
		extend_array_option(wk, opt, str_split(wk, &WKSTR(extra), NULL),
			option_value_source_environment);
	}
}

static void
set_str_opt_from_env(struct workspace *wk, const char *env_name, const char *opt_name)
{
	obj opt;
	if (!get_option(wk, NULL, &WKSTR(opt_name), &opt)) {
		UNREACHABLE;
	}

	const char *env_val;
	if ((env_val = getenv(env_name)) && *env_val) {
		set_option(wk, 0, opt, make_str(wk, env_val), option_value_source_environment, false);
	}
}

static bool
init_builtin_options(struct workspace *wk, const char *script, const char *fallback)
{
	const char *opts;
	if (!(opts = embedded_get(script))) {
		opts = fallback;
	}

	enum language_mode old_mode = wk->vm.lang_mode;
	wk->vm.lang_mode = language_opts;
	obj _;
	initializing_builtin_options = true;
	bool ret = eval_str(wk, opts, eval_mode_default, &_);
	initializing_builtin_options = false;
	wk->vm.lang_mode = old_mode;
	return ret;
}

static bool
init_per_project_options(struct workspace *wk)
{
	return init_builtin_options(wk, "per_project_options.meson",
		"option('default_library', type: 'string', value: 'static')\n"
		"option('warning_level', type: 'string', value: '3')\n"
		"option('c_std', type: 'string', value: 'c99')\n"
		);
}

static enum iteration_result
set_yielding_project_options_iter(struct workspace *wk, void *_ctx, obj _k, obj opt)
{
	struct project *parent_proj = _ctx;
	struct obj_option *o = get_obj_option(wk, opt), *po;
	if (!o->yield) {
		return ir_cont;
	}

	obj parent_opt;
	if (!get_option(wk, parent_proj, get_str(wk, o->name), &parent_opt)) {
		return ir_cont;
	}

	po = get_obj_option(wk, parent_opt);
	if (po->type != o->type) {
		vm_warning_at(wk, 0,
			"option %o cannot yield to parent option due to a type mismatch (%s != %s)",
			o->name,
			build_option_type_to_s[po->type],
			build_option_type_to_s[o->type]
			);
		return ir_cont;
	}

	if (!set_option(wk, 0, opt, po->val, option_value_source_yield, false)) {
		return ir_err;
	}
	return ir_cont;
}

bool
setup_project_options(struct workspace *wk, const char *cwd)
{
	if (!init_per_project_options(wk)) {
		return false;
	}

	if (!cwd) {
		return true;
	}

	const char *option_file_names[] = {
		"meson.options",
		"meson_options.txt",
	};

	SBUF(meson_opts);

	bool exists = false;
	uint32_t i;
	for (i = 0; i < ARRAY_LEN(option_file_names); ++i) {
		path_join(wk, &meson_opts, cwd, option_file_names[i]);

		if (fs_file_exists(meson_opts.buf)) {
			exists = true;
			break;
		}
	}

	if (exists) {
		enum language_mode old_mode = wk->vm.lang_mode;
		wk->vm.lang_mode = language_opts;
		if (!wk->vm.behavior.eval_project_file(wk, meson_opts.buf, false)) {
			wk->vm.lang_mode = old_mode;
			return false;
		}
		wk->vm.lang_mode = old_mode;
	}

	bool is_master_project = wk->cur_project == 0;

	if (!is_master_project) {
		if (!obj_dict_foreach(wk, current_project(wk)->opts,
			arr_get(&wk->projects, 0),
			set_yielding_project_options_iter)) {
			return false;
		}
	}

	bool ret = true;
	struct option_override *oo;

	for (i = 0; i < wk->option_overrides.len; ++i) {
		oo = arr_get(&wk->option_overrides, i);

		if (!subproj_name_matches(wk, get_cstr(wk, current_project(wk)->subproject_name), get_cstr(wk, oo->proj))) {
			continue;
		}

		const struct str *name = get_str(wk, oo->name);
		obj opt;
		if (obj_dict_index_strn(wk, current_project(wk)->opts, name->s, name->len, &opt)
		    || (is_master_project && obj_dict_index_strn(wk, wk->global_opts, name->s, name->len, &opt))) {
			if (!set_option(wk, 0, opt, oo->val, oo->source, !oo->obj_value)) {
				ret = false;
			}
		} else {
			LLOG_E("invalid option: ");
			log_option_override(wk, oo);
			log_plain("\n");
			ret = false;
		}
	}

	return ret;
}

bool
init_global_options(struct workspace *wk)
{
	if (!init_builtin_options(wk, "global_options.meson",
		"option('buildtype', type: 'string', value: 'debugoptimized')\n"
		"option('prefix', type: 'string', value: '/usr/local')\n"
		"option('bindir', type: 'string', value: 'bin')\n"
		"option('mandir', type: 'string', value: 'share/man')\n"
		"option('datadir', type: 'string', value: 'share')\n"
		"option('libdir', type: 'string', value: 'lib')\n"
		"option('includedir', type: 'string', value: 'include')\n"
		"option('wrap_mode', type: 'string', value: 'nopromote')\n"
		"option('force_fallback_for', type: 'array', value: [])\n"
		"option('pkg_config_path', type: 'string', value: '')\n"
		"option('c_args', type: 'array', value: [])\n"
		"option('c_link_args', type: 'array', value: [])\n"
		"option('werror', type: 'boolean', value: false)\n"

		"option('env.CC', type: 'array', value: ['cc'])\n"
		"option('env.NINJA', type: 'array', value: ['ninja'])\n"
		"option('env.AR', type: 'array', value: ['ar'])\n"
		"option('env.LD', type: 'array', value: ['ld'])\n"
		)) {
		return false;
	}

	set_binary_from_env(wk, "CC", "env.CC");
	set_binary_from_env(wk, "NINJA", "env.NINJA");
	set_binary_from_env(wk, "AR", "env.AR");
	set_binary_from_env(wk, "LD", "env.LD");
	set_compile_opt_from_env(wk, "c_args", "CFLAGS", "CPPFLAGS");
	set_compile_opt_from_env(wk, "c_link_args", "CFLAGS", "LDFLAGS");

#ifdef MUON_BOOTSTRAPPED
	set_binary_from_env(wk, "CXX", "env.CXX");
	set_binary_from_env(wk, "OBJC", "env.OBJC");
	set_binary_from_env(wk, "NASM", "env.NASM");
	set_compile_opt_from_env(wk, "cpp_args", "CXXFLAGS", "CPPFLAGS");
	set_compile_opt_from_env(wk, "cpp_link_args", "CXXFLAGS", "LDFLAGS");
	set_str_opt_from_env(wk, "PKG_CONFIG_PATH", "pkg_config_path");
#endif

	return true;
}

bool
parse_and_set_cmdline_option(struct workspace *wk, char *lhs)
{
	struct option_override oo = { .source = option_value_source_commandline };
	if (!parse_config_string(wk, &WKSTR(lhs), &oo)) {
		return false;
	}

	arr_push(&wk->option_overrides, &oo);
	return true;
}

struct parse_and_set_default_options_ctx {
	uint32_t node;
	obj project_name;
	bool for_subproject;
};

static enum iteration_result
parse_and_set_default_options_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct parse_and_set_default_options_ctx *ctx = _ctx;

	struct option_override oo = { .source = option_value_source_default_options };
	if (!parse_config_string(wk, get_str(wk, v), &oo)) {
		vm_error_at(wk, ctx->node, "invalid option string");
		return ir_err;
	}

	bool oo_for_subproject = true;
	if (!oo.proj) {
		oo_for_subproject = false;
		oo.proj = ctx->project_name;
	}

	if (ctx->for_subproject || oo_for_subproject) {
		oo.source = option_value_source_subproject_default_options;
		arr_push(&wk->option_overrides, &oo);
		return ir_cont;
	}

	obj opt;
	if (get_option(wk, current_project(wk), get_str(wk, oo.name), &opt)) {
		if (!set_option(wk, ctx->node, opt, oo.val, option_value_source_default_options, true)) {
			return ir_err;
		}
	} else {
		LLOG_E("invalid option: ");
		log_option_override(wk, &oo);
		log_plain("\n");
		return ir_err;
	}

	return ir_cont;
}

bool
parse_and_set_default_options(struct workspace *wk, uint32_t err_node,
	obj arr, obj project_name, bool for_subproject)
{
	struct parse_and_set_default_options_ctx ctx = {
		.node = err_node,
		.project_name = project_name,
		.for_subproject = for_subproject,
	};

	if (!obj_array_foreach(wk, arr, &ctx, parse_and_set_default_options_iter)) {
		return false;
	}

	return true;
}

struct parse_and_set_override_options_ctx {
	uint32_t node;
	obj opts;
};

static enum iteration_result
parse_and_set_override_options_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct parse_and_set_override_options_ctx *ctx = _ctx;

	struct option_override oo = { .source = option_value_source_default_options };
	if (!parse_config_string(wk, get_str(wk, v), &oo)) {
		vm_error_at(wk, ctx->node, "invalid option string");
		return ir_err;
	}

	if (oo.proj) {
		vm_error_at(wk, ctx->node, "subproject options may not be set in override_options");
		return ir_err;
	}

	obj opt;
	if (!get_option(wk, current_project(wk), get_str(wk, oo.name), &opt)) {
		vm_error_at(wk, ctx->node, "invalid option %o in override_options", oo.name);
		return ir_err;
	}

	obj newopt;
	make_obj(wk, &newopt, obj_option);
	struct obj_option *o = get_obj_option(wk, newopt);
	*o = *get_obj_option(wk, opt);

	if (!set_option(wk, ctx->node, newopt, oo.val, option_value_source_override_options, true)) {
		return ir_err;
	}

	if (obj_dict_in(wk, ctx->opts, o->name)) {
		vm_error_at(wk, ctx->node, "duplicate option %o in override_options", oo.name);
		return ir_err;
	}

	obj_dict_set(wk, ctx->opts, o->name, newopt);
	return ir_cont;
}

bool
parse_and_set_override_options(struct workspace *wk, uint32_t err_node,
	obj arr, obj *res)
{
	struct parse_and_set_override_options_ctx ctx = {
		.node = err_node,
	};

	make_obj(wk, &ctx.opts, obj_dict);

	if (!obj_array_foreach(wk, arr, &ctx, parse_and_set_override_options_iter)) {
		return false;
	}

	*res = ctx.opts;
	return true;
}

/* helper functions */

enum wrap_mode
get_option_wrap_mode(struct workspace *wk)
{
	obj opt;
	get_option_value(wk, current_project(wk), "wrap_mode", &opt);

	const char *s = get_cstr(wk, opt);

	const char *names[] = {
		[wrap_mode_nopromote] = "nopromote",
		[wrap_mode_nodownload] = "nodownload",
		[wrap_mode_nofallback] = "nofallback",
		[wrap_mode_forcefallback] = "forcefallback",
		NULL,
	};

	uint32_t i;
	for (i = 0; names[i]; ++i) {
		if (strcmp(names[i], s) == 0) {
			return i;
		}
	}

	UNREACHABLE_RETURN;
}

enum tgt_type
get_option_default_library(struct workspace *wk)
{
	obj opt;
	get_option_value(wk, current_project(wk), "default_library", &opt);

	if (str_eql(get_str(wk, opt), &WKSTR("static"))) {
		return tgt_static_library;
	} else if (str_eql(get_str(wk, opt), &WKSTR("shared"))) {
		return tgt_dynamic_library;
	} else if (str_eql(get_str(wk, opt), &WKSTR("both"))) {
		return tgt_dynamic_library | tgt_static_library;
	} else {
		UNREACHABLE_RETURN;
	}
}

bool
get_option_bool(struct workspace *wk, obj overrides, const char *name, bool fallback)
{
	obj opt;
	if (get_option_overridable(wk, current_project(wk), overrides, &WKSTR(name), &opt)) {
		return get_obj_bool(wk, get_obj_option(wk, opt)->val);
	} else {
		return fallback;
	}
}

/* options listing subcommand */

struct make_option_choices_ctx {
	obj selected;
	const char *val_clr, *sel_clr, *no_clr;
	uint32_t i, len;
	obj res;
};

static enum iteration_result
make_option_choices_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct make_option_choices_ctx *ctx = _ctx;

	const struct str *ss = get_str(wk, val);

	const char *clr = ctx->val_clr;
	if (ctx->selected && obj_array_in(wk, ctx->selected, val)) {
		clr = ctx->sel_clr;
	}

	str_app(wk, &ctx->res, clr);
	str_appn(wk, &ctx->res, ss->s, ss->len);
	str_app(wk, &ctx->res, ctx->no_clr);

	if (ctx->i < ctx->len - 1) {
		str_app(wk, &ctx->res, "|");
	}

	++ctx->i;
	return ir_cont;
}

struct list_options_ctx {
	bool show_builtin;
	const struct list_options_opts *list_opts;
};

static enum iteration_result
list_options_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	struct list_options_ctx *ctx = _ctx;
	struct obj_option *opt = get_obj_option(wk, val);

	if (opt->builtin != ctx->show_builtin) {
		return ir_cont;
	} else if (ctx->list_opts->only_modified && opt->source == option_value_source_default) {
		return ir_cont;
	}

	/* const char *option_type_names[] = { */
	/* 	[op_string] = "string", */
	/* 	[op_boolean] = "boolean", */
	/* 	[op_combo] = "combo", */
	/* 	[op_integer] = "integer", */
	/* 	[op_array] = "array", */
	/* 	[op_feature] = "feature", */
	/* }; */

	const char *key_clr = "", *val_clr = "", *sel_clr = "", *no_clr = "";

	if (fs_is_a_tty(stdout)) {
		key_clr = "\033[1;34m";
		val_clr = "\033[1;37m";
		sel_clr = "\033[1;32m";
		no_clr = "\033[0m";
	}

	obj_fprintf(wk, log_file(), "  %s%#o%s=", key_clr, key, no_clr);

	obj choices = 0;
	obj selected = 0;

	if (opt->type == op_combo) {
		choices = opt->choices;
		make_obj(wk, &selected, obj_array);
		obj_array_push(wk, selected, opt->val);
	} else if (opt->type == op_array && opt->choices) {
		choices = opt->choices;
		selected = opt->val;
	} else {
		make_obj(wk, &choices, obj_array);
		switch (opt->type) {
		case op_string:
			obj_array_push(wk, choices, make_str(wk, "string"));
			break;
		case op_boolean:
			obj_array_push(wk, choices, make_str(wk, "true"));
			obj_array_push(wk, choices, make_str(wk, "false"));
			make_obj(wk, &selected, obj_array);
			obj_array_push(wk, selected, make_str(wk, get_obj_bool(wk, opt->val) ? "true" : "false"));
			break;
		case op_feature:
			obj_array_push(wk, choices, make_str(wk, "enabled"));
			obj_array_push(wk, choices, make_str(wk, "disabled"));
			obj_array_push(wk, choices, make_str(wk, "auto"));
			make_obj(wk, &selected, obj_array);
			obj_array_push(wk, selected, make_str(wk, (char *[]){
				[feature_opt_enabled] = "enabled",
				[feature_opt_disabled] = "disabled",
				[feature_opt_auto] = "auto",
			}[get_obj_feature_opt(wk, opt->val)]));
			break;
		case op_combo:
		case op_array:
		case op_integer:
			break;
		default:
			UNREACHABLE;
		}
	}

	if (choices) {
		struct make_option_choices_ctx ctx = {
			.len = get_obj_array(wk, choices)->len,
			.val_clr = val_clr,
			.no_clr = no_clr,
			.sel_clr = sel_clr,
			.selected = selected,
			.res = make_str(wk, ""),
		};

		obj_array_foreach(wk, choices, &ctx, make_option_choices_iter);
		choices = ctx.res;
	}

	switch (opt->type) {
	case op_boolean:
	case op_combo:
	case op_feature:
		obj_fprintf(wk, log_file(), "<%s>", get_cstr(wk, choices));
		break;
	case op_string: {
		const struct str *def = get_str(wk, opt->val);
		obj_fprintf(wk, log_file(), "<%s>, default: %s%s%s", get_cstr(wk, choices),
			sel_clr, def->len ? def->s : "''", no_clr);
		break;
	}
	case op_integer:
		log_plain("<%sN%s>", val_clr, no_clr);
		if (opt->min || opt->max) {
			log_plain(" where ");
			if (opt->min) {
				obj_fprintf(wk, log_file(), "%o <= ", opt->min);
			}
			log_plain("%sN%s", val_clr, no_clr);
			if (opt->max) {
				obj_fprintf(wk, log_file(), " <= %o", opt->max);
			}
		}
		obj_fprintf(wk, log_file(), ", default: %s%o%s", sel_clr, opt->val, no_clr);

		break;
	case op_array:
		log_plain("<%svalue%s[,%svalue%s[...]]>", val_clr, no_clr, val_clr, no_clr);
		if (opt->choices) {
			obj_fprintf(wk, log_file(), " where value in %s", get_cstr(wk, choices));
		}
		break;
	default:
		UNREACHABLE;
	}

	if (opt->source != option_value_source_default) {
		obj_fprintf(wk, log_file(), "*");
	}

	if (opt->description) {
		obj_fprintf(wk, log_file(), "\n    %#o", opt->description);
	}

	log_plain("\n");
	return ir_cont;
}

bool
list_options(const struct list_options_opts *list_opts)
{
	bool ret = false;
	struct workspace wk = { 0 };
	workspace_init(&wk);
	wk.vm.lang_mode = language_opts;

	arr_push(&wk.projects, &(struct project){ 0 });
	struct project *proj = arr_get(&wk.projects, 0);
	make_obj(&wk, &proj->opts, obj_dict);

	if (fs_file_exists("meson.build")) {
		SBUF(meson_opts);
		path_make_absolute(&wk, &meson_opts, "meson_options.txt");

		if (fs_file_exists(meson_opts.buf)) {
			if (!wk.vm.behavior.eval_project_file(&wk, meson_opts.buf, false)) {
				goto ret;
			}
		}

		if (list_opts->list_all) {
			make_obj(&wk, &current_project(&wk)->opts, obj_dict);
			if (!init_per_project_options(&wk)) {
				goto ret;
			}
		}
	} else {
		SBUF(option_info);
		path_join(&wk, &option_info, output_path.private_dir, output_path.option_info);
		if (!fs_file_exists(option_info.buf)) {
			LOG_I("run this command must be run from a build directory or the project root");
			goto ret;
		}

		obj arr;
		if (!serial_load_from_private_dir(&wk, &arr, output_path.option_info)) {
			goto ret;
		}

		obj_array_index(&wk, arr, 0, &wk.global_opts);
		obj_array_index(&wk, arr, 1, &current_project(&wk)->opts);
	}

	struct list_options_ctx ctx = { .list_opts = list_opts };

	if (get_obj_dict(&wk, current_project(&wk)->opts)->len) {
		log_plain("project options:\n");
		obj_dict_foreach(&wk, current_project(&wk)->opts, &ctx, list_options_iter);
	} else if (!list_opts->list_all) {
		log_plain("no project options defined\n");
	}

	if (list_opts->list_all) {
		if (get_obj_dict(&wk, current_project(&wk)->opts)->len) {
			log_plain("\n");
		}

		ctx.show_builtin = true;

		log_plain("project builtin-options:\n");
		obj_dict_foreach(&wk, current_project(&wk)->opts, &ctx, list_options_iter);
		log_plain("\n");

		log_plain("global options:\n");
		obj_dict_foreach(&wk, wk.global_opts, &ctx, list_options_iter);
	}

	ret = true;
ret:
	workspace_destroy(&wk);
	return ret;
}

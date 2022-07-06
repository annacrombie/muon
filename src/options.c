#include "posix.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "embedded.h"
#include "error.h"
#include "lang/interpreter.h"
#include "lang/workspace.h"
#include "log.h"
#include "options.h"
#include "platform/filesystem.h"
#include "platform/path.h"

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

	struct str subproject = { 0 }, key = { 0 }, val = { 0 }, cur = { 0 };

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

	if (!val.len) {
		LOG_E("expected '=' in option '%s'", ss->s);
		return false;
	} else if (!key.len) {
		LOG_E("missing option name in option '%s'", ss->s);
		return false;
	} else if (have_subproject && !subproject.len) {
		LOG_E("missing subproject in option '%s'", ss->s);
		return false;
	}

	oo->name = make_strn(wk, key.s, key.len);
	oo->val = make_strn(wk, val.s, val.len);
	if (have_subproject) {
		oo->proj = make_strn(wk, subproject.s, subproject.len);
		obj_fprintf(wk, log_file(), "subproject option override: %o:%o=%o\n", oo->proj, oo->name, oo->val);
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

static const char *
option_override_to_s(struct workspace *wk, struct option_override *oo)
{
	static char buf[BUF_SIZE_2k + 1] = { 0 };
	char buf1[BUF_SIZE_2k / 2];

	const char *val;

	if (oo->obj_value) {
		obj_to_s(wk, oo->val, buf1, BUF_SIZE_2k / 2);
		val = buf1;
	} else {
		val = get_cstr(wk, oo->val);
	}

	snprintf(buf, BUF_SIZE_2k, "%s%s%s=%s",
		oo->proj ? get_cstr(wk, oo->proj) : "",
		oo->proj ? ":" : "",
		get_cstr(wk, oo->name),
		val
		);

	return buf;
}

bool
check_invalid_subproject_option(struct workspace *wk)
{
	uint32_t i, j;
	struct option_override *oo;
	struct project *proj;
	bool found, ret = true;

	for (i = 0; i < wk->option_overrides.len; ++i) {
		oo = darr_get(&wk->option_overrides, i);
		if (!oo->proj) {
			continue;
		}

		found = false;

		for (j = 1; j < wk->projects.len; ++j) {
			proj = darr_get(&wk->projects, j);
			if (proj->not_ok) {
				continue;
			}

			if (strcmp(get_cstr(wk, proj->subproject_name), get_cstr(wk, oo->proj)) == 0) {
				found = true;
				break;
			}
		}

		if (!found) {
			LOG_E("invalid option: '%s' (no such subproject)", option_override_to_s(wk, oo));
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
		interp_error(wk, ctx->node, "array element %o is not one of %o", val, ctx->choices);
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
		interp_error(wk, node, "unable to coerce '%s' into a feature", val->s);
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
			interp_warning(wk, ctx->err_node, "option value %o is deprecated", old);

			if (new) {
				obj_array_set(wk, *ctx->val, idx, new);
			}
		}
		break;
	}
	default:
		if (str_eql(get_str(wk, ctx->sval), get_str(wk, old))) {
			interp_warning(wk, ctx->err_node, "option value %o is deprecated", old);

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
			interp_warning(wk, err_node, "option %o is deprecated", ctx.opt->name);
		}
		break;
	case obj_string: {
		struct project *cur_proj = current_project(wk);

		interp_warning(wk, err_node, "option %o is deprecated to %o", opt->name, opt->deprecated);

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

			darr_push(&wk->option_overrides, &oo);
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
			interp_error(wk, node, "unable to coerce '%s' into a boolean", val->s);
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
			interp_error(wk, node, "unable to coerce '%s' into a number", val->s);
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
		interp_warning(wk, node, "option %o is deprecated", o->name);
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
			interp_error(wk, node, "'%o' is not one of %o", new_val, o->choices);
			return false;
		}
		break;
	}
	case op_integer: {
		int64_t num = get_obj_number(wk, new_val);

		if ((o->max && num > get_obj_number(wk, o->max))
		    || (o->min && num < get_obj_number(wk, o->min)) ) {
			interp_error(wk, node, "value %" PRId64 " is out of range (%" PRId64 "..%" PRId64 ")",
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
	obj _;
	struct project *proj = NULL;
	if (wk->projects.len) {
		proj = current_project(wk);
	}

	const struct str *name = get_str(wk, o->name);
	if (str_has_null(name)
	    || strchr(name->s, ':')) {
		interp_error(wk, node, "invalid option name %o", o->name);
		return false;
	} else if (get_option(wk, proj, name, &_)) {
		interp_error(wk, node, "duplicate option %o", o->name);
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
set_compile_opt_from_env(struct workspace *wk, const char *name, const char *flags, const char *extra)
{
#ifndef MUON_BOOTSTRAPPED
	return;
#endif
	obj opt;
	if (!get_option(wk, NULL, &WKSTR(name), &opt)) {
		UNREACHABLE;
	}

	struct obj_option *o = get_obj_option(wk, opt);

	flags = getenv(flags);
	extra = getenv(extra);
	if (flags && *flags) {
		o->val = str_split(wk, &WKSTR(flags), NULL);

		if (extra && *extra) {
			obj_array_extend(wk, o->val, str_split(wk, &WKSTR(extra), NULL));
		}
	} else if (extra && *extra) {
		o->val = str_split(wk, &WKSTR(extra), NULL);
	}
}

static bool
init_builtin_options(struct workspace *wk, const char *script, const char *fallback)
{
	const char *opts;
	if (!(opts = embedded_get(script))) {
		opts = fallback;
	}

	enum language_mode old_mode = wk->lang_mode;
	wk->lang_mode = language_opts;
	obj _;
	bool ret = eval_str(wk, opts, eval_mode_default, &_);
	wk->lang_mode = old_mode;
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
		interp_warning(wk, 0,
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

	char meson_opts[PATH_MAX];
	if (!path_join(meson_opts, PATH_MAX, cwd, "meson_options.txt")) {
		return false;
	}

	if (fs_file_exists(meson_opts)) {
		enum language_mode old_mode = wk->lang_mode;
		wk->lang_mode = language_opts;
		if (!wk->eval_project_file(wk, meson_opts)) {
			return false;
		}
		wk->lang_mode = old_mode;
	}

	bool is_master_project = wk->cur_project == 0;

	if (!is_master_project) {
		if (!obj_dict_foreach(wk, current_project(wk)->opts,
			darr_get(&wk->projects, 0),
			set_yielding_project_options_iter)) {
			return false;
		}
	}

	bool ret = true;
	uint32_t i;
	struct option_override *oo;

	for (i = 0; i < wk->option_overrides.len; ++i) {
		oo = darr_get(&wk->option_overrides, i);

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
			LOG_E("invalid option: '%s'", option_override_to_s(wk, oo));
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
		"option('wrap_mode', type: 'string', value: 'nopromote')\n"
		"option('force_fallback_for', type: 'array', value: [])\n"
		"option('pkg_config_path', type: 'string', value: '')\n"
		)) {
		return false;
	}

	set_compile_opt_from_env(wk, "c_args", "CFLAGS", "CPPFLAGS");
	set_compile_opt_from_env(wk, "c_link_args", "CFLAGS", "LDFLAGS");
	set_compile_opt_from_env(wk, "cpp_args", "CXXFLAGS", "CPPFLAGS");
	set_compile_opt_from_env(wk, "cpp_link_args", "CXXFLAGS", "LDFLAGS");
	return true;
}

bool
parse_and_set_cmdline_option(struct workspace *wk, char *lhs)
{
	struct option_override oo = { .source = option_value_source_commandline };
	if (!parse_config_string(wk, &WKSTR(lhs), &oo)) {
		return false;
	}

	darr_push(&wk->option_overrides, &oo);
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
		interp_error(wk, ctx->node, "invalid option string");
		return ir_err;
	}

	bool oo_for_subproject = true;
	if (!oo.proj) {
		oo_for_subproject = false;
		oo.proj = ctx->project_name;
	}

	if (ctx->for_subproject || oo_for_subproject) {
		oo.source = option_value_source_subproject_default_options;
		darr_push(&wk->option_overrides, &oo);
		return ir_cont;
	}

	obj opt;
	if (get_option(wk, current_project(wk), get_str(wk, oo.name), &opt)) {
		if (!set_option(wk, ctx->node, opt, oo.val, option_value_source_default_options, true)) {
			return ir_err;
		}
	} else {
		LOG_E("invalid option: '%s'", option_override_to_s(wk, &oo));
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
		interp_error(wk, ctx->node, "invalid option string");
		return ir_err;
	}

	if (oo.proj) {
		interp_error(wk, ctx->node, "subproject options may not be set in override_options");
		return ir_err;
	}

	obj opt;
	if (!get_option(wk, current_project(wk), get_str(wk, oo.name), &opt)) {
		interp_error(wk, ctx->node, "invalid option %o in override_options", oo.name);
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
		interp_error(wk, ctx->node, "duplicate option %o in override_options", oo.name);
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

	str_app(wk, ctx->res, clr);
	str_appn(wk, ctx->res, ss->s, ss->len);
	str_app(wk, ctx->res, ctx->no_clr);

	if (ctx->i < ctx->len - 1) {
		str_app(wk, ctx->res, "|");
	}

	++ctx->i;
	return ir_cont;
}

static enum iteration_result
list_options_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	struct obj_option *opt = get_obj_option(wk, val);
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

	obj_printf(wk, "  %s%#o%s=", key_clr, key, no_clr);

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
		obj_printf(wk, "<%s>", get_cstr(wk, choices));
		break;
	case op_string: {
		const struct str *def = get_str(wk, opt->val);
		obj_printf(wk, "<%s>, default: %s%s%s", get_cstr(wk, choices),
			sel_clr, def->len ? def->s : "''", no_clr);
		break;
	}
	case op_integer:
		printf("<%sN%s>", val_clr, no_clr);
		if (opt->min || opt->max) {
			printf(" where ");
			if (opt->min) {
				obj_printf(wk, "%o <= ", opt->min);
			}
			printf("%sN%s", val_clr, no_clr);
			if (opt->max) {
				obj_printf(wk, " <= %o", opt->max);
			}
		}
		obj_printf(wk, ", default: %s%o%s", sel_clr, opt->val, no_clr);

		break;
	case op_array:
		printf("<%svalue%s[,%svalue%s[...]]>", val_clr, no_clr, val_clr, no_clr);
		if (opt->choices) {
			obj_printf(wk, " where value in %s", get_cstr(wk, choices));
		}
		break;
	default:
		UNREACHABLE;
	}

	if (opt->description) {
		obj_printf(wk, "\n    %#o", opt->description);
	}

	printf("\n");
	return ir_cont;
}

bool
list_options(const struct list_options_opts *list_opts)
{
	bool ret = false;
	struct workspace wk = { 0 };
	workspace_init(&wk);
	wk.lang_mode = language_opts;

	darr_push(&wk.projects, &(struct project){ 0 });
	struct project *proj = darr_get(&wk.projects, 0);
	make_obj(&wk, &proj->opts, obj_dict);

	char meson_opts[PATH_MAX];
	if (!path_make_absolute(meson_opts, PATH_MAX, "meson_options.txt")) {
		goto ret;
	}

	if (fs_file_exists(meson_opts)) {
		if (!wk.eval_project_file(&wk, meson_opts)) {
			goto ret;
		}
	}

	if (get_obj_dict(&wk, current_project(&wk)->opts)->len) {
		printf("project options:\n");
		obj_dict_foreach(&wk, current_project(&wk)->opts, NULL, list_options_iter);
	}

	if (list_opts->list_all) {
		if (get_obj_dict(&wk, current_project(&wk)->opts)->len) {
			printf("\n");
		}

		make_obj(&wk, &current_project(&wk)->opts, obj_dict);
		if (!init_per_project_options(&wk)) {
			goto ret;
		}
		printf("project builtin-options:\n");
		obj_dict_foreach(&wk, current_project(&wk)->opts, NULL, list_options_iter);
		printf("\n");

		printf("global options:\n");
		obj_dict_foreach(&wk, wk.global_opts, NULL, list_options_iter);
	}

	ret = true;
ret:
	workspace_destroy(&wk);
	return ret;
}

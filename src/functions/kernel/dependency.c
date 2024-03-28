/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: illiliti <illiliti@dimension.sh>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "buf_size.h"
#include "coerce.h"
#include "error.h"
#include "external/libpkgconf.h"
#include "functions/common.h"
#include "functions/kernel/dependency.h"
#include "functions/kernel/subproject.h"
#include "functions/machine.h"
#include "functions/string.h"
#include "functions/subproject.h"
#include "lang/interpreter.h"
#include "lang/typecheck.h"
#include "log.h"
#include "options.h"
#include "platform/filesystem.h"
#include "platform/path.h"
#include "platform/run_cmd.h"

enum dependency_lookup_method {
	// Auto means to use whatever dependency checking mechanisms in whatever order meson thinks is best.
	dependency_lookup_method_auto,
	dependency_lookup_method_pkgconfig,
	dependency_lookup_method_cmake,
	// The dependency is provided by the standard library and does not need to be linked
	dependency_lookup_method_builtin,
	// Just specify the standard link arguments, assuming the operating system provides the library.
	dependency_lookup_method_system,
	// This is only supported on OSX - search the frameworks directory by name.
	dependency_lookup_method_extraframework,
	// Detect using the sysconfig module.
	dependency_lookup_method_sysconfig,
	// Specify using a "program"-config style tool
	dependency_lookup_method_config_tool,
	// Misc
	dependency_lookup_method_dub,
};

enum dep_lib_mode {
	dep_lib_mode_default,
	dep_lib_mode_static,
	dep_lib_mode_shared,
};

struct dep_lookup_ctx {
	obj *res;
	struct args_kw *default_options, *versions;
	enum requirement_type requirement;
	uint32_t err_node;
	uint32_t fallback_node;
	obj name;
	obj names;
	obj fallback;
	obj not_found_message;
	obj modules;
	enum dep_lib_mode lib_mode;
	bool disabler;
	bool fallback_allowed;
	bool fallback_only;
	bool from_cache;
	bool found;
};

static enum iteration_result
check_dependency_override_iter(struct workspace *wk, void *_ctx, obj n)
{
	struct dep_lookup_ctx *ctx = _ctx;

	if (ctx->lib_mode != dep_lib_mode_shared) {
		if (obj_dict_index(wk, wk->dep_overrides_static, n, ctx->res)) {
			ctx->lib_mode = dep_lib_mode_static;
			ctx->found = true;
			return ir_done;
		}
	}

	if (ctx->lib_mode != dep_lib_mode_static) {
		if (obj_dict_index(wk, wk->dep_overrides_dynamic, n, ctx->res)) {
			ctx->lib_mode = dep_lib_mode_shared;
			ctx->found = true;
			return ir_done;
		}
	}

	return ir_cont;
}

static bool
check_dependency_override(struct workspace *wk, struct dep_lookup_ctx *ctx)
{
	obj_array_foreach(wk, ctx->names, ctx, check_dependency_override_iter);

	return ctx->found;
}

static bool
check_dependency_cache(struct workspace *wk, struct dep_lookup_ctx *ctx, obj *res)
{
	if (ctx->lib_mode != dep_lib_mode_shared) {
		if (obj_dict_index(wk, current_project(wk)->dep_cache.static_deps, ctx->name, res)) {
			ctx->lib_mode = dep_lib_mode_static;
			return true;
		}
	}

	if (ctx->lib_mode != dep_lib_mode_static) {
		if (obj_dict_index(wk, current_project(wk)->dep_cache.shared_deps, ctx->name, res)) {
			ctx->lib_mode = dep_lib_mode_shared;
			return true;
		}
	}

	return false;
}

static bool
check_dependency_version(struct workspace *wk, obj dep_ver_str, uint32_t err_node, obj ver, bool *res)
{
	if (!ver) {
		*res = true;
		return true;
	}

	if (!version_compare(wk, err_node, get_str(wk, dep_ver_str), ver, res)) {
		return false;
	}

	return true;
}

static bool
handle_dependency_fallback(struct workspace *wk, struct dep_lookup_ctx *ctx, bool *found)
{
	if (get_option_wrap_mode(wk) == wrap_mode_nofallback) {
		return true;
	}

	obj subproj_name, subproj_dep = 0, subproj;

	switch (get_obj_array(wk, ctx->fallback)->len) {
	case 2:
		obj_array_index(wk, ctx->fallback, 1, &subproj_dep);
	/* FALLTHROUGH */
	case 1:
		obj_array_index(wk, ctx->fallback, 0, &subproj_name);
		break;
	default:
		interp_error(wk, ctx->err_node, "expected array of length 1-2 for fallback");
		return false;
	}

	if (ctx->lib_mode != dep_lib_mode_default) {
		obj libopt;
		if (ctx->lib_mode == dep_lib_mode_static) {
			libopt = make_str(wk, "default_library=static");
		} else {
			libopt = make_str(wk, "default_library=shared");
		}

		if (ctx->default_options->set) {
			if (!obj_array_in(wk, ctx->default_options->val, libopt)) {
				obj newopts;
				obj_array_dup(wk, ctx->default_options->val, &newopts);
				obj_array_push(wk, newopts, libopt);

				ctx->default_options->val = newopts;
			}
		} else {
			make_obj(wk, &ctx->default_options->val, obj_array);
			obj_array_push(wk, ctx->default_options->val, libopt);
			ctx->default_options->set = true;
		}
	}

	if (!subproject(wk, subproj_name, ctx->requirement, ctx->default_options, ctx->versions, &subproj)) {
		goto not_found;
	}

	if (!get_obj_subproject(wk, subproj)->found) {
		goto not_found;
	}

	if (subproj_dep) {
		if (!subproject_get_variable(wk, ctx->fallback_node, subproj_dep, 0, subproj, ctx->res)) {
			interp_warning(wk, ctx->fallback_node, "subproject dependency variable %o is not defined", subproj_dep);
			goto not_found;
		}
	} else {
		if (!check_dependency_override(wk, ctx)) {
			interp_warning(wk, ctx->fallback_node, "subproject does not override dependency %o", ctx->name);
			goto not_found;
		}
	}

	if (get_obj_type(wk, *ctx->res) != obj_dependency) {
		interp_warning(wk, ctx->fallback_node, "overridden dependency is not a dependency object");
		goto not_found;
	}

	*found = true;
	return true;
not_found:
	obj_fprintf(wk, log_file(), "fallback %o failed for %o\n", ctx->fallback, ctx->name);
	*ctx->res = 0;
	*found = false;
	return true;
}

static bool
get_dependency_pkgconfig(struct workspace *wk, struct dep_lookup_ctx *ctx, bool *found)
{
	struct pkgconf_info info = { 0 };
	*found = false;

	if (!muon_pkgconf_lookup(wk, ctx->name, ctx->lib_mode == dep_lib_mode_static, &info)) {
		return true;
	}

	obj ver_str = make_str(wk, info.version);
	bool ver_match;
	if (!check_dependency_version(wk, ver_str, ctx->err_node, ctx->versions->val, &ver_match)) {
		return false;
	} else if (!ver_match) {
		obj_fprintf(wk, log_file(), "pkgconf found dependency %o, but the version %o does not match the requested version %o\n",
			ctx->name, ver_str, ctx->versions->val);
		return true;
	}

	make_obj(wk, ctx->res, obj_dependency);
	struct obj_dependency *dep = get_obj_dependency(wk, *ctx->res);
	dep->name = ctx->name;
	dep->version = ver_str;
	dep->flags |= dep_flag_found;
	dep->type = dependency_type_pkgconf;
	dep->dep.link_with = info.libs;
	dep->dep.link_with_not_found = info.not_found_libs;
	dep->dep.include_directories = info.includes;
	dep->dep.compile_args = info.compile_args;
	dep->dep.link_args = info.link_args;

	*found = true;
	return true;
}

static bool
get_dependency(struct workspace *wk, struct dep_lookup_ctx *ctx)
{
	{
		obj cached_dep;
		if (check_dependency_cache(wk, ctx, &cached_dep)) {
			bool ver_match;
			struct obj_dependency *dep = get_obj_dependency(wk, cached_dep);
			if (!check_dependency_version(wk, dep->version, ctx->versions->node,
				ctx->versions->val, &ver_match)) {
				return false;
			}

			if (!ver_match) {
				return true;
			}

			*ctx->res = cached_dep;
			ctx->found = true;
			ctx->from_cache = true;
			return true;
		}
	}

	if (check_dependency_override(wk, ctx)) {
		return true;
	}

	bool force_fallback = false;
	enum wrap_mode wrap_mode = get_option_wrap_mode(wk);

	if (!ctx->fallback) {
		obj provided_fallback;
		if (obj_dict_index(wk, current_project(wk)->wrap_provides_deps,
			ctx->name, &provided_fallback)) {
			ctx->fallback = provided_fallback;
		}
	}

	// implicitly fallback on a subproject named the same as this dependency
	if (!ctx->fallback && ctx->fallback_allowed) {
		make_obj(wk, &ctx->fallback, obj_array);
		obj_array_push(wk, ctx->fallback, ctx->name);
	}

	if (ctx->fallback) {
		obj force_fallback_for, subproj_name;

		get_option_value(wk, current_project(wk), "force_fallback_for", &force_fallback_for);
		obj_array_index(wk, ctx->fallback, 0, &subproj_name);

		force_fallback =
			wrap_mode == wrap_mode_forcefallback
			|| obj_array_in(wk, force_fallback_for, ctx->name)
			|| obj_dict_in(wk, wk->subprojects, subproj_name);
	}

	if (!ctx->found) {
		if (ctx->fallback && (force_fallback || ctx->fallback_only)) {
			if (!handle_dependency_fallback(wk, ctx, &ctx->found)) {
				return false;
			}
		} else {
			if (!get_dependency_pkgconfig(wk, ctx, &ctx->found)) {
				return false;
			}

			if (!ctx->found && ctx->fallback) {
				if (!handle_dependency_fallback(wk, ctx, &ctx->found)) {
					return false;
				}
			}
		}
	}

	return true;
}

static enum iteration_result
handle_appleframeworks_modules_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct dep_lookup_ctx *ctx = _ctx;
	struct obj_dependency *dep = get_obj_dependency(wk, *ctx->res);

	obj_array_push(wk, dep->dep.link_args, make_str(wk, "-framework"));
	obj_array_push(wk, dep->dep.link_args, val);
	return ir_cont;
}

static bool
handle_special_dependency(struct workspace *wk, struct dep_lookup_ctx *ctx, bool *handled)
{
	if (strcmp(get_cstr(wk, ctx->name), "threads") == 0) {
		LOG_I("dependency threads found");

		*handled = true;
		make_obj(wk, ctx->res, obj_dependency);
		struct obj_dependency *dep = get_obj_dependency(wk, *ctx->res);
		dep->name = ctx->name;
		dep->flags |= dep_flag_found;
		dep->type = dependency_type_threads;

		make_obj(wk, &dep->dep.compile_args, obj_array);
		obj_array_push(wk, dep->dep.compile_args, make_str(wk, "-pthread"));

		make_obj(wk, &dep->dep.link_args, obj_array);
		obj_array_push(wk, dep->dep.link_args, make_str(wk, "-pthread"));
	} else if (strcmp(get_cstr(wk, ctx->name), "curses") == 0) {
		*handled = true;
		ctx->name = make_str(wk, "ncurses");
		if (!get_dependency(wk, ctx)) {
			return false;
		}

		if (!ctx->found) {
			*handled = false;
		}
	} else if (strcmp(get_cstr(wk, ctx->name), "appleframeworks") == 0) {
		*handled = true;
		if (!ctx->modules) {
			interp_error(wk, ctx->err_node, "'appleframeworks' dependency requires the modules keyword");
			return false;
		}

		make_obj(wk, ctx->res, obj_dependency);
		if (machine_system() == machine_system_darwin) {
			struct obj_dependency *dep = get_obj_dependency(wk, *ctx->res);
			dep->name = make_str(wk, "appleframeworks");
			dep->flags |= dep_flag_found;
			dep->type = dependency_type_appleframeworks;
			make_obj(wk, &dep->dep.link_args, obj_array);
			obj_array_foreach(wk, ctx->modules, ctx, handle_appleframeworks_modules_iter);
		}
	} else if (strcmp(get_cstr(wk, ctx->name), "") == 0) {
		*handled = true;
		if (ctx->requirement == requirement_required) {
			interp_error(wk, ctx->err_node, "dependency '' cannot be required");
			return false;
		}
		make_obj(wk, ctx->res, obj_dependency);
	} else {
		*handled = false;
	}

	return true;
}

static enum iteration_result
dependency_iter(struct workspace *wk, void *_ctx, obj name)
{
	bool handled;
	struct dep_lookup_ctx *parent_ctx = _ctx;
	struct dep_lookup_ctx ctx = *parent_ctx;
	parent_ctx->name = ctx.name = name;

	if (!handle_special_dependency(wk, &ctx, &handled)) {
		return ir_err;
	} else if (handled) {
		ctx.found = true;
	} else {
		if (!get_dependency(wk, &ctx)) {
			return ir_err;
		}
	}

	if (ctx.found) {
		parent_ctx->lib_mode = ctx.lib_mode;
		parent_ctx->from_cache = ctx.from_cache;
		parent_ctx->found = true;
		return ir_done;
	} else {
		return ir_cont;
	}
}

static enum iteration_result
set_dependency_cache_iter(struct workspace *wk, void *_ctx, obj name)
{
	struct dep_lookup_ctx *ctx = _ctx;

	if (ctx->lib_mode != dep_lib_mode_shared) {
		obj_dict_set(wk, current_project(wk)->dep_cache.static_deps, name, *ctx->res);
	}

	if (ctx->lib_mode != dep_lib_mode_static) {
		obj_dict_set(wk, current_project(wk)->dep_cache.shared_deps, name, *ctx->res);
	}

	return ir_cont;
}

bool
func_dependency(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_required,
		kw_native, // ignored
		kw_version,
		kw_static,
		kw_modules, // ignored
		kw_optional_modules, // ignored
		kw_components, // ignored
		kw_fallback,
		kw_allow_fallback,
		kw_default_options,
		kw_not_found_message,
		kw_disabler,
		kw_method,
		kw_include_type,
	};
	struct args_kw akw[] = {
		[kw_required] = { "required", tc_required_kw },
		[kw_native] = { "native", obj_bool },
		[kw_version] = { "version", TYPE_TAG_LISTIFY | obj_string },
		[kw_static] = { "static", obj_bool },
		[kw_modules] = { "modules", TYPE_TAG_LISTIFY | obj_string },
		[kw_optional_modules] = { "optional_modules", TYPE_TAG_LISTIFY | obj_string },
		[kw_components] = { "components", TYPE_TAG_LISTIFY | obj_string },
		[kw_fallback] = { "fallback", TYPE_TAG_LISTIFY | obj_string },
		[kw_allow_fallback] = { "allow_fallback", obj_bool },
		[kw_default_options] = { "default_options", TYPE_TAG_LISTIFY | obj_string },
		[kw_not_found_message] = { "not_found_message", obj_string },
		[kw_disabler] = { "disabler", obj_bool },
		[kw_method] = { "method", obj_string },
		[kw_include_type] = { "include_type", obj_string },
		0
	};

	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	if (!get_obj_array(wk, an[0].val)->len) {
		interp_error(wk, an[0].node, "no dependency names specified");
		return false;
	}

	enum include_type inc_type = include_type_preserve;
	if (akw[kw_include_type].set) {
		if (!coerce_include_type(wk, get_str(wk, akw[kw_include_type].val),
			akw[kw_include_type].node, &inc_type)) {
			return false;
		}
	}

	enum dependency_lookup_method lookup_method = dependency_lookup_method_auto;
	if (akw[kw_method].set) {
		struct {
			const char *name;
			enum dependency_lookup_method method;
		} lookup_method_names[] = {
			{ "auto", dependency_lookup_method_auto },
			{ "builtin", dependency_lookup_method_builtin },
			{ "cmake", dependency_lookup_method_cmake },
			{ "config-tool", dependency_lookup_method_config_tool },
			{ "dub", dependency_lookup_method_dub },
			{ "extraframework", dependency_lookup_method_extraframework },
			{ "pkg-config", dependency_lookup_method_pkgconfig },
			{ "sysconfig", dependency_lookup_method_sysconfig },
			{ "system", dependency_lookup_method_system },

			// For backwards compatibility
			{ "sdlconfig", dependency_lookup_method_config_tool },
			{ "cups-config", dependency_lookup_method_config_tool },
			{ "pcap-config", dependency_lookup_method_config_tool },
			{ "libwmf-config", dependency_lookup_method_config_tool },
			{ "qmake", dependency_lookup_method_config_tool },
		};

		uint32_t i;
		for (i = 0; i < ARRAY_LEN(lookup_method_names); ++i) {
			if (str_eql(get_str(wk, akw[kw_method].val), &WKSTR(lookup_method_names[i].name))) {
				lookup_method = lookup_method_names[i].method;
				break;
			}
		}

		if (i == ARRAY_LEN(lookup_method_names)) {
			interp_error(wk, akw[kw_method].node, "invalid dependency method %o", akw[kw_method].val);
			return false;
		}

		if (!(lookup_method == dependency_lookup_method_auto
		      || lookup_method == dependency_lookup_method_pkgconfig
		      || lookup_method == dependency_lookup_method_builtin
		      || lookup_method == dependency_lookup_method_system
		      )) {
			interp_warning(wk, akw[kw_method].node, "unsupported dependency method %o, falling back to 'auto'", akw[kw_method].val);
			lookup_method = dependency_lookup_method_auto;
		}
	}

	enum requirement_type requirement;
	if (!coerce_requirement(wk, &akw[kw_required], &requirement)) {
		return false;
	}

	if (requirement == requirement_skip) {
		make_obj(wk, res, obj_dependency);
		struct obj_dependency *dep = get_obj_dependency(wk, *res);
		obj_array_index(wk, an[0].val, 0, &dep->name);
		return true;
	}

	enum dep_lib_mode lib_mode = dep_lib_mode_default;
	if (akw[kw_static].set) {
		if (get_obj_bool(wk, akw[kw_static].val)) {
			lib_mode = dep_lib_mode_static;
		} else {
			lib_mode = dep_lib_mode_shared;
		}
	}

	/* A fallback is allowed if */
	bool fallback_allowed =
		/* - allow_fallback: true */
		(akw[kw_allow_fallback].set && get_obj_bool(wk, akw[kw_allow_fallback].val))
		/* - allow_fallback is not specified and the requirement is required */
		|| (!akw[kw_allow_fallback].set && requirement == requirement_required)
		/* - allow_fallback is not specified and the fallback keyword is
		 *   specified with at least one value (i.e. not an empty array) */
		|| (!akw[kw_allow_fallback].set && akw[kw_fallback].set && get_obj_array(wk, akw[kw_fallback].val)->len);

	uint32_t fallback_err_node = 0;
	obj fallback = 0;
	if (fallback_allowed) {
		if (akw[kw_fallback].set) {
			fallback_err_node = akw[kw_fallback].node;
			fallback = akw[kw_fallback].val;
		} else if (akw[kw_allow_fallback].set) {
			fallback_err_node = akw[kw_allow_fallback].node;
		} else {
			fallback_err_node = an[0].node;
		}
	}

	struct dep_lookup_ctx ctx = {
		.res = res,
		.names = an[0].val,
		.requirement = requirement,
		.versions = &akw[kw_version],
		.err_node = an[0].node,
		.fallback_node = fallback_err_node,
		.fallback = fallback,
		.default_options = &akw[kw_default_options],
		.not_found_message = akw[kw_not_found_message].val,
		.lib_mode = lib_mode,
		.disabler = akw[kw_disabler].set && get_obj_bool(wk, akw[kw_disabler].val),
		.modules = akw[kw_modules].val,
	};

	if (!obj_array_foreach(wk, an[0].val, &ctx, dependency_iter)) {
		return false;
	}
	if (!ctx.found && fallback_allowed) {
		ctx.fallback_allowed = fallback_allowed;
		ctx.fallback_only = true;
		if (!obj_array_foreach(wk, an[0].val, &ctx, dependency_iter)) {
			return false;
		}
	}

	if (!ctx.found) {
		if (ctx.requirement == requirement_required) {
			LLOG_E("required ");
		} else {
			LLOG_W("%s", "");
		}

		obj_fprintf(wk, log_file(), "dependency %o not found", an[0].val);

		if (ctx.not_found_message) {
			obj_fprintf(wk, log_file(), ", %#o", ctx.not_found_message);
		}

		log_plain("\n");

		if (ctx.requirement == requirement_required) {
			interp_error(wk, ctx.err_node, "required dependency not found");
			return false;
		} else {
			if (ctx.disabler) {
				*ctx.res = disabler_id;
			} else {
				make_obj(wk, ctx.res, obj_dependency);
				struct obj_dependency *dep = get_obj_dependency(wk, *ctx.res);
				dep->name = ctx.name;
				dep->type = dependency_type_not_found;
			}
		}
	} else if (!str_eql(get_str(wk, ctx.name), &WKSTR(""))) {
		struct obj_dependency *dep = get_obj_dependency(wk, *ctx.res);

		LLOG_I("found dependency ");
		if (dep->type == dependency_type_declared) {
			obj_fprintf(wk, log_file(), "%o (declared dependency)", ctx.name);
		} else {
			log_plain("%s", get_cstr(wk, dep->name));
		}

		if (dep->version) {
			log_plain(" version %s", get_cstr(wk, dep->version));
		}

		if (ctx.lib_mode == dep_lib_mode_static) {
			log_plain(" static");
		}

		log_plain("\n");

		if (dep->type == dependency_type_declared) {
			L("(%s)", get_cstr(wk, dep->name));
		}
	}

	if (get_obj_type(wk, *res) == obj_dependency) {
		struct obj_dependency *dep = get_obj_dependency(wk, *res);

		if (ctx.from_cache) {
			obj dup;
			make_obj(wk, &dup, obj_dependency);
			struct obj_dependency *newdep = get_obj_dependency(wk, dup);
			*newdep = *dep;
			dep = newdep;
			*res = dup;
		}

		// set the include type if the return value is not a disabler
		dep->include_type = inc_type;

		if (dep->flags & dep_flag_found && !ctx.from_cache) {
			obj_array_foreach(wk, ctx.names, &ctx, set_dependency_cache_iter);
		}
	}

	return true;
}

struct process_dependency_sources_ctx {
	uint32_t err_node;
	obj res;
};

static enum iteration_result
coerce_dependency_sources_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct process_dependency_sources_ctx *ctx = _ctx;

	switch (get_obj_type(wk, val)) {
	case obj_generated_list:
		obj_array_push(wk, ctx->res, val);
		break;
	default: {
		obj res;
		if (!coerce_files(wk, ctx->err_node, val, &res)) {
			return ir_err;
		}

		obj_array_extend_nodup(wk, ctx->res, res);
	}
	}

	return ir_cont;
}
bool
func_declare_dependency(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	enum kwargs {
		kw_sources,
		kw_link_with,
		kw_link_whole,
		kw_link_args,
		kw_dependencies,
		kw_version,
		kw_include_directories,
		kw_variables,
		kw_compile_args,
		kw_objects,
	};
	struct args_kw akw[] = {
		[kw_sources] = { "sources", TYPE_TAG_LISTIFY | tc_coercible_files | tc_generated_list },
		[kw_link_with] = { "link_with", tc_link_with_kw },
		[kw_link_whole] = { "link_whole", tc_link_with_kw },
		[kw_link_args] = { "link_args", TYPE_TAG_LISTIFY | obj_string },
		[kw_dependencies] = { "dependencies", TYPE_TAG_LISTIFY | tc_dependency },
		[kw_version] = { "version", obj_string },
		[kw_include_directories] = { "include_directories", TYPE_TAG_LISTIFY | tc_coercible_inc },
		[kw_variables] = { "variables", tc_array | tc_dict },
		[kw_compile_args] = { "compile_args", TYPE_TAG_LISTIFY | obj_string },
		[kw_objects] = { "objects", TYPE_TAG_LISTIFY | tc_file | tc_string },
		0
	};

	if (!interp_args(wk, args_node, NULL, NULL, akw)) {
		return false;
	}

	if (akw[kw_include_directories].set) {
		obj inc_dirs;
		if (!coerce_include_dirs(wk, akw[kw_include_directories].node, akw[kw_include_directories].val, false, &inc_dirs)) {
			return false;
		}

		akw[kw_include_directories].val = inc_dirs;
	}

	struct obj_dependency *dep;
	make_obj(wk, res, obj_dependency);
	dep = get_obj_dependency(wk, *res);

	if (akw[kw_objects].set) {
		dep->dep.objects = akw[kw_objects].val;
	}

	build_dep_init(wk, &dep->dep);

	dep->name = make_strf(wk, "%s:declared_dep@%s:%d",
		get_cstr(wk, current_project(wk)->cfg.name),
		wk->src->label,
		get_node(wk->ast, args_node)->location.line);
	dep->flags |= dep_flag_found;
	dep->type = dependency_type_declared;

	if (akw[kw_variables].set && !coerce_key_value_dict(wk, akw[kw_variables].node, akw[kw_variables].val, &dep->variables)) {
		return false;
	}

	if (akw[kw_link_args].set) {
		obj_array_extend_nodup(wk, dep->dep.link_args, akw[kw_link_args].val);
	}

	if (akw[kw_compile_args].set) {
		obj_array_extend_nodup(wk, dep->dep.compile_args, akw[kw_compile_args].val);
	}

	if (akw[kw_version].set) {
		dep->version = akw[kw_version].val;
	} else {
		dep->version = current_project(wk)->cfg.version;
	}

	if (akw[kw_sources].set) {
		struct process_dependency_sources_ctx ctx = {
			.err_node = akw[kw_sources].node,
			.res = dep->dep.sources,
		};

		if (!obj_array_foreach_flat(wk, akw[kw_sources].val, &ctx, coerce_dependency_sources_iter)) {
			return false;
		}
	}

	if (akw[kw_link_with].set) {
		if (!dep_process_link_with(wk, akw[kw_link_with].node,
			akw[kw_link_with].val, &dep->dep)) {
			return false;
		}
	}

	if (akw[kw_link_whole].set) {
		if (!dep_process_link_whole(wk, akw[kw_link_whole].node,
			akw[kw_link_whole].val, &dep->dep)) {
			return false;
		}
	}

	if (akw[kw_include_directories].set) {
		dep_process_includes(wk, akw[kw_include_directories].val, include_type_preserve, dep->dep.include_directories);
	}

	if (akw[kw_dependencies].set) {
		dep_process_deps(wk, akw[kw_dependencies].val, &dep->dep);
	}

	return true;
}

/*
 */

static bool
skip_if_present(struct workspace *wk, obj arr, obj val)
{
	if (hash_get(&wk->obj_hash, &val)) {
		return true;
	}

	hash_set(&wk->obj_hash, &val, true);

	return false;
}

struct dep_process_includes_ctx {
	obj dest;
	enum include_type include_type;
};

static enum iteration_result
dep_process_includes_iter(struct workspace *wk, void *_ctx, obj inc_id)
{
	struct dep_process_includes_ctx *ctx = _ctx;

	struct obj_include_directory *inc = get_obj_include_directory(wk, inc_id);

	bool new_is_system = inc->is_system;

	switch (ctx->include_type) {
	case include_type_preserve:
		break;
	case include_type_system:
		new_is_system = true;
		break;
	case include_type_non_system:
		new_is_system = false;
		break;
	}

	if (inc->is_system != new_is_system) {
		make_obj(wk, &inc_id, obj_include_directory);
		struct obj_include_directory *new_inc =
			get_obj_include_directory(wk, inc_id);
		*new_inc = *inc;
		new_inc->is_system = new_is_system;
	}

	obj_array_push(wk, ctx->dest, inc_id);
	return ir_cont;
}

void
dep_process_includes(struct workspace *wk, obj arr, enum include_type include_type, obj dest)
{
	obj_array_foreach_flat(wk, arr, &(struct dep_process_includes_ctx) {
		.include_type = include_type,
		.dest = dest,
	}, dep_process_includes_iter);
}

void
build_dep_init(struct workspace *wk, struct build_dep *dep)
{
	if (!dep->include_directories) {
		make_obj(wk, &dep->include_directories, obj_array);
	}

	if (!dep->link_with) {
		make_obj(wk, &dep->link_with, obj_array);
	}

	if (!dep->link_whole) {
		make_obj(wk, &dep->link_whole, obj_array);
	}

	if (!dep->link_with_not_found) {
		make_obj(wk, &dep->link_with_not_found, obj_array);
	}

	if (!dep->link_args) {
		make_obj(wk, &dep->link_args, obj_array);
	}

	if (!dep->compile_args) {
		make_obj(wk, &dep->compile_args, obj_array);
	}

	if (!dep->order_deps) {
		make_obj(wk, &dep->order_deps, obj_array);
	}

	if (!dep->rpath) {
		make_obj(wk, &dep->rpath, obj_array);
	}

	if (!dep->sources) {
		make_obj(wk, &dep->sources, obj_array);
	}

	if (!dep->objects) {
		make_obj(wk, &dep->objects, obj_array);
	}
}

static void
merge_build_deps(struct workspace *wk, struct build_dep *src, struct build_dep *dest, bool dep)
{
	build_dep_init(wk, dest);

	dest->link_language = coalesce_link_languages(src->link_language, dest->link_language);

	if (src->link_with) {
		obj_array_extend(wk, dest->link_with, src->link_with);
	}

	if (src->link_with_not_found) {
		obj_array_extend(wk, dest->link_with_not_found, src->link_with_not_found);
	}

	if (src->link_whole) {
		obj_array_extend(wk, dest->link_whole, src->link_whole);
	}

	if (dep && src->include_directories) {
		obj_array_extend(wk, dest->include_directories, src->include_directories);
	}

	if (src->link_args) {
		obj_array_extend(wk, dest->link_args, src->link_args);
	}

	if (dep && src->compile_args) {
		obj_array_extend(wk, dest->compile_args, src->compile_args);
	}

	if (src->rpath) {
		obj_array_extend(wk, dest->rpath, src->rpath);
	}

	if (src->order_deps) {
		obj_array_extend(wk, dest->order_deps, src->order_deps);
	}

	if (dep && src->sources) {
		obj_array_extend(wk, dest->sources, src->sources);
	}

	if (dep && src->objects) {
		obj_array_extend(wk, dest->objects, src->objects);
	}
}

static enum iteration_result
dedup_link_args_iter(struct workspace *wk, void *_ctx, obj val)
{
	obj new_args = *(obj *)_ctx;

	static const char *known[] = {
		"-pthread",
	};

	const char *s = get_cstr(wk, val);

	uint32_t i;
	for (i = 0; i < ARRAY_LEN(known); ++i) {
		if (strcmp(known[i], s) == 0) {
			if (obj_array_in(wk, new_args, val)) {
				return ir_cont;
			} else {
				break;
			}
		}
	}

	obj_array_push(wk, new_args, val);
	return ir_cont;
}

static void
dedup_build_dep(struct workspace *wk, struct build_dep *dep)
{
	obj_array_dedup_in_place(wk, &dep->link_with);
	obj_array_dedup_in_place(wk, &dep->link_with_not_found);
	obj_array_dedup_in_place(wk, &dep->link_whole);
	obj_array_dedup_in_place(wk, &dep->raw.deps);
	obj_array_dedup_in_place(wk, &dep->raw.link_with);
	obj_array_dedup_in_place(wk, &dep->raw.link_whole);
	obj_array_dedup_in_place(wk, &dep->include_directories);
	obj_array_dedup_in_place(wk, &dep->rpath);
	obj_array_dedup_in_place(wk, &dep->order_deps);
	obj_array_dedup_in_place(wk, &dep->sources);
	obj_array_dedup_in_place(wk, &dep->objects);

	obj new_link_args;
	make_obj(wk, &new_link_args, obj_array);
	obj_array_foreach(wk, dep->link_args, &new_link_args, dedup_link_args_iter);
	dep->link_args = new_link_args;

	obj new_compile_args;
	make_obj(wk, &new_compile_args, obj_array);
	obj_array_foreach(wk, dep->compile_args, &new_compile_args, dedup_link_args_iter);
	dep->compile_args = new_compile_args;
}

struct dep_process_link_with_ctx {
	struct build_dep *dest;
	bool link_whole;
	uint32_t err_node;
};

static enum iteration_result
dep_process_link_with_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct dep_process_link_with_ctx *ctx = _ctx;

	if (skip_if_present(wk, ctx->dest->raw.link_with, val)) {
		return ir_cont;
	}

	enum obj_type t = get_obj_type(wk, val);

	/* obj_fprintf(wk, log_file(), "link_with: %o\n", val); */

	obj dest_link_with;

	if (ctx->link_whole) {
		dest_link_with = ctx->dest->link_whole;
	} else {
		dest_link_with = ctx->dest->link_with;
	}

	switch (t) {
	case obj_both_libs:
		val = get_obj_both_libs(wk, val)->dynamic_lib;
	/* fallthrough */
	case obj_build_target: {
		struct obj_build_target *tgt = get_obj_build_target(wk, val);
		const char *path = get_cstr(wk, tgt->build_path);

		if (ctx->link_whole && tgt->type != tgt_static_library) {
			interp_error(wk, ctx->err_node, "link whole only accepts static libraries");
			return ir_err;
		}

		if (tgt->type != tgt_executable) {
			obj_array_push(wk, dest_link_with, make_str(wk, path));
		}

		// calculate rpath for this target
		// we always want an absolute path here, regardles of
		// ctx->relativize
		if (tgt->type != tgt_static_library) {
			SBUF(abs);
			SBUF(dir);
			const char *p;
			path_dirname(wk, &dir, path);

			if (path_is_absolute(dir.buf)) {
				p = dir.buf;
			} else {
				path_join(wk, &abs, wk->build_root, dir.buf);
				p = abs.buf;
			}

			obj s = make_str(wk, p);

			if (!obj_array_in(wk, ctx->dest->rpath, s)) {
				obj_array_push(wk, ctx->dest->rpath, s);
			}
		}

		merge_build_deps(wk, &tgt->dep, ctx->dest, false);
		break;
	}
	case obj_custom_target: {
		obj_array_foreach(wk, get_obj_custom_target(wk, val)->output, ctx, dep_process_link_with_iter);
		break;
	}
	case obj_file: {
		obj_array_push(wk, dest_link_with, *get_obj_file(wk, val));
		break;
	}
	case obj_string:
		obj_array_push(wk, dest_link_with, val);
		break;
	default:
		interp_error(wk, ctx->err_node, "invalid type for link_with: '%s'", obj_type_to_s(t));
		return ir_err;
	}

	return ir_cont;
}

bool
dep_process_link_with(struct workspace *wk, uint32_t err_node, obj arr, struct build_dep *dest)
{
	build_dep_init(wk, dest);
	dest->raw.link_with = arr;

	hash_clear(&wk->obj_hash);

	if (!obj_array_foreach_flat(wk, arr, &(struct dep_process_link_with_ctx) {
		.dest = dest,
		.err_node = err_node,
	}, dep_process_link_with_iter)) {
		return false;
	}

	dedup_build_dep(wk, dest);
	return true;
}

bool
dep_process_link_whole(struct workspace *wk, uint32_t err_node, obj arr, struct build_dep *dest)
{
	build_dep_init(wk, dest);
	dest->raw.link_whole = arr;

	hash_clear(&wk->obj_hash);

	if (!obj_array_foreach_flat(wk, arr, &(struct dep_process_link_with_ctx) {
		.dest = dest,
		.link_whole = true,
		.err_node = err_node,
	}, dep_process_link_with_iter)) {
		return false;
	}

	dedup_build_dep(wk, dest);
	return true;
}

static enum iteration_result
dep_process_deps_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct build_dep *dest = _ctx;

	/* obj_fprintf(wk, log_file(), "dep: %o\n", val); */

	if (skip_if_present(wk, dest->raw.deps, val)) {
		return ir_cont;
	}

	struct obj_dependency *dep = get_obj_dependency(wk, val);
	if (!(dep->flags & dep_flag_found)) {
		return ir_cont;
	}

	merge_build_deps(wk, &dep->dep, dest, true);

	return ir_cont;
}

void
dep_process_deps(struct workspace *wk, obj deps, struct build_dep *dest)
{
	build_dep_init(wk, dest);
	dest->raw.deps = deps;

	hash_clear(&wk->obj_hash);

	obj_array_foreach(wk, deps, dest, dep_process_deps_iter);

	dedup_build_dep(wk, dest);
}

/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: Eli Schwartz <eschwartz@archlinux.org>
 * SPDX-FileCopyrightText: illiliti <illiliti@dimension.sh>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "args.h"
#include "backend/common_args.h"
#include "coerce.h"
#include "compilers.h"
#include "error.h"
#include "functions/build_target.h"
#include "functions/common.h"
#include "functions/meson.h"
#include "lang/typecheck.h"
#include "log.h"
#include "options.h"
#include "platform/path.h"
#include "version.h"

static bool
func_meson_get_compiler(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs { kw_native, };
	struct args_kw akw[] = {
		[kw_native] = { "native", obj_bool },
		0
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	enum compiler_language l;
	if (!s_to_compiler_language(get_cstr(wk, an[0].val), &l)
	    || !obj_dict_geti(wk, current_project(wk)->compilers, l, res)) {
		vm_error_at(wk, an[0].node, "no compiler found for '%s'", get_cstr(wk, an[0].val));
		return false;
	}

	return true;
}

static bool
func_meson_project_name(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = current_project(wk)->cfg.name;
	return true;
}

static bool
func_meson_project_license(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = current_project(wk)->cfg.license;
	if (!*res) {
		make_obj(wk, res, obj_array);
	}
	return true;
}

static bool
func_meson_project_license_files(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = current_project(wk)->cfg.license_files;
	if (!*res) {
		make_obj(wk, res, obj_array);
	}
	return true;
}

static bool
func_meson_project_version(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = current_project(wk)->cfg.version;
	return true;
}

static bool
func_meson_version(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = make_str(wk, muon_version.meson_compat);
	return true;
}

static bool
func_meson_current_source_dir(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = current_project(wk)->cwd;
	return true;
}

static bool
func_meson_current_build_dir(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = current_project(wk)->build_dir;
	return true;
}

static bool
func_meson_project_source_root(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = current_project(wk)->source_root;
	return true;
}

static bool
func_meson_project_build_root(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = current_project(wk)->build_root;
	return true;
}

static bool
func_meson_global_source_root(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = make_str(wk, wk->source_root);
	return true;
}

static bool
func_meson_global_build_root(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = make_str(wk, wk->build_root);
	return true;
}

static bool
func_meson_build_options(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	obj options = regenerate_build_command(wk, true);

	// remove the build directory from options
	obj_array_pop(wk, options);

	*res = join_args_shell(wk, options);
	return true;
}

static bool
func_meson_is_subproject(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, wk->cur_project != 0);
	return true;
}

static bool
func_meson_backend(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = make_str(wk, "ninja");
	return true;
}

static bool
func_meson_is_cross_build(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, false);
	return true;
}

static bool
func_meson_is_unity(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, false);
	return true;
}

static bool
func_meson_override_dependency(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_string }, { obj_dependency }, ARG_TYPE_NULL };
	enum kwargs {
		kw_static,
		kw_native, // ignored
	};
	struct args_kw akw[] = {
		[kw_static] = { "static", obj_bool },
		[kw_native] = { "native", obj_bool },
		0
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	obj override_dict;

	if (akw[kw_static].set) {
		if (get_obj_bool(wk, akw[kw_static].val)) {
			override_dict = wk->dep_overrides_static;
		} else {
			override_dict = wk->dep_overrides_dynamic;
		}
	} else {
		switch (get_option_default_library(wk)) {
		case tgt_static_library:
			override_dict = wk->dep_overrides_static;
			break;
		default:
			override_dict = wk->dep_overrides_dynamic;
			break;
		}
	}

	obj_dict_set(wk, override_dict, an[0].val, an[1].val);
	return true;
}

static bool
func_meson_override_find_program(struct workspace *wk, obj _, obj *res)
{
	type_tag tc_allowed = tc_file | tc_external_program | tc_build_target \
			      | tc_custom_target | tc_python_installation;
	struct args_norm an[] = { { obj_string }, { tc_allowed }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	obj override;

	switch (get_obj_type(wk, an[1].val)) {
	case obj_build_target:
	case obj_custom_target:
	case obj_file:
		make_obj(wk, &override, obj_array);
		obj_array_push(wk, override, an[1].val);

		obj ver = 0;
		if (!current_project(wk)->cfg.no_version) {
			ver = current_project(wk)->cfg.version;
		}
		obj_array_push(wk, override, ver);
		break;
	case obj_external_program:
	case obj_python_installation:
		override = an[1].val;
		break;
	default:
		UNREACHABLE;
	}

	obj_dict_set(wk, wk->find_program_overrides, an[0].val, override);
	return true;
}

struct process_script_commandline_ctx {
	uint32_t node;
	obj arr;
	uint32_t i;
	bool allow_not_built;
	bool make_deps_default;
};

static enum iteration_result
process_script_commandline_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct process_script_commandline_ctx *ctx = _ctx;
	obj str;
	enum obj_type t = get_obj_type(wk, val);

	switch (t) {
	case obj_string:
		if (ctx->i) {
			str = val;
		} else {
			const char *p = get_cstr(wk, val);

			if (path_is_absolute(p)) {
				str = val;
			} else {
				SBUF(path);
				path_join(wk, &path, get_cstr(wk, current_project(wk)->cwd), p);
				str = sbuf_into_str(wk, &path);
			}
		}
		break;
	case obj_custom_target:
		if (!ctx->allow_not_built) {
			goto type_error;
		}

		struct obj_custom_target *o = get_obj_custom_target(wk, val);
		if (ctx->make_deps_default) {
			o->flags |= custom_target_build_by_default;
		}

		if (!obj_array_foreach(wk, o->output, ctx, process_script_commandline_iter)) {
			return false;
		}
		goto cont;
	case obj_build_target: {
		if (!ctx->allow_not_built) {
			goto type_error;
		}

		struct obj_build_target *o = get_obj_build_target(wk, val);

		if (ctx->make_deps_default) {
			o->flags |= build_tgt_flag_build_by_default;
		}
	}
	//fallthrough
	case obj_external_program:
	case obj_python_installation:
	case obj_file: {
		obj args;
		if (!coerce_executable(wk, ctx->node, val, &str, &args)) {
			return ir_err;
		}

		if (args) {
			obj_array_push(wk, ctx->arr, str);
			obj_array_extend_nodup(wk, ctx->arr, args);
			return ir_cont;
		}

		break;
	}
	default:
type_error:
		vm_error_at(wk, ctx->node, "invalid type for script commandline '%s'",
			obj_type_to_s(t));
		return ir_err;
	}

	obj_array_push(wk, ctx->arr, str);
cont:
	++ctx->i;
	return ir_cont;
}

static bool
func_meson_add_install_script(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | tc_exe }, ARG_TYPE_NULL };
	enum kwargs {
		kw_install_tag, // ignored
		kw_skip_if_destdir, // ignored
		kw_dry_run,
	};
	struct args_kw akw[] = {
		[kw_install_tag] = { "install_tag", obj_string },
		[kw_skip_if_destdir] = { "skip_if_destdir", obj_bool },
		[kw_dry_run] = { "dry_run", obj_bool },
		0
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	struct process_script_commandline_ctx ctx = {
		.node = an[0].node,
		.allow_not_built = true,
		.make_deps_default = true,
	};
	make_obj(wk, &ctx.arr, obj_array);

	if (!obj_array_foreach_flat(wk, an[0].val, &ctx, process_script_commandline_iter)) {
		return false;
	}

	if (!akw[kw_dry_run].set) {
		make_obj(wk, &akw[kw_dry_run].val, obj_bool);
		set_obj_bool(wk, akw[kw_dry_run].val, false);
	}

	obj install_script;
	make_obj(wk, &install_script, obj_array);
	obj_array_push(wk, install_script, akw[kw_dry_run].val);
	obj_array_push(wk, install_script, ctx.arr);
	obj_array_push(wk, wk->install_scripts, install_script);
	return true;
}

static bool
func_meson_add_postconf_script(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | tc_exe }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	struct process_script_commandline_ctx ctx = {
		.node = an[0].node,
	};
	make_obj(wk, &ctx.arr, obj_array);

	if (!obj_array_foreach_flat(wk, an[0].val, &ctx, process_script_commandline_iter)) {
		return false;
	}

	obj_array_push(wk, wk->postconf_scripts, ctx.arr);
	return true;
}

static bool
func_meson_add_dist_script(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | tc_exe }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	struct process_script_commandline_ctx ctx = {
		.node = an[0].node,
		.allow_not_built = true,
	};
	make_obj(wk, &ctx.arr, obj_array);

	if (!obj_array_foreach_flat(wk, an[0].val, &ctx, process_script_commandline_iter)) {
		return false;
	}

	// TODO: uncomment when muon dist is implemented
	/* obj_array_push(wk, wk->dist_scripts, ctx.arr); */
	return true;
}

static bool
func_meson_get_cross_property(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { tc_any }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	if (ao[0].set) {
		*res = ao[0].val;
	} else {
		vm_error_at(wk, an[0].node, "TODO: get cross property");
		return false;
	}

	return true;
}

static bool
func_meson_get_external_property(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { tc_any }, ARG_TYPE_NULL };
	enum kwargs { kw_native, };
	struct args_kw akw[] = {
		[kw_native] = { "native", obj_bool },
		0
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	if (ao[0].set) {
		*res = ao[0].val;
	} else {
		vm_error_at(wk, an[0].node, "TODO: get external property");
		return false;
	}

	return true;
}

static bool
func_meson_can_run_host_binaries(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, true); // TODO: can return false in cross compile
	return true;
}

static bool
func_meson_add_devenv(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { tc_any }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	return true;
}

const struct func_impl impl_tbl_meson[] = {
	{ "add_devenv", func_meson_add_devenv },
	{ "add_dist_script", func_meson_add_dist_script },
	{ "add_install_script", func_meson_add_install_script },
	{ "add_postconf_script", func_meson_add_postconf_script },
	{ "backend", func_meson_backend, tc_string },
	{ "build_options", func_meson_build_options, tc_string },
	{ "build_root", func_meson_global_build_root, tc_string },
	{ "can_run_host_binaries", func_meson_can_run_host_binaries, tc_bool },
	{ "current_build_dir", func_meson_current_build_dir, tc_string },
	{ "current_source_dir", func_meson_current_source_dir, tc_string },
	{ "get_compiler", func_meson_get_compiler, tc_compiler },
	{ "get_cross_property", func_meson_get_cross_property, tc_any },
	{ "get_external_property", func_meson_get_external_property, tc_any },
	{ "global_build_root", func_meson_global_build_root, tc_string },
	{ "global_source_root", func_meson_global_source_root, tc_string },
	{ "has_exe_wrapper", func_meson_can_run_host_binaries, tc_bool },
	{ "is_cross_build", func_meson_is_cross_build, tc_bool },
	{ "is_subproject", func_meson_is_subproject, tc_bool },
	{ "is_unity", func_meson_is_unity, tc_bool },
	{ "override_dependency", func_meson_override_dependency },
	{ "override_find_program", func_meson_override_find_program },
	{ "project_build_root", func_meson_project_build_root, tc_string },
	{ "project_license", func_meson_project_license, tc_string },
	{ "project_license_files", func_meson_project_license_files, tc_string },
	{ "project_name", func_meson_project_name, tc_string },
	{ "project_source_root", func_meson_project_source_root, tc_string },
	{ "project_version", func_meson_project_version, tc_string },
	{ "source_root", func_meson_global_source_root, tc_string },
	{ "version", func_meson_version, tc_string },
	{ NULL, NULL },
};

static enum iteration_result
compiler_dict_to_str_dict_iter(struct workspace *wk, void *_ctx, obj k, obj v)
{
	obj_dict_set(wk, *(obj *)_ctx, make_str(wk, compiler_language_to_s(k)), v);
	return ir_cont;
}

static obj
compiler_dict_to_str_dict(struct workspace *wk, obj d)
{
	obj r;
	make_obj(wk, &r, obj_dict);
	obj_dict_foreach(wk, d, &r, compiler_dict_to_str_dict_iter);

	return r;
}

static bool
func_meson_project(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	struct project *proj = current_project(wk);

	make_obj(wk, res, obj_dict);
	obj_dict_set(wk, *res, make_str(wk, "opts"), proj->opts);
	obj_dict_set(wk, *res, make_str(wk, "compilers"), compiler_dict_to_str_dict(wk, proj->compilers));
	obj_dict_set(wk, *res, make_str(wk, "args"), compiler_dict_to_str_dict(wk, proj->args));
	obj_dict_set(wk, *res, make_str(wk, "link_args"), compiler_dict_to_str_dict(wk, proj->link_args));
	return true;
}

const struct func_impl impl_tbl_meson_internal[] = {
	{ "project", func_meson_project, tc_dict, true },
	{ NULL, NULL },
};

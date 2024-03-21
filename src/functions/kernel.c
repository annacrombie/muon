/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: dffdff2423 <dffdff2423@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "args.h"
#include "buf_size.h"
#include "coerce.h"
#include "error.h"
#include "external/samurai.h"
#include "functions/common.h"
#include "functions/environment.h"
#include "functions/external_program.h"
#include "functions/kernel.h"
#include "functions/kernel/build_target.h"
#include "functions/kernel/configure_file.h"
#include "functions/kernel/custom_target.h"
#include "functions/kernel/dependency.h"
#include "functions/kernel/install.h"
#include "functions/kernel/options.h"
#include "functions/kernel/subproject.h"
#include "functions/modules.h"
#include "functions/string.h"
#include "lang/serial.h"
#include "lang/typecheck.h"
#include "log.h"
#include "options.h"
#include "platform/filesystem.h"
#include "platform/mem.h"
#include "platform/path.h"
#include "platform/run_cmd.h"
#include "wrap.h"

static bool
project_add_language(struct workspace *wk, uint32_t err_node, obj str, enum requirement_type req, bool *found)
{
	if (req == requirement_skip) {
		return true;
	}

	obj comp_id;
	enum compiler_language l;
	if (!s_to_compiler_language(get_cstr(wk, str), &l)) {
		if (req == requirement_required) {
			vm_error_at(wk, err_node, "%o is not a valid language", str);
			return false;
		} else {
			return true;
		}
	}

	obj res;
	if (obj_dict_geti(wk, current_project(wk)->compilers, l, &res)) {
		*found = true;
		return true;
	}

	if (!toolchain_detect(wk, &comp_id, l)) {
		if (req == requirement_required) {
			vm_error_at(wk, err_node, "unable to detect %s compiler", get_cstr(wk, str));
			return false;
		} else {
			return true;
		}
	}

	obj_dict_seti(wk, current_project(wk)->compilers, l, comp_id);

	/* if we just added a c or cpp compiler, set the assembly compiler to that */
	if (l == compiler_language_c || l == compiler_language_cpp) {
		obj_dict_seti(wk, current_project(wk)->compilers, compiler_language_assembly, comp_id);

		struct obj_compiler *comp = get_obj_compiler(wk, comp_id);
		if (comp->type == compiler_clang) {
			obj llvm_ir_compiler;
			make_obj(wk, &llvm_ir_compiler, obj_compiler);
			struct obj_compiler *c = get_obj_compiler(wk, llvm_ir_compiler);
			*c = *comp;
			c->type = compiler_clang_llvm_ir;
			c->lang = compiler_language_llvm_ir;
			obj_dict_seti(wk, current_project(wk)->compilers, compiler_language_llvm_ir, llvm_ir_compiler);
		}
	}

	if (l == compiler_language_nasm) {
		obj c_compiler;
		if (!obj_dict_geti(wk, current_project(wk)->compilers, compiler_language_c, &c_compiler)
			&& !obj_dict_geti(wk, current_project(wk)->compilers, compiler_language_cpp, &c_compiler)) {
			bool c_found;
			if (!project_add_language(wk, err_node, make_str(wk, "c"), req, &c_found)) {
				return false;
			}
		}
	}

	*found = true;
	return true;
}

struct project_add_language_iter_ctx {
	uint32_t err_node;
	enum requirement_type req;
	bool missing;
};

static enum iteration_result
project_add_language_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct project_add_language_iter_ctx *ctx = _ctx;

	bool found = false;
	if (!project_add_language(wk, ctx->err_node, val, ctx->req, &found)) {
		return ir_err;
	}

	if (!found) {
		ctx->missing = true;
	}

	return ir_cont;
}

static bool
func_project(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_string }, { TYPE_TAG_GLOB | tc_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_default_options,
		kw_license,
		kw_license_files,
		kw_meson_version,
		kw_subproject_dir,
		kw_version,
	};
	struct args_kw akw[] = {
		[kw_default_options] = { "default_options", TYPE_TAG_LISTIFY | obj_string },
		[kw_license] = { "license", TYPE_TAG_LISTIFY | obj_string },
		[kw_license_files] = { "license_files", TYPE_TAG_LISTIFY | obj_string },
		[kw_meson_version] = { "meson_version", obj_string },
		[kw_subproject_dir] = { "subproject_dir", obj_string },
		[kw_version] = { "version", tc_string | tc_file },
		0,
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	if (current_project(wk)->initialized) {
		vm_error(wk, "project may only be called once");
		return false;
	}

	current_project(wk)->cfg.name = an[0].val;
#ifndef MUON_BOOTSTRAPPED
	if (wk->cur_project == 0 && !str_eql(get_str(wk, an[0].val), &WKSTR("muon"))) {
		vm_error_at(wk,
			an[0].node,
			"This muon has not been fully bootstrapped. It can only be used to setup muon itself.");
		return false;
	}
#endif

	if (!obj_array_foreach_flat(wk,
		    an[1].val,
		    &(struct project_add_language_iter_ctx){
			    .err_node = an[1].node,
			    .req = requirement_required,
		    },
		    project_add_language_iter)) {
		return false;
	}

	current_project(wk)->cfg.license = akw[kw_license].val;
	current_project(wk)->cfg.license_files = akw[kw_license_files].val;

	if (akw[kw_version].set) {
		if (get_obj_type(wk, akw[kw_version].val) == obj_string) {
			current_project(wk)->cfg.version = akw[kw_version].val;
		} else {
			struct source ver_src = { 0 };
			if (!fs_read_entire_file(get_file_path(wk, akw[kw_version].val), &ver_src)) {
				vm_error_at(wk, akw[kw_version].node, "failed to read version file");
				return false;
			}

			const char *str_ver = ver_src.src;
			uint32_t i;
			for (i = 0; ver_src.src[i]; ++i) {
				if (ver_src.src[i] == '\n') {
					if (ver_src.src[i + 1]) {
						vm_error_at(wk,
							akw[kw_version].node,
							"version file is more than one line long");
						return false;
					}
					break;
				}
			}

			current_project(wk)->cfg.version = make_strn(wk, str_ver, i);

			fs_source_destroy(&ver_src);
		}
	} else {
		current_project(wk)->cfg.version = make_str(wk, "undefined");
		current_project(wk)->cfg.no_version = true;
	}

	if (akw[kw_default_options].set) {
		if (!parse_and_set_default_options(
			    wk, akw[kw_default_options].node, akw[kw_default_options].val, 0, false)) {
			return false;
		}
	}

	if (wk->cur_project == 0) {
		if (!prefix_dir_opts(wk)) {
			return false;
		}
	}

	if (akw[kw_subproject_dir].set) {
		current_project(wk)->subprojects_dir = akw[kw_subproject_dir].val;
	}

	{ // subprojects
		SBUF(subprojects_path);
		path_join(wk,
			&subprojects_path,
			get_cstr(wk, current_project(wk)->source_root),
			get_cstr(wk, current_project(wk)->subprojects_dir));

		if (!wrap_load_all_provides(wk, subprojects_path.buf)) {
			LOG_E("failed loading wrap provides");
			return false;
		}
	}

	LOG_I("configuring '%s', version: %s",
		get_cstr(wk, current_project(wk)->cfg.name),
		get_cstr(wk, current_project(wk)->cfg.version));

	current_project(wk)->initialized = true;
	return true;
}

static obj
get_project_argument_array(struct workspace *wk, obj dict, enum compiler_language l)
{
	obj arg_arr;
	if (!obj_dict_geti(wk, dict, l, &arg_arr)) {
		make_obj(wk, &arg_arr, obj_array);
		obj_dict_seti(wk, dict, l, arg_arr);
	}

	return arg_arr;
}

struct add_arguments_ctx {
	uint32_t lang_node;
	uint32_t args_node;
	obj args_dict;
	obj args_to_add;
	obj arg_arr;
};

static enum iteration_result
add_arguments_language_iter(struct workspace *wk, void *_ctx, obj val_id)
{
	struct add_arguments_ctx *ctx = _ctx;

	if (!typecheck(wk, ctx->args_node, val_id, obj_string)) {
		return ir_err;
	}

	obj_array_push(wk, ctx->arg_arr, val_id);

	return ir_cont;
}

static enum iteration_result
add_arguments_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct add_arguments_ctx *ctx = _ctx;
	enum compiler_language l;

	if (!s_to_compiler_language(get_cstr(wk, val), &l)) {
		vm_error_at(wk, ctx->lang_node, "unknown language '%s'", get_cstr(wk, val));
		return ir_err;
	}

	ctx->arg_arr = get_project_argument_array(wk, ctx->args_dict, l);

	if (!obj_array_foreach_flat(wk, ctx->args_to_add, ctx, add_arguments_language_iter)) {
		return ir_err;
	}

	return ir_cont;
}

static bool
add_arguments_common(struct workspace *wk, obj args_dict, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_language,
		kw_native, // ignored
	};
	struct args_kw akw[] = {
		[kw_language] = { "language", TYPE_TAG_LISTIFY | obj_string, .required = true },
		[kw_native] = { "native", obj_bool },
		0,
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	struct add_arguments_ctx ctx = {
		.lang_node = akw[kw_language].node,
		.args_node = an[0].node,
		.args_dict = args_dict,
		.args_to_add = an[0].val,
	};
	return obj_array_foreach(wk, akw[kw_language].val, &ctx, add_arguments_iter);
}

static bool
func_add_project_arguments(struct workspace *wk, obj _, obj *res)
{
	return add_arguments_common(wk, current_project(wk)->args, res);
}

static bool
func_add_global_arguments(struct workspace *wk, obj _, obj *res)
{
	if (wk->cur_project != 0) {
		vm_error(wk, "add_global_arguments cannot be called from a subproject");
		return false;
	}

	return add_arguments_common(wk, wk->global_args, res);
}

static bool
func_add_project_link_arguments(struct workspace *wk, obj _, obj *res)
{
	return add_arguments_common(wk, current_project(wk)->link_args, res);
}

static bool
func_add_global_link_arguments(struct workspace *wk, obj _, obj *res)
{
	if (wk->cur_project != 0) {
		vm_error(wk, "add_global_link_arguments cannot be called from a subproject");
		return false;
	}

	return add_arguments_common(wk, wk->global_link_args, res);
}

struct add_project_dependencies_ctx {
	uint32_t lang_node;
	struct build_dep *d;
};

static enum iteration_result
add_project_dependencies_iter(struct workspace *wk, void *_ctx, obj lang)
{
	struct add_project_dependencies_ctx *ctx = _ctx;
	enum compiler_language l;

	if (!s_to_compiler_language(get_cstr(wk, lang), &l)) {
		vm_error_at(wk, ctx->lang_node, "unknown language '%s'", get_cstr(wk, lang));
		return ir_err;
	}

	obj res;
	if (!obj_dict_geti(wk, current_project(wk)->compilers, l, &res)) {
		// NOTE: Its a little weird that the other add_project_xxx
		// functions don't check this and this function does, but that
		// is how meson does it.
		vm_error_at(wk, ctx->lang_node, "undeclared language '%s'", get_cstr(wk, lang));
		return ir_err;
	}

	obj_array_extend(wk, get_project_argument_array(wk, current_project(wk)->args, l), ctx->d->compile_args);
	obj_array_extend(wk, get_project_argument_array(wk, current_project(wk)->link_args, l), ctx->d->link_args);
	obj_array_extend(
		wk, get_project_argument_array(wk, current_project(wk)->include_dirs, l), ctx->d->include_directories);
	return ir_cont;
}

static bool
func_add_project_dependencies(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | tc_dependency }, ARG_TYPE_NULL };
	enum kwargs {
		kw_language,
		kw_native, // ignored
	};
	struct args_kw akw[] = {
		[kw_language] = { "language", TYPE_TAG_LISTIFY | obj_string, .required = true },
		[kw_native] = { "native", obj_bool },
		0,
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	struct build_dep d = { 0 };
	dep_process_deps(wk, an[0].val, &d);

	struct add_project_dependencies_ctx ctx = {
		.lang_node = akw[kw_language].node,
		.d = &d,
	};

	obj_array_foreach(wk, akw[kw_language].val, &ctx, add_project_dependencies_iter);
	return true;
}

static bool
func_add_languages(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_required,
		kw_native,
	};
	struct args_kw akw[] = {
		[kw_required] = { "required", tc_required_kw },
		[kw_native] = { "native", obj_bool },
		0,
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	struct project_add_language_iter_ctx ctx = {
		.err_node = an[0].node,
	};

	if (!coerce_requirement(wk, &akw[kw_required], &ctx.req)) {
		return false;
	}

	if (!obj_array_foreach(wk, an[0].val, &ctx, project_add_language_iter)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, !ctx.missing);
	return true;
}

static bool
func_files(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | obj_string }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	return coerce_files(wk, an[0].node, an[0].val, res);
}

struct find_program_iter_ctx {
	bool found;
	uint32_t node, version_node;
	obj version;
	obj dirs;
	obj *res;
	enum requirement_type requirement;
};

struct find_program_custom_dir_ctx {
	const char *prog;
	struct sbuf *buf;
	bool found;
};

static enum iteration_result
find_program_custom_dir_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct find_program_custom_dir_ctx *ctx = _ctx;

	path_join(wk, ctx->buf, get_cstr(wk, val), ctx->prog);

	if (fs_file_exists(ctx->buf->buf)) {
		ctx->found = true;
		return ir_done;
	}

	return ir_cont;
}

static bool
find_program_check_override(struct workspace *wk, struct find_program_iter_ctx *ctx, obj prog)
{
	obj override;
	if (!obj_dict_index(wk, wk->find_program_overrides, prog, &override)) {
		return true;
	}

	obj over = 0, op;
	switch (get_obj_type(wk, override)) {
	case obj_array:
		obj_array_index(wk, override, 0, &op);
		obj_array_index(wk, override, 1, &over);
		break;
	case obj_python_installation:
	case obj_external_program:
		op = override;
		struct obj_external_program *ep = get_obj_external_program(wk, op);

		if (!ep->found) {
			return true;
		}

		if (ctx->version) {
			find_program_guess_version(wk, ep->cmd_array, &over);
		}
		break;
	default: UNREACHABLE;
	}

	if (ctx->version && over) {
		bool comparison_result;
		if (!version_compare(wk, ctx->version_node, get_str(wk, over), ctx->version, &comparison_result)) {
			return false;
		} else if (!comparison_result) {
			return true;
		}
	}

	if (get_obj_type(wk, op) == obj_file) {
		obj newres;
		make_obj(wk, &newres, obj_external_program);
		struct obj_external_program *ep = get_obj_external_program(wk, newres);
		ep->found = true;
		make_obj(wk, &ep->cmd_array, obj_array);
		obj_array_push(wk, ep->cmd_array, *get_obj_file(wk, op));
		op = newres;
	}

	ctx->found = true;
	*ctx->res = op;
	return true;
}

static bool
find_program_check_fallback(struct workspace *wk, struct find_program_iter_ctx *ctx, obj prog)
{
	obj fallback_arr, subproj_name;
	if (obj_dict_index(wk, current_project(wk)->wrap_provides_exes, prog, &fallback_arr)) {
		obj_array_flatten_one(wk, fallback_arr, &subproj_name);

		obj subproj;
		if (!subproject(wk, subproj_name, requirement_auto, NULL, NULL, &subproj)
			&& get_obj_subproject(wk, subproj)->found) {
			return true;
		}

		if (!find_program_check_override(wk, ctx, prog)) {
			return false;
		} else if (!ctx->found) {
			obj _;
			if (!obj_dict_index(wk, wk->find_program_overrides, prog, &_)) {
				vm_warning_at(wk,
					0,
					"subproject %o claims to provide %o, but did not override it",
					subproj_name,
					prog);
			}
		}
	}

	return true;
}

static bool
find_program(struct workspace *wk, struct find_program_iter_ctx *ctx, obj prog)
{
	const char *str;
	obj ver = 0;
	bool guessed_ver = false;

	type_tag tc_allowed = tc_file | tc_string | tc_external_program | tc_python_installation;
	if (!typecheck(wk, ctx->node, prog, tc_allowed)) {
		return false;
	}

	enum obj_type t = get_obj_type(wk, prog);
	switch (t) {
	case obj_file: str = get_file_path(wk, prog); break;
	case obj_string: str = get_cstr(wk, prog); break;
	case obj_python_installation: prog = get_obj_python_installation(wk, prog)->prog;
	/* fallthrough */
	case obj_external_program:
		if (get_obj_external_program(wk, prog)->found) {
			*ctx->res = prog;
			ctx->found = true;
		}
		return true;
	default: UNREACHABLE_RETURN;
	}

	const char *path;

	SBUF(buf);
	struct find_program_custom_dir_ctx dir_ctx = {
		.buf = &buf,
		.prog = str,
	};

	/* 1. Program overrides set via meson.override_find_program() */
	if (t == obj_string) {
		if (!find_program_check_override(wk, ctx, prog)) {
			return false;
		}

		if (ctx->found) {
			return true;
		}
	}

	/* 2. [provide] sections in subproject wrap files, if wrap_mode is set to forcefallback */
	enum wrap_mode wrap_mode = get_option_wrap_mode(wk);
	if (t == obj_string && wrap_mode == wrap_mode_forcefallback) {
		if (!find_program_check_fallback(wk, ctx, prog)) {
			return false;
		}

		if (ctx->found) {
			return true;
		}
	}

	/* TODO: 3. [binaries] section in your machine files */

	/* 4. Directories provided using the dirs: kwarg */
	if (ctx->dirs) {
		obj_array_foreach(wk, ctx->dirs, &dir_ctx, find_program_custom_dir_iter);
		if (dir_ctx.found) {
			path = buf.buf;
			goto found;
		}
	}

	/* 5. Project's source tree relative to the current subdir */
	/*       If you use the return value of configure_file(), the current subdir inside the build tree is used instead */
	path_join(wk, &buf, get_cstr(wk, current_project(wk)->cwd), str);
	if (fs_file_exists(buf.buf)) {
		path = buf.buf;
		goto found;
	}

	/* 6. PATH environment variable */
	if (fs_find_cmd(wk, &buf, str)) {
		path = buf.buf;
		goto found;
	}

	/* 7. [provide] sections in subproject wrap files, if wrap_mode is set to anything other than nofallback */
	if (t == obj_string && wrap_mode != wrap_mode_nofallback && ctx->requirement == requirement_required) {
		if (!find_program_check_fallback(wk, ctx, prog)) {
			return false;
		}

		if (ctx->found) {
			return true;
		}
	}

	/* 8. Special cases */
	if (t == obj_string) {
		if (have_samurai && (strcmp(str, "ninja") == 0 || strcmp(str, "samu") == 0)) {
			make_obj(wk, ctx->res, obj_external_program);
			struct obj_external_program *ep = get_obj_external_program(wk, *ctx->res);
			ep->found = true;
			make_obj(wk, &ep->cmd_array, obj_array);
			obj_array_push(wk, ep->cmd_array, make_str(wk, wk->argv0));
			obj_array_push(wk, ep->cmd_array, make_str(wk, "samu"));

			ctx->found = true;
			return true;
		}
	}

	return true;
found: {
	obj cmd_array;
	make_obj(wk, &cmd_array, obj_array);
	obj_array_push(wk, cmd_array, make_str(wk, path));

	if (ctx->version) {
		find_program_guess_version(wk, cmd_array, &ver);
		guessed_ver = true;

		if (!ver) {
			return true; // no version to check against
		}

		bool comparison_result;
		if (!version_compare(wk, ctx->version_node, get_str(wk, ver), ctx->version, &comparison_result)) {
			return false;
		} else if (!comparison_result) {
			return true;
		}
	}

	make_obj(wk, ctx->res, obj_external_program);
	struct obj_external_program *ep = get_obj_external_program(wk, *ctx->res);
	ep->found = true;
	ep->cmd_array = cmd_array;
	ep->guessed_ver = guessed_ver;
	ep->ver = ver;

	ctx->found = true;
	return true;
}
}

static enum iteration_result
find_program_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct find_program_iter_ctx *ctx = _ctx;

	if (!find_program(wk, ctx, val)) {
		return ir_err;
	}

	return ctx->found ? ir_done : ir_cont;
}

static bool
func_find_program(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | tc_string | tc_file }, ARG_TYPE_NULL };
	enum kwargs {
		kw_required,
		kw_native,
		kw_disabler,
		kw_dirs,
		kw_version,
	};
	struct args_kw akw[] = { [kw_required] = { "required", tc_required_kw },
		[kw_native] = { "native", obj_bool },
		[kw_disabler] = { "disabler", obj_bool },
		[kw_dirs] = { "dirs", TYPE_TAG_LISTIFY | obj_string },
		[kw_version] = { "version", TYPE_TAG_LISTIFY | obj_string },
		0 };
	if (!pop_args(wk, an, akw)) {
		return false;
	}

	enum requirement_type requirement;
	if (!coerce_requirement(wk, &akw[kw_required], &requirement)) {
		return false;
	}

	if (requirement == requirement_skip) {
		if (akw[kw_disabler].set && get_obj_bool(wk, akw[kw_disabler].val)) {
			*res = disabler_id;
		} else {
			make_obj(wk, res, obj_external_program);
			get_obj_external_program(wk, *res)->found = false;
		}
		return true;
	}

	struct find_program_iter_ctx ctx = {
		.node = an[0].node,
		.version = akw[kw_version].val,
		.dirs = akw[kw_dirs].val,
		.res = res,
		.requirement = requirement,
	};
	obj_array_foreach_flat(wk, an[0].val, &ctx, find_program_iter);

	if (!ctx.found) {
		if (requirement == requirement_required) {
			vm_error_at(wk, an[0].node, "program not found");
			return false;
		}

		if (akw[kw_disabler].set && get_obj_bool(wk, akw[kw_disabler].val)) {
			*res = disabler_id;
		} else {
			make_obj(wk, res, obj_external_program);
			get_obj_external_program(wk, *res)->found = false;
		}
	}

	return true;
}

static bool
func_include_directories(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | tc_coercible_inc }, ARG_TYPE_NULL };
	enum kwargs {
		kw_is_system,
	};
	struct args_kw akw[] = { [kw_is_system] = { "is_system", obj_bool }, 0 };
	if (!pop_args(wk, an, akw)) {
		return false;
	}

	bool is_system = akw[kw_is_system].set ? get_obj_bool(wk, akw[kw_is_system].val) : false;

	if (!coerce_include_dirs(wk, an[0].node, an[0].val, is_system, res)) {
		return false;
	}

	return true;
}

static bool
func_generator(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { tc_exe }, ARG_TYPE_NULL };
	enum kwargs {
		kw_output,
		kw_arguments,
		kw_capture,
		kw_depfile,
		kw_depends,
	};
	struct args_kw akw[] = { [kw_output] = { "output", TYPE_TAG_LISTIFY | obj_string, .required = true },
		[kw_arguments] = { "arguments", obj_array, .required = true },
		[kw_capture] = { "capture", obj_bool },
		[kw_depfile] = { "depfile", obj_string },
		[kw_depends] = { "depends", tc_depends_kw },
		0 };

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	obj command;
	make_obj(wk, &command, obj_array);
	obj_array_push(wk, command, an[0].val);
	obj_array_extend(wk, command, akw[kw_arguments].val);

	make_obj(wk, res, obj_generator);
	struct obj_generator *gen = get_obj_generator(wk, *res);

	gen->output = akw[kw_output].val;
	gen->raw_command = command;
	gen->depfile = akw[kw_depfile].val;
	gen->capture = akw[kw_capture].set && get_obj_bool(wk, akw[kw_capture].val);

	if (akw[kw_depends].set) {
		obj depends;
		if (!coerce_files(wk, akw[kw_depends].node, akw[kw_depends].val, &depends)) {
			return false;
		}

		gen->depends = depends;
	}
	return true;
}

static bool
func_assert(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_bool }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { obj_string }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	*res = 0;

	if (!get_obj_bool(wk, an[0].val)) {
		if (ao[0].set) {
			LOG_E("%s", get_cstr(wk, ao[0].val));
		}
		return false;
	}

	return true;
}

static enum iteration_result
message_print_iter(struct workspace *wk, void *_ctx, obj val)
{
	obj_fprintf(wk, log_file(), "%#o ", val);
	return ir_cont;
}

static bool
func_debug(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { tc_message }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	log_plain(log_clr() ? "\033[35mdebug\033[0m: " : "debug: ");
	obj_array_foreach(wk, an[0].val, NULL, message_print_iter);
	log_plain("\n");
	*res = 0;

	return true;
}

static bool
func_message(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { tc_message }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	log_plain(log_clr() ? "\033[34mmessage\033[0m " : "message: ");
	obj_array_foreach(wk, an[0].val, NULL, message_print_iter);
	log_plain("\n");
	*res = 0;

	return true;
}

static bool
func_error(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { tc_message }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	log_plain(log_clr() ? "\033[31merror\033[0m " : "error: ");
	obj_array_foreach(wk, an[0].val, NULL, message_print_iter);
	log_plain("\n");
	*res = 0;

	return false;
}

static bool
func_warning(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { tc_message }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	log_plain(log_clr() ? "\033[33mwarn\033[0m " : "warn: ");
	obj_array_foreach(wk, an[0].val, NULL, message_print_iter);
	log_plain("\n");
	*res = 0;

	return true;
}

static bool
func_run_command(struct workspace *wk, obj _, obj *res)
{
	type_tag tc_allowed_an = tc_string | tc_file | tc_external_program | tc_compiler | tc_python_installation;
	struct args_norm an[] = { { TYPE_TAG_GLOB | tc_allowed_an }, ARG_TYPE_NULL };
	enum kwargs {
		kw_check,
		kw_env,
		kw_capture,
	};
	struct args_kw akw[] = { [kw_check] = { "check", obj_bool },
		[kw_env] = { "env", tc_coercible_env },
		[kw_capture] = { "capture", obj_bool },
		0 };
	if (!pop_args(wk, an, akw)) {
		return false;
	}

	const char *argstr, *envstr;
	uint32_t argc, envc;

	{
		obj arg0;
		obj cmd_file = 0;
		struct find_program_iter_ctx find_program_ctx = {
			.node = an[0].node,
			.res = &cmd_file,
			.requirement = requirement_auto,
		};

		if (!get_obj_array(wk, an[0].val)->len) {
			vm_error_at(wk, an[0].node, "missing command");
			return false;
		}

		obj_array_index(wk, an[0].val, 0, &arg0);

		if (get_obj_type(wk, arg0) == obj_compiler) {
			obj cmd_arr = get_obj_compiler(wk, arg0)->cmd_arr;

			obj tail;
			obj_array_tail(wk, an[0].val, &tail);
			obj_array_dup(wk, cmd_arr, &an[0].val);
			obj_array_extend_nodup(wk, an[0].val, tail);
		} else {
			if (!find_program(wk, &find_program_ctx, arg0)) {
				return false;
			} else if (!find_program_ctx.found) {
				vm_error_at(wk, an[0].node, "unable to find program %o", arg0);
				return false;
			}
			obj_array_set(wk, an[0].val, 0, cmd_file);
		}

		obj args;
		if (!arr_to_args(wk, arr_to_args_external_program, an[0].val, &args)) {
			return false;
		}

		workspace_add_regenerate_deps(wk, args);

		join_args_argstr(wk, &argstr, &argc, args);
	}

	{
		obj env;
		if (!coerce_environment_from_kwarg(wk, &akw[kw_env], true, &env)) {
			return false;
		}
		env_to_envstr(wk, &envstr, &envc, env);
	}

	bool ret = false;
	struct run_cmd_ctx cmd_ctx = { 0 };

	if (!run_cmd(&cmd_ctx, argstr, argc, envstr, envc)) {
		vm_error_at(wk, an[0].node, "%s", cmd_ctx.err_msg);
		goto ret;
	}

	if (akw[kw_check].set && get_obj_bool(wk, akw[kw_check].val) && cmd_ctx.status != 0) {
		vm_error_at(wk, an[0].node, "command failed: '%s'", cmd_ctx.err.buf);
		return false;
	}

	make_obj(wk, res, obj_run_result);
	struct obj_run_result *run_result = get_obj_run_result(wk, *res);
	run_result->status = cmd_ctx.status;
	if (akw[kw_capture].set && !get_obj_bool(wk, akw[kw_capture].val)) {
		run_result->out = make_str(wk, "");
		run_result->err = make_str(wk, "");
	} else {
		run_result->out = sbuf_into_str(wk, &cmd_ctx.out);
		run_result->err = sbuf_into_str(wk, &cmd_ctx.err);
	}

	ret = true;
ret:
	run_cmd_ctx_destroy(&cmd_ctx);
	return ret;
}

static bool
func_run_target(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_command,
		kw_depends,
		kw_env,
	};
	struct args_kw akw[] = { [kw_command] = { "command", tc_command_array, .required = true },
		[kw_depends] = { "depends", tc_depends_kw },
		[kw_env] = { "env", tc_coercible_env },
		0 };
	if (!pop_args(wk, an, akw)) {
		return false;
	}

	struct make_custom_target_opts opts = {
		.name = an[0].val,
		.command_node = akw[kw_command].node,
		.command_orig = akw[kw_command].val,
	};

	if (!make_custom_target(wk, &opts, res)) {
		return false;
	}

	struct obj_custom_target *tgt = get_obj_custom_target(wk, *res);
	tgt->flags |= custom_target_console;

	if (akw[kw_depends].set) {
		obj depends;
		if (!coerce_files(wk, akw[kw_depends].node, akw[kw_depends].val, &depends)) {
			return false;
		}

		obj_array_extend_nodup(wk, tgt->depends, depends);
	}

	if (!coerce_environment_from_kwarg(wk, &akw[kw_env], true, &tgt->env)) {
		return false;
	}

	L("adding run target '%s'", get_cstr(wk, tgt->name));
	obj_array_push(wk, current_project(wk)->targets, *res);
	return true;
}

static enum iteration_result
subdir_if_found_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct obj_dependency *dep = get_obj_dependency(wk, v);

	bool *all_found = _ctx;

	if (!(dep->flags & dep_flag_found)) {
		*all_found = false;
		return ir_done;
	}

	return ir_cont;
}

static bool
func_subdir(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_if_found,
	};
	type_tag if_found_type = wk->vm.in_analyzer ? tc_any : TYPE_TAG_LISTIFY | tc_dependency;
	struct args_kw akw[] = { [kw_if_found] = { "if_found", if_found_type }, 0 };
	if (!pop_args(wk, an, akw)) {
		return false;
	}

	if (akw[kw_if_found].set && !wk->vm.in_analyzer) {
		bool all_found = true;
		obj_array_foreach(wk, akw[kw_if_found].val, &all_found, subdir_if_found_iter);

		if (!all_found) {
			return true;
		}
	}

	SBUF(build_dir);

	obj old_cwd = current_project(wk)->cwd;
	obj old_build_dir = current_project(wk)->build_dir;

	SBUF(new_cwd);
	path_join(wk, &new_cwd, get_cstr(wk, old_cwd), get_cstr(wk, an[0].val));
	current_project(wk)->cwd = make_str(wk, new_cwd.buf);

	path_join(wk, &build_dir, get_cstr(wk, old_build_dir), get_cstr(wk, an[0].val));
	current_project(wk)->build_dir = sbuf_into_str(wk, &build_dir);

	bool ret = false;
	if (!wk->vm.in_analyzer) {
		if (!fs_mkdir_p(build_dir.buf)) {
			goto ret;
		}
	}

	path_push(wk, &new_cwd, "meson.build");
	ret = wk->vm.behavior.eval_project_file(wk, new_cwd.buf, false);

ret:
	current_project(wk)->cwd = old_cwd;
	current_project(wk)->build_dir = old_build_dir;

	return ret;
}

static bool
func_configuration_data(struct workspace *wk, obj _, obj *res)
{
	struct args_norm ao[] = { { obj_dict }, ARG_TYPE_NULL };
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_configuration_data);

	if (ao[0].set) {
		get_obj_configuration_data(wk, *res)->dict = ao[0].val;
	} else {
		obj dict;
		make_obj(wk, &dict, obj_dict);
		get_obj_configuration_data(wk, *res)->dict = dict;
	}

	return true;
}

static bool
func_add_test_setup(struct workspace *wk, obj _, obj *ret)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_env,
		kw_exclude_suites,
		kw_exe_wrapper,
		kw_gdb,
		kw_is_default,
		kw_timeout_multiplier,
	};
	struct args_kw akw[] = {
		[kw_env] = { "env", tc_coercible_env, },
		[kw_exclude_suites] = { "exclude_suites", TYPE_TAG_LISTIFY | obj_string },
		[kw_exe_wrapper] = { "exe_wrapper", tc_command_array },
		[kw_gdb] = { "gdb", obj_bool },
		[kw_is_default] = { "is_default", obj_bool },
		[kw_timeout_multiplier] = { "timeout_multiplier", obj_number },
		0
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	obj test_setup;
	make_obj(wk, &test_setup, obj_array);

	obj env = 0;
	if (akw[kw_env].set && !coerce_environment_from_kwarg(wk, &akw[kw_env], false, &env)) {
		return false;
	}

	obj exe_wrapper = 0;
	if (akw[kw_exe_wrapper].set
		&& !arr_to_args(wk,
			arr_to_args_build_target | arr_to_args_custom_target | arr_to_args_external_program,
			akw[kw_exe_wrapper].val,
			&exe_wrapper)) {
		return false;
	}

	/* [name, env, exclude_suites, exe_wrapper, is_default, timeout_multiplier] */

	obj_array_push(wk, test_setup, an[0].val);
	obj_array_push(wk, test_setup, env);
	obj_array_push(wk, test_setup, akw[kw_exclude_suites].val);
	obj_array_push(wk, test_setup, exe_wrapper);
	obj_array_push(wk, test_setup, akw[kw_is_default].val);
	obj_array_push(wk, test_setup, akw[kw_timeout_multiplier].val);

	if (!current_project(wk)->test_setups) {
		make_obj(wk, &current_project(wk)->test_setups, obj_array);
	}

	obj_array_push(wk, current_project(wk)->test_setups, test_setup);
	return true;
}

struct add_test_depends_ctx {
	struct obj_test *t;
	bool from_custom_tgt;
};

static enum iteration_result
add_test_depends_iter(struct workspace *wk, void *_ctx, obj val)
{
	SBUF(rel);
	struct add_test_depends_ctx *ctx = _ctx;

	switch (get_obj_type(wk, val)) {
	case obj_string:
	case obj_external_program:
	case obj_python_installation: break;

	case obj_file:
		if (!ctx->from_custom_tgt) {
			break;
		}

		path_relative_to(wk, &rel, wk->build_root, get_file_path(wk, val));
		obj_array_push(wk, ctx->t->depends, sbuf_into_str(wk, &rel));
		break;

	case obj_both_libs: val = get_obj_both_libs(wk, val)->dynamic_lib;
	/* fallthrough */
	case obj_build_target: {
		struct obj_build_target *tgt = get_obj_build_target(wk, val);

		path_relative_to(wk, &rel, wk->build_root, get_cstr(wk, tgt->build_path));
		obj_array_push(wk, ctx->t->depends, sbuf_into_str(wk, &rel));
		break;
	}
	case obj_custom_target:
		ctx->from_custom_tgt = true;
		if (!obj_array_foreach(wk, get_obj_custom_target(wk, val)->output, ctx, add_test_depends_iter)) {
			return ir_err;
		}
		ctx->from_custom_tgt = false;
		break;
	default: UNREACHABLE;
	}

	return ir_cont;
}

static bool
add_test_common(struct workspace *wk, enum test_category cat)
{
	type_tag tc_allowed_an = tc_build_target | tc_external_program | tc_file | tc_python_installation;
	struct args_norm an[] = { { obj_string }, { tc_allowed_an }, ARG_TYPE_NULL };
	enum kwargs {
		kw_args,
		kw_workdir,
		kw_depends,
		kw_should_fail,
		kw_env,
		kw_suite,
		kw_priority,
		kw_timeout,
		kw_protocol,
		kw_is_parallel,
		kw_verbose,
	};
	struct args_kw akw[] = {
		[kw_args] = { "args", tc_command_array, },
		[kw_workdir] = { "workdir", obj_string, },
		[kw_depends] = { "depends", tc_depends_kw, },
		[kw_should_fail] = { "should_fail", obj_bool, },
		[kw_env] = { "env", tc_coercible_env, },
		[kw_suite] = { "suite", TYPE_TAG_LISTIFY | obj_string },
		[kw_priority] = { "priority", obj_number, },
		[kw_timeout] = { "timeout", obj_number, },
		[kw_protocol] = { "protocol", obj_string, },
		[kw_is_parallel] = { 0 },
		[kw_verbose] = { "verbose", obj_bool },
		0
	};

	if (cat == test_category_test) {
		akw[kw_is_parallel] = (struct args_kw){
			"is_parallel",
			obj_bool,
		};
	}

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	enum test_protocol protocol = test_protocol_exitcode;
	if (akw[kw_protocol].set) {
		const char *protocol_names[] = {
			[test_protocol_exitcode] = "exitcode",
			[test_protocol_tap] = "tap",
			[test_protocol_gtest] = "gtest",
			[test_protocol_rust] = "rust",
		};

		for (protocol = 0; protocol < ARRAY_LEN(protocol_names); ++protocol) {
			if (str_eql(get_str(wk, akw[kw_protocol].val), &WKSTR(protocol_names[protocol]))) {
				break;
			}
		}

		if (protocol == ARRAY_LEN(protocol_names)) {
			vm_error_at(wk, akw[kw_protocol].node, "invalid protocol %o", akw[kw_protocol].val);
			return false;
		}

		if (protocol == test_protocol_gtest || protocol == test_protocol_rust) {
			vm_warning_at(wk,
				akw[kw_protocol].node,
				"unsupported protocol %o, falling back to 'exitcode'",
				akw[kw_protocol].val);
			protocol = test_protocol_exitcode;
		}
	}

	obj exe, exe_args = 0;
	if (!coerce_executable(wk, an[1].node, an[1].val, &exe, &exe_args)) {
		return false;
	}

	obj args = exe_args;
	if (akw[kw_args].set) {
		if (!arr_to_args(wk, arr_to_args_build_target | arr_to_args_custom_target, akw[kw_args].val, &args)) {
			return false;
		}

		if (exe_args) {
			obj_array_extend_nodup(wk, exe_args, args);
			args = exe_args;
		}
	}

	obj test;
	make_obj(wk, &test, obj_test);
	struct obj_test *t = get_obj_test(wk, test);

	if (!coerce_environment_from_kwarg(wk, &akw[kw_env], false, &t->env)) {
		return false;
	}

	t->name = an[0].val;
	t->exe = exe;
	t->args = args;
	t->should_fail = akw[kw_should_fail].set && get_obj_bool(wk, akw[kw_should_fail].val);
	t->suites = akw[kw_suite].val;
	t->workdir = akw[kw_workdir].val;
	t->timeout = akw[kw_timeout].val;
	t->priority = akw[kw_priority].val;
	t->category = cat;
	t->protocol = protocol;
	t->verbose = akw[kw_verbose].set && get_obj_bool(wk, akw[kw_verbose].val);

	if (akw[kw_is_parallel].key) {
		t->is_parallel = akw[kw_is_parallel].set ? get_obj_bool(wk, akw[kw_is_parallel].val) : true;
	}

	struct add_test_depends_ctx deps_ctx = { .t = t };
	make_obj(wk, &t->depends, obj_array);
	add_test_depends_iter(wk, &deps_ctx, an[1].val);
	if (akw[kw_depends].set) {
		obj_array_foreach(wk, akw[kw_depends].val, &deps_ctx, add_test_depends_iter);
	}
	if (akw[kw_args].set) {
		obj_array_foreach(wk, akw[kw_args].val, &deps_ctx, add_test_depends_iter);
	}

	obj_array_push(wk, current_project(wk)->tests, test);
	return true;
}

static bool
func_test(struct workspace *wk, obj _, obj *ret)
{
	return add_test_common(wk, test_category_test);
}

static bool
func_benchmark(struct workspace *wk, obj _, obj *ret)
{
	return add_test_common(wk, test_category_benchmark);
}

struct join_paths_ctx {
	struct sbuf *buf;
};

static enum iteration_result
join_paths_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct join_paths_ctx *ctx = _ctx;

	if (!typecheck(wk, 0, val, obj_string)) {
		return ir_err;
	}

	path_push(wk, ctx->buf, get_cstr(wk, val));
	return ir_cont;
}

static bool
func_join_paths(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | obj_string }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(join_paths_buf);
	struct join_paths_ctx ctx = {
		.buf = &join_paths_buf,
	};

	if (!obj_array_foreach_flat(wk, an[0].val, &ctx, join_paths_iter)) {
		return false;
	}

	*res = sbuf_into_str(wk, ctx.buf);
	return true;
}

static enum iteration_result
environment_set_initial_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	obj env = *(obj *)_ctx;

	if (!environment_set(wk, env, environment_set_mode_set, key, val, 0)) {
		return ir_err;
	}

	return ir_cont;
}

static bool
func_environment(struct workspace *wk, obj _, obj *res)
{
	struct args_norm ao[] = { { obj_dict }, ARG_TYPE_NULL };
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_environment);
	struct obj_environment *d = get_obj_environment(wk, *res);
	make_obj(wk, &d->actions, obj_array);

	if (ao[0].set) {
		if (!typecheck(
			    wk, ao[0].node, ao[0].val, make_complex_type(wk, complex_type_nested, tc_dict, tc_string))) {
			return false;
		}

		obj_dict_foreach(wk, ao[0].val, res, environment_set_initial_iter);
	}

	return true;
}

static bool
func_import(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_required,
		kw_disabler,
	};
	struct args_kw akw[]
		= { [kw_required] = { "required", tc_required_kw }, [kw_disabler] = { "disabler", obj_bool }, 0 };

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	if (wk->vm.in_analyzer) {
		// If we are in the analyzer, don't create a disabler here so
		// that the custom not found module logic can be used
		akw[kw_disabler].set = false;
	}

	enum requirement_type requirement;
	if (!coerce_requirement(wk, &akw[kw_required], &requirement)) {
		return false;
	}

	bool found = false;

	if (requirement == requirement_skip) {
		make_obj(wk, res, obj_module);
	} else {
		if (module_import(wk, get_cstr(wk, an[0].val), true, res)) {
			found = true;
		} else if (requirement == requirement_required) {
			vm_error_at(wk, an[0].node, "module not found");
			return false;
		}
	}

	struct obj_module *m = get_obj_module(wk, *res);
	if (!m->has_impl) {
		if (requirement != requirement_required || wk->vm.in_analyzer) {
			found = false;
		} else {
			LOG_W("importing unimplemented module '%s'", get_cstr(wk, an[0].val));
		}
	}

	if (!found && akw[kw_disabler].set && get_obj_bool(wk, akw[kw_disabler].val)) {
		*res = disabler_id;
		return true;
	}

	return true;
}

static bool
func_is_disabler(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { tc_any }, ARG_TYPE_NULL };

	disabler_among_args_immunity = true;
	if (!pop_args(wk, an, NULL)) {
		return false;
	}
	disabler_among_args_immunity = false;

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, an[0].val == disabler_id);
	return true;
}

static bool
func_disabler(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = disabler_id;
	return true;
}

static bool
func_set_variable(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_string }, { tc_any }, ARG_TYPE_NULL };
	disabler_among_args_immunity = true;
	if (!pop_args(wk, an, NULL)) {
		return false;
	}
	disabler_among_args_immunity = false;

	wk->vm.behavior.assign_variable(wk, get_cstr(wk, an[0].val), an[1].val, 0, assign_local);
	return true;
}

static bool
func_unset_variable(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	const char *varname = get_cstr(wk, an[0].val);
	obj _val;

	if (wk->vm.behavior.get_variable(wk, varname, &_val)) {
		wk->vm.behavior.unassign_variable(wk, varname);
	} else {
		vm_error_at(wk, an[0].node, "cannot unset undefined variable: %o", an[0].val);
		return false;
	}

	return true;
}

static bool
func_get_variable(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { tc_any }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { tc_any }, ARG_TYPE_NULL };
	disabler_among_args_immunity = true;
	if (!pop_args(wk, an, NULL)) {
		return false;
	}
	disabler_among_args_immunity = false;

	if (an[0].val == disabler_id) {
		*res = disabler_id;
		return true;
	} else if (!typecheck(wk, an[0].node, an[0].val, obj_string)) {
		return false;
	}

	if (!wk->vm.behavior.get_variable(wk, get_cstr(wk, an[0].val), res)) {
		if (ao[0].set) {
			*res = ao[0].val;
		} else {
			vm_error_at(wk, an[0].node, "undefined object %o", an[0].val);
			return false;
		}
	}

	return true;
}

static bool
func_is_variable(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	disabler_among_args_immunity = true;
	if (!pop_args(wk, an, NULL)) {
		return false;
	}
	disabler_among_args_immunity = false;

	obj dont_care;

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, wk->vm.behavior.get_variable(wk, get_cstr(wk, an[0].val), &dont_care));
	return true;
}

static bool
func_subdir_done(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	return true;
}

static bool
func_summary(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { tc_any }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { tc_any }, ARG_TYPE_NULL };
	enum kwargs {
		kw_section,
		kw_bool_yn, // ignored
		kw_list_sep, // ignored
	};
	struct args_kw akw[] = {
		[kw_section] = { "section", obj_string, },
		[kw_bool_yn] = { "bool_yn", obj_bool, },
		[kw_list_sep] = { "list_sep", obj_string, },
		0
	};
	if (!pop_args(wk, an, akw)) {
		return false;
	}

	obj sec = akw[kw_section].set ? akw[kw_section].val : make_str(wk, "");
	obj dict;

	if (ao[0].set) {
		if (!typecheck(wk, an[0].node, an[0].val, obj_string)) {
			return false;
		}

		make_obj(wk, &dict, obj_dict);
		obj_dict_set(wk, dict, an[0].val, ao[0].val);
	} else {
		if (!typecheck(wk, an[0].node, an[0].val, obj_dict)) {
			return false;
		}

		dict = an[0].val;
	}

	obj prev;
	if (obj_dict_index(wk, current_project(wk)->summary, sec, &prev)) {
		obj ndict;
		obj_dict_merge(wk, prev, dict, &ndict);
		dict = ndict;
	}

	obj_dict_set(wk, current_project(wk)->summary, sec, dict);
	return true;
}

static obj
make_alias_target(struct workspace *wk, obj name, obj deps)
{
	assert(get_obj_type(wk, name) == obj_string && "Alias target name must be a string.");
	assert(get_obj_type(wk, deps) == obj_array && "Alias target list must be an array.");

	obj id;
	make_obj(wk, &id, obj_alias_target);
	struct obj_alias_target *alias_tgt = get_obj_alias_target(wk, id);

	alias_tgt->name = name;
	alias_tgt->depends = deps;

	return id;
}

struct alias_target_iter_ctx {
	obj deps;
};

static enum iteration_result
push_alias_target_deps_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct alias_target_iter_ctx *ctx = _ctx;
	enum obj_type t = get_obj_type(wk, val);
	switch (t) {
	case obj_both_libs: val = get_obj_both_libs(wk, val)->dynamic_lib;
	/* fallthrough */
	case obj_alias_target:
	case obj_build_target:
	case obj_custom_target: obj_array_push(wk, ctx->deps, val); break;
	default: vm_error_at(wk, val, "expected target but got: %s", obj_type_to_s(t)); return ir_err;
	}

	return ir_cont;
}

static bool
func_alias_target(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_string },
		{ TYPE_TAG_GLOB | tc_build_target | tc_custom_target | tc_alias_target | tc_both_libs },
		ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	L("adding alias target '%s'", get_cstr(wk, an[0].val));

	obj deps_id;
	make_obj(wk, &deps_id, obj_array);

	struct alias_target_iter_ctx iter_ctx = {
		.deps = deps_id,
	};

	if (!obj_array_foreach_flat(wk, an[1].val, &iter_ctx, push_alias_target_deps_iter)) {
		return false;
	}

	*res = make_alias_target(wk, an[0].val, deps_id);
	obj_array_push(wk, current_project(wk)->targets, *res);

	return true;
}

bool
func_range_common(struct workspace *wk, uint32_t args_node, struct range_params *res)
{
	struct args_norm an[] = { { obj_number }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { obj_number }, { obj_number }, { obj_number }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	int64_t n = get_obj_number(wk, an[0].val);
	if (!rangecheck(wk, an[0].node, 0, UINT32_MAX, n)) {
		return false;
	}
	res->start = n;

	if (ao[0].set) {
		int64_t n = get_obj_number(wk, ao[0].val);
		if (!rangecheck(wk, ao[0].node, res->start, UINT32_MAX, n)) {
			return false;
		}

		res->stop = n;
	} else {
		res->stop = res->start;
		res->start = 0;
	}

	if (ao[1].set) {
		int64_t n = get_obj_number(wk, ao[1].val);
		if (!rangecheck(wk, ao[1].node, 1, UINT32_MAX, n)) {
			return false;
		}
		res->step = n;
	} else {
		res->step = 1;
	}

	return true;
}

static bool
func_range(struct workspace *wk, obj _, obj *res)
{
	struct range_params params = { 0 };
	if (!func_range_common(wk, 0, &params)) {
		return false;
	}

	make_obj(wk, res, obj_array);

	uint32_t i;
	for (i = params.start; i < params.stop; i += params.step) {
		obj num;
		make_obj(wk, &num, obj_number);
		set_obj_number(wk, num, i);
		obj_array_push(wk, *res, num);
	}
	return true;
}

/*
 * muon extension funcitons
 */

static bool
func_p(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { tc_any | TYPE_TAG_ALLOW_VOID }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	obj_fprintf(wk, log_file(), "%o\n", an[0].val);
	*res = an[0].val;
	return true;
}

static bool
func_dbg(struct workspace *wk, obj _, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	LOG_I("entering debugger, type \\help for help");
	repl(wk, true);

	return true;
}

static bool
func_serial_load(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { tc_string | tc_file }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	obj str;
	coerce_string(wk, an[0].node, an[0].val, &str);

	FILE *f;
	if (str_eql(get_str(wk, str), &WKSTR("-"))) {
		f = stdin;
	} else if (!(f = fs_fopen(get_cstr(wk, str), "rb"))) {
		return false;
	}

	bool ret = false;
	if (!serial_load(wk, res, f)) {
		goto ret;
	}

	if (!fs_fclose(f)) {
		goto ret;
	}

	ret = true;
ret:
	return ret;
}

static bool
func_serial_dump(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { tc_string | tc_file }, { tc_any }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	obj str;
	coerce_string(wk, an[0].node, an[0].val, &str);

	FILE *f;
	if (!(f = fs_fopen(get_cstr(wk, str), "wb"))) {
		return false;
	}

	bool ret = false;
	if (!serial_dump(wk, an[1].val, f)) {
		goto ret;
	}

	if (!fs_fclose(f)) {
		goto ret;
	}

	ret = true;
ret:
	return ret;
}

static bool
func_is_void(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_ALLOW_VOID | tc_any }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	if (get_obj_type(wk, an[0].val) == obj_null) {
		*res = obj_bool_true;
	} else {
		*res = obj_bool_false;
	}

	return true;
}

static bool
func_typeof(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_ALLOW_VOID | tc_any }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	*res = make_str(wk, obj_type_to_s(get_obj_type(wk, an[0].val)));

	return true;
}

// clang-format off
const struct func_impl impl_tbl_kernel[] =
{
	{ "add_global_arguments", func_add_global_arguments },
	{ "add_global_link_arguments", func_add_global_link_arguments },
	{ "add_languages", func_add_languages, tc_bool },
	{ "add_project_arguments", func_add_project_arguments },
	{ "add_project_dependencies", func_add_project_dependencies },
	{ "add_project_link_arguments", func_add_project_link_arguments },
	{ "add_test_setup", func_add_test_setup },
	{ "alias_target", func_alias_target, tc_alias_target },
	{ "assert", func_assert },
	{ "benchmark", func_benchmark },
	{ "both_libraries", func_both_libraries, tc_both_libs },
	{ "build_target", func_build_target, tc_build_target | tc_both_libs },
	{ "configuration_data", func_configuration_data, tc_configuration_data },
	{ "configure_file", func_configure_file, tc_file },
	{ "custom_target", func_custom_target, tc_custom_target },
	{ "debug", func_debug },
	{ "declare_dependency", func_declare_dependency, tc_dependency },
	{ "dependency", func_dependency, tc_dependency },
	{ "disabler", func_disabler, tc_disabler },
	{ "environment", func_environment, tc_environment },
	{ "error", func_error },
	{ "executable", func_executable, tc_build_target },
	{ "files", func_files, tc_array },
	{ "find_program", func_find_program, tc_external_program },
	{ "generator", func_generator, tc_generator },
	{ "get_option", func_get_option, tc_string | tc_number | tc_bool | tc_feature_opt | tc_array, },
	{ "get_variable", func_get_variable, tc_any, true },
	{ "import", func_import, tc_module, true },
	{ "include_directories", func_include_directories, tc_array },
	{ "install_data", func_install_data },
	{ "install_emptydir", func_install_emptydir },
	{ "install_headers", func_install_headers },
	{ "install_man", func_install_man },
	{ "install_subdir", func_install_subdir },
	{ "install_symlink", func_install_symlink },
	{ "is_disabler", func_is_disabler, tc_bool, true },
	{ "is_variable", func_is_variable, tc_bool, true },
	{ "join_paths", func_join_paths, tc_string, true },
	{ "library", func_library, tc_build_target | tc_both_libs },
	{ "message", func_message },
	{ "project", func_project },
	{ "range", func_range, tc_array, true },
	{ "run_command", func_run_command, tc_run_result },
	{ "run_target", func_run_target, tc_custom_target },
	{ "set_variable", func_set_variable, 0, true },
	{ "shared_library", func_shared_library, tc_build_target },
	{ "shared_module", func_shared_module, tc_build_target },
	{ "static_library", func_static_library, tc_build_target },
	{ "subdir", func_subdir, 0, true },
	{ "subdir_done", func_subdir_done },
	{ "subproject", func_subproject, tc_subproject },
	{ "summary", func_summary },
	{ "test", func_test },
	{ "unset_variable", func_unset_variable, 0, true },
	{ "vcs_tag", func_vcs_tag, tc_custom_target },
	{ "warning", func_warning },
	// non-standard muon extensions
	{ "dbg", func_dbg, .extension = true },
	{ "p", func_p, tc_any, true, .extension = true },
	{ NULL, NULL },
};

const struct func_impl impl_tbl_kernel_internal[] = {
	{ "assert", func_assert },
	{ "configuration_data", func_configuration_data, tc_configuration_data },
	{ "disabler", func_disabler, tc_disabler },
	{ "environment", func_environment, tc_environment },
	{ "error", func_error },
	{ "files", func_files, tc_array },
	{ "find_program", func_find_program, tc_external_program },
	{ "get_variable", func_get_variable, tc_any, true },
	{ "import", func_import, tc_module, true },
	{ "is_disabler", func_is_disabler, tc_bool, true },
	{ "is_variable", func_is_variable, tc_bool, true },
	{ "join_paths", func_join_paths, tc_string, true },
	{ "message", func_message },
	{ "range", func_range, tc_array, true },
	{ "run_command", func_run_command, tc_run_result, .fuzz_unsafe = true },
	{ "set_variable", func_set_variable, 0, true },
	{ "unset_variable", func_unset_variable, 0, true },
	{ "warning", func_warning },
	// non-standard muon extensions
	{ "dbg", func_dbg },
	{ "p", func_p, tc_any },
	{ "serial_load", func_serial_load, tc_any },
	{ "serial_dump", func_serial_dump, .fuzz_unsafe = true },
	{ "is_void", func_is_void, tc_bool, true },
	{ "typeof", func_typeof, tc_string, true },
	{ NULL, NULL },
};

const struct func_impl impl_tbl_kernel_opts[] = {
	{ "option", func_option, 0, true },
	// non-standard muon extensions
	{ "dbg", func_dbg },
	{ "p", func_p, tc_any },
	{ NULL, NULL },
};

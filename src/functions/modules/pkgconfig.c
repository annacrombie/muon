/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: illiliti <illiliti@dimension.sh>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <assert.h>
#include <string.h>

#include "args.h"
#include "error.h"
#include "functions/custom_target.h"
#include "functions/file.h"
#include "install.h"
#include "lang/typecheck.h"
#include "log.h"
#include "options.h"
#include "platform/filesystem.h"
#include "platform/path.h"

enum pkgconf_visibility {
	pkgconf_visibility_pub,
	pkgconf_visibility_priv,
};

struct pkgconf_file {
	// strings
	obj name, description, url, version;

	// arrays of string
	obj cflags, conflicts;
	obj builtin_dir_variables, variables;
	obj reqs[2], libs[2];
	obj exclude;
	bool libs_contains_internal[2];

	bool dataonly;
};

enum module_pkgconf_diropt {
	module_pkgconf_diropt_prefix,
	module_pkgconf_diropt_bindir,
	module_pkgconf_diropt_datadir,
	module_pkgconf_diropt_includedir,
	module_pkgconf_diropt_infodir,
	module_pkgconf_diropt_libdir,
	module_pkgconf_diropt_libexecdir,
	module_pkgconf_diropt_localedir,
	module_pkgconf_diropt_localstatedir,
	module_pkgconf_diropt_mandir,
	module_pkgconf_diropt_sbindir,
	module_pkgconf_diropt_sharedstatedir,
	module_pkgconf_diropt_sysconfdir,
};

static struct { const char *const name, *const optname; bool refd, added; } module_pkgconf_diropts[] = {
	[module_pkgconf_diropt_prefix]         = { "${prefix}",         "prefix" },
	[module_pkgconf_diropt_bindir]         = { "${bindir}",         "bindir" },
	[module_pkgconf_diropt_datadir]        = { "${datadir}",        "datadir" },
	[module_pkgconf_diropt_includedir]     = { "${includedir}",     "includedir" },
	[module_pkgconf_diropt_infodir]        = { "${infodir}",        "infodir" },
	[module_pkgconf_diropt_libdir]         = { "${libdir}",         "libdir" },
	[module_pkgconf_diropt_libexecdir]     = { "${libexecdir}",     "libexecdir" },
	[module_pkgconf_diropt_localedir]      = { "${localedir}",      "localedir" },
	[module_pkgconf_diropt_localstatedir]  = { "${localstatedir}",  "localstatedir" },
	[module_pkgconf_diropt_mandir]         = { "${mandir}",         "mandir" },
	[module_pkgconf_diropt_sbindir]        = { "${sbindir}",        "sbindir" },
	[module_pkgconf_diropt_sharedstatedir] = { "${sharedstatedir}", "sharedstatedir" },
	[module_pkgconf_diropt_sysconfdir]     = { "${sysconfdir}",     "sysconfdir" },
};

static enum iteration_result
add_subdirs_includes_iter(struct workspace *wk, void *_ctx, obj val)
{
	obj *cflags = _ctx;

	if (str_eql(get_str(wk, val), &WKSTR("."))) {
		obj_array_push(wk, *cflags, make_str(wk, "-I${includedir}"));
	} else {
		SBUF(path);
		path_join(wk, &path, "-I${includedir}", get_cstr(wk, val));
		obj_array_push(wk, *cflags, sbuf_into_str(wk, &path));
	}

	return ir_cont;
}

static bool
module_pkgconf_lib_to_lname(struct workspace *wk, obj lib, obj *res)
{
	SBUF(basename);
	const char *str;

	switch (get_obj_type(wk, lib)) {
	case obj_string:
		str = get_cstr(wk, lib);
		break;
	case obj_file: {
		path_basename(wk, &basename, get_file_path(wk, lib));

		char *dot;
		if ((dot = strrchr(basename.buf, '.'))) {
			*dot = '\0';
		}

		str = basename.buf;
		break;
	}
	default:
		UNREACHABLE;
	}

	if (str[0] == '-') {
		*res = make_str(wk, str);
		return true;
	}

	struct str s = WKSTR(str);
	if (str_startswith(&s, &WKSTR("-l"))) {
		s.len -= 2;
		s.s += 2;
	} else if (str_startswith(&s, &WKSTR("lib"))) {
		s.len -= 3;
		s.s += 3;
	}

	*res = make_strf(wk, "-l%.*s", s.len, s.s);
	return true;
}

struct module_pkgconf_process_reqs_iter_ctx {
	uint32_t err_node;
	obj dest;
};

static enum iteration_result
module_pkgconf_process_reqs_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct module_pkgconf_process_reqs_iter_ctx *ctx = _ctx;

	switch (get_obj_type(wk, val)) {
	case obj_string:
		obj_array_push(wk, ctx->dest, val);
		break;
	case obj_both_libs:
		val = get_obj_both_libs(wk, val)->dynamic_lib;
	/* fallthrough */
	case obj_build_target: {
		struct obj_build_target *tgt = get_obj_build_target(wk, val);
		if (!tgt->generated_pc) {
			vm_error_at(wk, ctx->err_node, "build target has no associated pc file");
			return ir_err;
		}

		obj_array_push(wk, ctx->dest, tgt->generated_pc);
		break;
	}
	case obj_dependency: {
		struct obj_dependency *dep = get_obj_dependency(wk, val);
		if (!(dep->flags & dep_flag_found)
		    || (dep->type == dependency_type_threads)) {
			return ir_cont;
		}

		if (dep->type != dependency_type_pkgconf) {
			vm_error_at(wk, ctx->err_node, "dependency not from pkgconf");
			return ir_err;
		}

		obj_array_push(wk, ctx->dest, dep->name);
		// TODO: handle version req
		break;
	}
	default:
		vm_error_at(wk, ctx->err_node, "invalid type for pkgconf require %s", obj_type_to_s(get_obj_type(wk, val)));
		return ir_err;
	}

	return ir_cont;
}

static bool
module_pkgconf_process_reqs(struct workspace *wk, uint32_t err_node, obj reqs, obj dest)
{
	struct module_pkgconf_process_reqs_iter_ctx ctx = {
		.err_node = err_node,
		.dest = dest,
	};

	if (!obj_array_foreach(wk, reqs, &ctx, module_pkgconf_process_reqs_iter)) {
		return false;
	}
	return true;
}

struct module_pkgconf_process_libs_iter_ctx {
	uint32_t err_node;
	struct pkgconf_file *pc;
	enum pkgconf_visibility vis;
	bool link_whole;
};

static bool module_pkgconf_process_libs(struct workspace *wk, uint32_t err_node, obj src,
	struct pkgconf_file *pc, enum pkgconf_visibility vis, bool link_whole);

static enum iteration_result
str_to_file_iter(struct workspace *wk, void *_ctx, obj v)
{
	obj *arr = _ctx;

	obj f;
	make_obj(wk, &f, obj_file);
	*get_obj_file(wk, f) = v;
	obj_array_push(wk, *arr, f);
	return ir_cont;
}

static enum iteration_result
module_pkgconf_process_libs_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct module_pkgconf_process_libs_iter_ctx *ctx = _ctx;

	/* obj_fprintf(wk, log_file(), "%o\n", val); */

	switch (get_obj_type(wk, val)) {
	case obj_string: {
		obj lib;
		if (!module_pkgconf_lib_to_lname(wk, val, &lib)) {
			return ir_err;
		}

		obj_array_push(wk, ctx->pc->libs[ctx->vis], lib);
		break;
	}
	case obj_file: {
		if (!file_is_linkable(wk, val)) {
			vm_error_at(wk, ctx->err_node, "non linkable file %o among libraries", val);
			return ir_err;
		}

		if (path_is_subpath(wk->source_root, get_file_path(wk, val))
		    || path_is_subpath(wk->build_root, get_file_path(wk, val))) {
			ctx->pc->libs_contains_internal[ctx->vis] = true;
		}

		obj lib;
		if (!module_pkgconf_lib_to_lname(wk, val, &lib)) {
			return ir_err;
		}

		obj_array_push(wk, ctx->pc->libs[ctx->vis], lib);
		break;
	}
	case obj_both_libs:
		val = get_obj_both_libs(wk, val)->dynamic_lib;
	/* fallthrough */
	case obj_build_target: {
		struct obj_build_target *tgt = get_obj_build_target(wk, val);
		if (tgt->generated_pc) {
			obj_array_push(wk, ctx->pc->reqs[ctx->vis], tgt->generated_pc);
		} else {
			if (tgt->type == tgt_executable) {
				vm_error_at(wk, ctx->err_node, "invalid build_target type");
				return ir_err;
			}

			if (tgt->dep.raw.deps) {
				if (!module_pkgconf_process_libs(wk, ctx->err_node,
					tgt->dep.raw.deps, ctx->pc, pkgconf_visibility_priv, false)) {
					return ir_err;
				}
			}

			const enum pkgconf_visibility link_vis =
				tgt->type == tgt_static_library
				? pkgconf_visibility_pub
				: pkgconf_visibility_priv;

			if (tgt->dep.raw.link_with) {
				if (!module_pkgconf_process_libs(wk, ctx->err_node,
					tgt->dep.raw.link_with, ctx->pc, link_vis, false)) {
					return ir_err;
				}
			}

			if (tgt->dep.raw.link_whole) {
				if (!module_pkgconf_process_libs(wk, ctx->err_node,
					tgt->dep.raw.link_whole, ctx->pc, link_vis, true)) {
					return ir_err;
				}
			}

			obj lib;
			if (!module_pkgconf_lib_to_lname(wk, tgt->name, &lib)) {
				return ir_err;
			}

			if (ctx->link_whole) {
				obj_array_push(wk, ctx->pc->exclude, lib);
				return ir_cont;
			} else if ((tgt->type == tgt_static_library && !(tgt->flags & build_tgt_flag_installed))) {
				return ir_cont;
			}

			ctx->pc->libs_contains_internal[ctx->vis] = true;
			obj_array_push(wk, ctx->pc->libs[ctx->vis], lib);
		}
		break;
	}
	case obj_dependency: {
		struct obj_dependency *dep = get_obj_dependency(wk, val);
		if (!(dep->flags & dep_flag_found)) {
			return ir_cont;
		}

		switch (dep->type) {
		case dependency_type_declared: {
			// TODO: I'm pretty sure this doesn't obey partial
			// dependency semantics if this is a sub dependency of
			// a partial dep with compile_args: false
			if (dep->dep.compile_args) {
				obj_array_extend(wk, ctx->pc->cflags, dep->dep.compile_args);
			}

			if (dep->dep.raw.link_with) {
				if (!module_pkgconf_process_libs(wk, ctx->err_node,
					dep->dep.raw.link_with, ctx->pc, ctx->vis, false)) {
					return ir_err;
				}
			}

			if (dep->dep.raw.link_whole) {
				if (!module_pkgconf_process_libs(wk, ctx->err_node,
					dep->dep.raw.link_whole, ctx->pc, ctx->vis, true)) {
					return ir_err;
				}
			}

			if (dep->dep.raw.deps) {
				if (!module_pkgconf_process_libs(wk, ctx->err_node,
					dep->dep.raw.deps, ctx->pc, pkgconf_visibility_priv, false)) {
					return ir_err;
				}
			}
			break;
		}
		case dependency_type_pkgconf:
			obj_array_push(wk, ctx->pc->reqs[ctx->vis], dep->name);
			// TODO: handle version req
			break;
		case dependency_type_threads:
			obj_array_push(wk, ctx->pc->libs[pkgconf_visibility_priv], make_str(wk, "-pthread"));
			break;
		case dependency_type_external_library: {
			obj link_with_files;
			make_obj(wk, &link_with_files, obj_array);
			obj_array_foreach(wk, dep->dep.link_with, &link_with_files, str_to_file_iter);

			if (!module_pkgconf_process_libs(wk, ctx->err_node,
				link_with_files, ctx->pc, ctx->vis, false)) {
				return ir_err;
			}
			break;
		}
		case dependency_type_appleframeworks:
			// TODO: actually add correct -framework arguments
			break;
		case dependency_type_not_found:
			break;
		}
		break;
	}
	case obj_custom_target: {
		if (!custom_target_is_linkable(wk, val)) {
			vm_error_at(wk, ctx->err_node, "non linkable custom target %o among libraries", val);
			return ir_err;
		}

		struct obj_custom_target *tgt = get_obj_custom_target(wk, val);
		obj out;
		obj_array_index(wk, tgt->output, 0, &out);

		if (str_endswith(get_str(wk, *get_obj_file(wk, out)), &WKSTR(".a"))) {
			return ir_cont;
		}

		obj lib;
		if (!module_pkgconf_lib_to_lname(wk, out, &lib)) {
			return ir_err;
		}

		ctx->pc->libs_contains_internal[ctx->vis] = true;
		obj_array_push(wk, ctx->pc->libs[ctx->vis], lib);
		break;
	}
	default:
		vm_error_at(wk, ctx->err_node, "invalid type for pkgconf library %s", obj_type_to_s(get_obj_type(wk, val)));
		return ir_err;
	}

	return ir_cont;
}

static bool
module_pkgconf_process_libs(struct workspace *wk, uint32_t err_node, obj src,
	struct pkgconf_file *pc, enum pkgconf_visibility vis, bool link_whole)
{
	struct module_pkgconf_process_libs_iter_ctx ctx = {
		.pc = pc,
		.err_node = err_node,
		.vis = vis,
		.link_whole = link_whole,
	};

	if (get_obj_type(wk, src) == obj_array) {
		if (!obj_array_foreach(wk, src, &ctx, module_pkgconf_process_libs_iter)) {
			return false;
		}
	} else {
		if (module_pkgconf_process_libs_iter(wk, &ctx, src) == ir_err) {
			return false;
		}
	}

	return true;
}

static bool
module_pkgconf_declare_var(struct workspace *wk, uint32_t err_node, bool escape, bool skip_reserved_check,
	const struct str *key, const struct str *val, obj dest)
{
	if (!skip_reserved_check) {
		const char *reserved[] = { "prefix", "libdir", "includedir", NULL };
		uint32_t i;
		for (i = 0; reserved[i]; ++i) {
			if (str_eql(key, &WKSTR(reserved[i]))) {
				vm_error_at(wk, err_node, "variable %s is reserved", reserved[i]);
				return false;
			}
		}

		for (i = 0; i < ARRAY_LEN(module_pkgconf_diropts); ++i) {
			if (str_eql(key, &WKSTR(module_pkgconf_diropts[i].optname))) {
				module_pkgconf_diropts[i].added = true;
			}

			if (module_pkgconf_diropts[i].refd) {
				continue;
			}

			if (str_startswith(val, &WKSTR(module_pkgconf_diropts[i].name))) {
				module_pkgconf_diropts[module_pkgconf_diropt_prefix].refd = true; // prefix
				module_pkgconf_diropts[i].refd = true;
			}
		}
	}

	SBUF(esc);
	const char *esc_val;
	if (escape) {
		pkgconf_escape(wk, &esc, val->s);
		esc_val = esc.buf;
	} else {
		esc_val = val->s;
	}

	obj_array_push(wk, dest, make_strf(wk, "%.*s=%s", key->len, key->s, esc_val));
	return true;
}

static void
module_pkgconf_declare_builtin_dir_var(struct workspace *wk, const char *opt, obj dest)
{
	obj val, valstr;
	get_option_value(wk, current_project(wk), opt, &val);
	if (strcmp(opt, "prefix") == 0) {
		valstr = val;
	} else {
		valstr = make_strf(wk, "${prefix}/%s", get_cstr(wk, val));
	}

	module_pkgconf_declare_var(wk, 0, true, true, &WKSTR(opt), get_str(wk, valstr), dest);
}

struct module_pkgconf_process_vars_ctx {
	uint32_t err_node;
	bool escape, dataonly;
	obj dest;
};

static enum iteration_result
module_pkgconf_process_vars_array_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct module_pkgconf_process_vars_ctx *ctx = _ctx;

	const struct str *src = get_str(wk, v);
	const char *sep;
	if (!(sep = strchr(src->s, '='))) {
		vm_error_at(wk, ctx->err_node, "invalid variable string, missing '='");
		return ir_err;
	}

	struct str key = {
		.s = src->s,
		.len = sep - src->s
	};
	struct str val = {
		.s = src->s + (key.len + 1),
		.len = src->len - (key.len + 1),
	};

	if (!module_pkgconf_declare_var(wk, ctx->err_node, ctx->escape,
		ctx->dataonly, &key, &val, ctx->dest)) {
		return ir_err;
	}

	return ir_cont;
}

static enum iteration_result
module_pkgconf_process_vars_dict_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	struct module_pkgconf_process_vars_ctx *ctx = _ctx;

	if (!module_pkgconf_declare_var(wk, ctx->err_node, ctx->escape,
		ctx->dataonly, get_str(wk, key), get_str(wk, val), ctx->dest)) {
		return ir_err;
	}

	return ir_cont;
}

static bool
module_pkgconf_process_vars(struct workspace *wk, uint32_t err_node, bool escape,
	bool dataonly, obj vars, obj dest)
{
	struct module_pkgconf_process_vars_ctx ctx = {
		.err_node = err_node,
		.escape = escape,
		.dataonly = dataonly,
		.dest = dest,
	};

	switch (get_obj_type(wk, vars)) {
	case obj_string:
		if (module_pkgconf_process_vars_array_iter(wk, &ctx, vars) == ir_err) {
			return false;
		}
		break;
	case obj_array:
		if (!obj_array_foreach(wk, vars, &ctx, module_pkgconf_process_vars_array_iter)) {
			return false;
		}
		break;
	case obj_dict:
		if (!obj_dict_foreach(wk, vars, &ctx, module_pkgconf_process_vars_dict_iter)) {
			return false;
		}
		break;
	default:
		vm_error_at(wk, err_node, "invalid type for variables, expected array or dict");
		return false;
	}

	return true;
}

static bool
module_pkgconf_prepend_libdir(struct workspace *wk, struct args_kw *install_dir_opt, obj *libs)
{
	obj libdir;
	const char *path;
	if (install_dir_opt->set) {
		SBUF(rel);
		obj pre;
		get_option_value(wk, current_project(wk), "prefix", &pre);

		const char *install_dir = get_cstr(wk, install_dir_opt->val),
			   *prefix = get_cstr(wk, pre);

		if (path_is_subpath(prefix, install_dir)) {
			path_relative_to(wk, &rel, prefix, install_dir);
			path = rel.buf;
		} else if (path_is_absolute(install_dir)) {
			vm_error_at(wk, install_dir_opt->val, "absolute install dir path not a subdir of prefix");
			return false;
		} else {
			path = install_dir;
		}

		libdir = make_strf(wk, "-L${prefix}/%s", path);
	} else {
		libdir = make_strf(wk, "-L${libdir}");
	}

	obj arr;
	make_obj(wk, &arr, obj_array);
	obj_array_push(wk, arr, libdir);
	obj_array_extend_nodup(wk, arr, *libs);
	*libs = arr;
	return true;
}

struct module_pkgconf_remove_dups_ctx {
	obj exclude;
	obj res;
};

enum iteration_result
module_pkgconf_remove_dups_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct module_pkgconf_remove_dups_ctx *ctx = _ctx;

	if (obj_array_in(wk, ctx->exclude, val)) {
		return ir_cont;
	}

	obj_array_push(wk, ctx->exclude, val);
	obj_array_push(wk, ctx->res, val);
	return ir_cont;
}

static void
module_pkgconf_remove_dups(struct workspace *wk, obj *list, obj exclude)
{
	obj arr;
	make_obj(wk, &arr, obj_array);

	struct module_pkgconf_remove_dups_ctx ctx = {
		.exclude = exclude,
		.res = arr,
	};

	obj_array_foreach(wk, *list, &ctx, module_pkgconf_remove_dups_iter);

	*list = ctx.res;
}

static bool
module_pkgconf_write(struct workspace *wk, const char *path, struct pkgconf_file *pc)
{
	FILE *f;
	if (!(f = fs_fopen(path, "wb"))) {
		return false;
	}

	if (get_obj_array(wk, pc->builtin_dir_variables)->len) {
		obj str;
		obj_array_join(wk, false, pc->builtin_dir_variables, make_str(wk, "\n"), &str);
		fputs(get_cstr(wk, str), f);
		fputc('\n', f);
	}

	if (get_obj_array(wk, pc->variables)->len) {
		fputc('\n', f);
		obj str;
		obj_array_join(wk, false, pc->variables, make_str(wk, "\n"), &str);
		fputs(get_cstr(wk, str), f);
		fputc('\n', f);
	}

	fputc('\n', f);

	fprintf(f, "Name: %s\n", get_cstr(wk, pc->name));
	fprintf(f, "Description: %s\n", get_cstr(wk, pc->description));

	if (pc->url) {
		fprintf(f, "URL: %s\n", get_cstr(wk, pc->url));
	}

	fprintf(f, "Version: %s\n", get_cstr(wk, pc->version));

	if (get_obj_array(wk, pc->reqs[pkgconf_visibility_pub])->len) {
		obj str;
		obj_array_join(wk, false, pc->reqs[pkgconf_visibility_pub], make_str(wk, ", "), &str);
		fprintf(f, "Requires: %s\n", get_cstr(wk, str));
	}

	if (get_obj_array(wk, pc->reqs[pkgconf_visibility_priv])->len) {
		obj str;
		obj_array_join(wk, false, pc->reqs[pkgconf_visibility_priv], make_str(wk, ", "), &str);
		fprintf(f, "Requires.private: %s\n", get_cstr(wk, str));
	}

	if (get_obj_array(wk, pc->libs[pkgconf_visibility_pub])->len) {
		obj str;
		obj_array_join(wk, false, pc->libs[pkgconf_visibility_pub], make_str(wk, " "), &str);
		fprintf(f, "Libs: %s\n", get_cstr(wk, str));
	}

	if (get_obj_array(wk, pc->libs[pkgconf_visibility_priv])->len) {
		obj str;
		obj_array_join(wk, false, pc->libs[pkgconf_visibility_priv], make_str(wk, " "), &str);
		fprintf(f, "Libs.private: %s\n", get_cstr(wk, str));
	}

	if (!pc->dataonly && get_obj_array(wk, pc->cflags)->len) {
		fprintf(f, "Cflags: %s\n", get_cstr(wk, join_args_pkgconf(wk, pc->cflags)));
	}

	if (!fs_fclose(f)) {
		return false;
	}

	return true;
}

static bool
func_module_pkgconfig_generate(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	uint32_t i;
	struct args_norm ao[] = { { tc_both_libs | tc_build_target }, ARG_TYPE_NULL };
	enum kwargs {
		kw_name,
		kw_description,
		kw_extra_cflags,
		kw_filebase,
		kw_install_dir,
		kw_libraries,
		kw_libraries_private,
		kw_subdirs,
		kw_requires,
		kw_requires_private,
		kw_url,
		kw_variables,
		kw_unescaped_variables,
		kw_uninstalled_variables, // TODO
		kw_unescaped_uninstalled_variables, // TODO
		kw_version,
		kw_dataonly,
		kw_conflicts,
	};
	const type_tag
		tc_library = tc_string | tc_file | tc_build_target | tc_dependency | tc_custom_target | tc_both_libs,
		tc_requires = tc_string | tc_build_target | tc_dependency | tc_both_libs;

	struct args_kw akw[] = {
		[kw_name] = { "name", obj_string },
		[kw_description] = { "description", obj_string },
		[kw_extra_cflags] = { "extra_cflags", TYPE_TAG_LISTIFY | obj_string },
		[kw_filebase] = { "filebase", obj_string },
		[kw_install_dir] = { "install_dir", obj_string },
		[kw_libraries] = { "libraries", TYPE_TAG_LISTIFY | tc_library },
		[kw_libraries_private] = { "libraries_private", TYPE_TAG_LISTIFY | tc_library  },
		[kw_subdirs] = { "subdirs", TYPE_TAG_LISTIFY | obj_string },
		[kw_requires] = { "requires", TYPE_TAG_LISTIFY | tc_requires },
		[kw_requires_private] = { "requires_private", TYPE_TAG_LISTIFY | tc_requires },
		[kw_url] = { "url", obj_string },
		[kw_variables] = { "variables", tc_string | tc_array | tc_dict },
		[kw_unescaped_variables] = { "unescaped_variables", tc_string | tc_array | tc_dict },
		[kw_uninstalled_variables] = { "uninstalled_variables", tc_string |  tc_array | tc_dict },
		[kw_unescaped_uninstalled_variables] = { "unescaped_uninstalled_variables", tc_string | tc_array | tc_dict },
		[kw_version] = { "version", obj_string },
		[kw_dataonly] = { "dataonly", obj_bool },
		[kw_conflicts] = { "conflicts", TYPE_TAG_LISTIFY | obj_string },
		0
	};
	if (!interp_args(wk, args_node, NULL, ao, akw)) {
		return false;
	}

	if (!ao[0].set && !akw[kw_name].set) {
		vm_error_at(wk, args_node, "you must either pass a library, "
			"or the name keyword");
		return false;
	}

	struct pkgconf_file pc = {
		.url = akw[kw_url].val,
		.conflicts = akw[kw_conflicts].val,
		.dataonly = akw[kw_dataonly].set
			? get_obj_bool(wk, akw[kw_dataonly].val)
			: false,
	};
	for (i = 0; i < 2; ++i) {
		make_obj(wk, &pc.libs[i], obj_array);
		make_obj(wk, &pc.reqs[i], obj_array);
	}
	make_obj(wk, &pc.cflags, obj_array);
	make_obj(wk, &pc.variables, obj_array);
	make_obj(wk, &pc.builtin_dir_variables, obj_array);
	make_obj(wk, &pc.exclude, obj_array);

	obj mainlib = 0;
	if (ao[0].set) {
		switch (get_obj_type(wk, ao[0].val)) {
		case obj_both_libs: {
			mainlib = get_obj_both_libs(wk, ao[0].val)->dynamic_lib;
			break;
		}
		case obj_build_target:
			mainlib = ao[0].val;
			break;
		default:
			assert(false && "unreachable");
		}
	}

	if (akw[kw_name].set) {
		pc.name = akw[kw_name].val;
	} else if (ao[0].set) {
		pc.name = get_obj_build_target(wk, mainlib)->name;
	}

	if (akw[kw_description].set) {
		pc.description = akw[kw_description].val;
	} else if (mainlib) {
		pc.description = make_strf(wk, "%s: %s",
			get_cstr(wk, current_project(wk)->cfg.name),
			get_cstr(wk, pc.name));
	}

	if (akw[kw_version].set) {
		pc.version = akw[kw_version].val;
	} else {
		pc.version = current_project(wk)->cfg.version;
	}

	/* cflags include dirs */
	if (akw[kw_subdirs].set) {
		if (!obj_array_foreach(wk, akw[kw_subdirs].val, &pc.cflags, add_subdirs_includes_iter)) {
			return false;
		}
	} else {
		obj_array_push(wk, pc.cflags, make_str(wk, "-I${includedir}"));
	}

	if (mainlib) {
		if (!module_pkgconf_process_libs(wk, ao[0].node, mainlib,
			&pc, pkgconf_visibility_pub, false)) {
			return false;
		}
	}

	if (akw[kw_libraries].set) {
		if (!module_pkgconf_process_libs(wk, akw[kw_libraries].node,
			akw[kw_libraries].val, &pc, pkgconf_visibility_pub, false)) {
			return false;
		}
	}

	if (akw[kw_libraries_private].set) {
		if (!module_pkgconf_process_libs(wk, akw[kw_libraries_private].node,
			akw[kw_libraries_private].val, &pc, pkgconf_visibility_priv, false)) {
			return false;
		}
	}

	module_pkgconf_remove_dups(wk, &pc.reqs[pkgconf_visibility_pub], pc.exclude);
	module_pkgconf_remove_dups(wk, &pc.libs[pkgconf_visibility_pub], pc.exclude);
	module_pkgconf_remove_dups(wk, &pc.reqs[pkgconf_visibility_priv], pc.exclude);
	module_pkgconf_remove_dups(wk, &pc.libs[pkgconf_visibility_priv], pc.exclude);

	for (i = 0; i < 2; ++i) {
		if (get_obj_array(wk, pc.libs[i])->len && pc.libs_contains_internal[i]) {
			if (!module_pkgconf_prepend_libdir(wk, &akw[kw_install_dir], &pc.libs[i])) {
				return false;
			}
		}
	}

	if (akw[kw_requires].set) {
		if (!module_pkgconf_process_reqs(wk, akw[kw_requires].node,
			akw[kw_requires].val, pc.reqs[pkgconf_visibility_pub])) {
			return false;
		}
	}

	if (akw[kw_requires_private].set) {
		if (!module_pkgconf_process_reqs(wk, akw[kw_requires_private].node,
			akw[kw_requires_private].val, pc.reqs[pkgconf_visibility_priv])) {
			return false;
		}
	}

	if (akw[kw_extra_cflags].set) {
		obj_array_extend(wk, pc.cflags, akw[kw_extra_cflags].val);
	}

	{ // variables
		for (i = 0; i < ARRAY_LEN(module_pkgconf_diropts); ++i) {
			module_pkgconf_diropts[i].refd = false;
		}

		if (!pc.dataonly) {
			module_pkgconf_diropts[module_pkgconf_diropt_prefix].refd = true;

			module_pkgconf_diropts[module_pkgconf_diropt_includedir].refd = true;
			if (get_obj_array(wk, pc.libs[0])->len || get_obj_array(wk, pc.libs[1])->len ) {
				module_pkgconf_diropts[module_pkgconf_diropt_libdir].refd = true;
			}
		}

		if (akw[kw_variables].set) {
			if (!module_pkgconf_process_vars(wk, akw[kw_variables].node, true,
				pc.dataonly, akw[kw_variables].val, pc.variables)) {
				return false;
			}
		}

		if (akw[kw_unescaped_variables].set) {
			if (!module_pkgconf_process_vars(wk, akw[kw_unescaped_variables].node, false,
				pc.dataonly, akw[kw_unescaped_variables].val, pc.variables)) {
				return false;
			}
		}

		for (i = 0; i < ARRAY_LEN(module_pkgconf_diropts); ++i) {
			if (module_pkgconf_diropts[i].refd && !module_pkgconf_diropts[i].added) {
				module_pkgconf_declare_builtin_dir_var(wk,
					module_pkgconf_diropts[i].optname,
					pc.builtin_dir_variables);
			}
		}
	}

	obj filebase = pc.name;
	if (akw[kw_filebase].set) {
		filebase = akw[kw_filebase].val;
	}

	SBUF(path);
	path_join(wk, &path, wk->muon_private, get_cstr(wk, filebase));
	sbuf_pushs(wk, &path, ".pc");

	if (!module_pkgconf_write(wk, path.buf, &pc)) {
		return false;
	}

	if (mainlib) {
		get_obj_build_target(wk, mainlib)->generated_pc = filebase;
	}

	make_obj(wk, res, obj_file);
	*get_obj_file(wk, *res) = sbuf_into_str(wk, &path);

	{
		SBUF(install_dir_buf);
		const char *install_dir;
		if (akw[kw_install_dir].set) {
			install_dir = get_cstr(wk, akw[kw_install_dir].val);
		} else {
			obj install_base;
			if (pc.dataonly) {
				get_option_value(wk, current_project(wk), "datadir", &install_base);
			} else {
				get_option_value(wk, current_project(wk), "libdir", &install_base);
			}

			path_join(wk, &install_dir_buf, get_cstr(wk, install_base), "pkgconfig");
			install_dir = install_dir_buf.buf;
		}

		SBUF(dest);
		path_join(wk, &dest, install_dir, get_cstr(wk, filebase));
		sbuf_pushs(wk, &dest, ".pc");

		push_install_target(wk, *get_obj_file(wk, *res), sbuf_into_str(wk, &dest), 0);
	}

	return true;
}

const struct func_impl impl_tbl_module_pkgconfig[] = {
	{ "generate", func_module_pkgconfig_generate, tc_file },
	{ NULL, NULL },
};

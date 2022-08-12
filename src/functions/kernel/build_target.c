#include "posix.h"

#include <assert.h>
#include <string.h>

#include "buf_size.h"
#include "coerce.h"
#include "error.h"
#include "functions/build_target.h"
#include "functions/file.h"
#include "functions/generator.h"
#include "functions/kernel/build_target.h"
#include "functions/kernel/dependency.h"
#include "install.h"
#include "lang/interpreter.h"
#include "log.h"
#include "options.h"
#include "platform/filesystem.h"
#include "platform/path.h"

enum build_target_kwargs {
	bt_kw_sources,
	bt_kw_include_directories,
	bt_kw_implicit_include_directories,
	bt_kw_dependencies,
	bt_kw_install,
	bt_kw_install_dir,
	bt_kw_install_mode,
	bt_kw_install_tag,
	bt_kw_link_with,
	bt_kw_link_whole,
	bt_kw_version,
	bt_kw_build_by_default,
	bt_kw_extra_files, // TODO
	bt_kw_target_type,
	bt_kw_name_prefix,
	bt_kw_name_suffix,
	bt_kw_soversion,
	bt_kw_link_depends,
	bt_kw_objects,
	bt_kw_pic,
	bt_kw_pie,
	bt_kw_build_rpath,
	bt_kw_install_rpath,
	bt_kw_export_dynamic,
	bt_kw_vs_module_defs, // TODO
	bt_kw_gnu_symbol_visibility,
	bt_kw_native, // TODO
	bt_kw_darwin_versions, // TODO
	bt_kw_implib, // TODO
	bt_kw_gui_app, // TODO
	bt_kw_link_language, // TODO
	bt_kw_override_options,

	/* lang args */
	bt_kw_c_pch, // TODO
	bt_kw_c_args,
	bt_kw_cpp_args,
	bt_kw_objc_args,
	bt_kw_link_args,

	bt_kwargs_count,
};

static enum iteration_result
determine_linker_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct obj_build_target *tgt = _ctx;

	enum compiler_language fl;

	if (!filename_to_compiler_language(get_file_path(wk, val), &fl)) {
		/* LOG_E("unable to determine language for '%s'", get_cstr(wk, src->dat.file)); */
		return ir_cont;
	}

	tgt->dep_internal.link_language = coalesce_link_languages(tgt->dep_internal.link_language, fl);

	return ir_cont;
}

static bool
build_tgt_determine_linker(struct workspace *wk, uint32_t err_node, struct obj_build_target *tgt)
{

	if (!obj_array_foreach(wk, tgt->src, tgt, determine_linker_iter)) {
		return ir_err;
	}

	if (!tgt->dep_internal.link_language) {
		enum compiler_language clink_langs[] = {
			compiler_language_c,
			compiler_language_cpp,
		};

		obj comp;
		uint32_t i;
		for (i = 0; i < ARRAY_LEN(clink_langs); ++i) {
			if (obj_dict_geti(wk, current_project(wk)->compilers, clink_langs[i], &comp)) {
				tgt->dep_internal.link_language = clink_langs[i];
				break;
			}
		}
	}

	if (!tgt->dep_internal.link_language) {
		interp_error(wk, err_node, "unable to determine linker for target");
		return false;
	}

	return true;
}

struct process_build_tgt_sources_ctx {
	uint32_t err_node;
	obj tgt_id;
	obj res;
	bool implicit_include_directories;
};

static bool
process_source_include(struct workspace *wk, struct process_build_tgt_sources_ctx *ctx, obj val)
{
	const char *src = get_file_path(wk, val);

	if (!path_is_subpath(wk->build_root, src)) {
		return true;
	}

	char dir[PATH_MAX], path[PATH_MAX];
	if (!path_relative_to(path, PATH_MAX, wk->build_root, src)) {
		return false;
	}

	struct obj_build_target *tgt = get_obj_build_target(wk, ctx->tgt_id);
	obj_array_push(wk, tgt->dep_internal.order_deps, make_str(wk, path));

	if (!ctx->implicit_include_directories) {
		return true;
	}

	if (!path_dirname(dir, PATH_MAX, src)) {
		return false;
	}

	obj inc;
	make_obj(wk, &inc, obj_include_directory);
	struct obj_include_directory *d = get_obj_include_directory(wk, inc);
	d->path = make_str(wk, dir);
	obj_array_push(wk, tgt->dep_internal.include_directories, inc);

	return true;
}

static enum iteration_result
build_tgt_push_source_files_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct process_build_tgt_sources_ctx *ctx = _ctx;

	if (file_is_linkable(wk, val)) {
		struct obj_build_target *tgt = get_obj_build_target(wk, ctx->tgt_id);
		obj_array_push(wk, tgt->dep_internal.link_with, val);
		return ir_cont;
	}

	enum compiler_language lang;
	if (!filename_to_compiler_language(get_file_path(wk, val), &lang)
	    || languages[lang].is_header) {
		// process every file that is either a header, or isn't
		// recognized, as a header
		if (!process_source_include(wk, ctx, val)) {
			return ir_err;
		}
		return ir_cont;
	} else if (languages[lang].is_linkable) {
		struct obj_build_target *tgt = get_obj_build_target(wk, ctx->tgt_id);
		obj_array_push(wk, tgt->objects, val);
		return ir_cont;
	}

	obj_array_push(wk, ctx->res, val);
	return ir_cont;
}

static enum iteration_result
process_build_tgt_sources_iter(struct workspace *wk, void *_ctx, obj val)
{
	obj res;
	struct process_build_tgt_sources_ctx *ctx = _ctx;

	switch (get_obj_type(wk, val)) {
	case obj_generated_list:
		if (!generated_list_process_for_target(wk, ctx->err_node, val, ctx->tgt_id, true, &res)) {
			return ir_err;
		}
		break;
	default: {
		if (!coerce_files(wk, ctx->err_node, val, &res)) {
			return ir_err;
		}
		break;
	}
	}

	obj_array_foreach(wk, res, ctx, build_tgt_push_source_files_iter);
	return ir_cont;
}

static bool
type_from_kw(struct workspace *wk, uint32_t node, obj t, enum tgt_type *res)
{
	const char *tgt_type = get_cstr(wk, t);
	struct { char *name; enum tgt_type type; } tgt_tbl[] = {
		{ "executable", tgt_executable, },
		{ "shared_library", tgt_dynamic_library, },
		{ "shared_module", tgt_shared_module, },
		{ "static_library", tgt_static_library, },
		{ "both_libraries", tgt_dynamic_library | tgt_static_library, },
		{ "library", get_option_default_library(wk), },
		{ 0 },
	};
	uint32_t i;

	for (i = 0; tgt_tbl[i].name; ++i) {
		if (strcmp(tgt_type, tgt_tbl[i].name) == 0) {
			*res = tgt_tbl[i].type;
			break;
		}
	}

	if (!tgt_tbl[i].name) {
		interp_error(wk, node, "unsupported target type '%s'", tgt_type);
		return false;
	}

	return true;
}

static void
setup_soname(struct workspace *wk, struct obj_build_target *tgt, const char *plain_name, obj sover, obj ver)
{
	char soversion[BUF_SIZE_1k] = { "." };
	bool have_soversion = false;

	if (sover) {
		have_soversion = true;
		strncpy(&soversion[1], get_cstr(wk, sover), BUF_SIZE_1k - 2);
	} else if (ver) {
		have_soversion = true;
		strncpy(&soversion[1], get_cstr(wk, ver), BUF_SIZE_1k - 2);
		char *p;
		if ((p = strchr(&soversion[1], '.'))) {
			*p = 0;
		}
	}

	tgt->soname = make_strf(wk, "%s%s", plain_name, have_soversion ? soversion : "");
}

static bool
setup_shared_object_symlinks(struct workspace *wk, struct obj_build_target *tgt,
	const char *plain_name, const char **plain_name_install,
	const char **soname_install)
{
	static char soname_symlink[PATH_MAX], plain_name_symlink[PATH_MAX];

	if (!fs_mkdir_p(get_cstr(wk, tgt->build_dir))) {
		return false;
	}

	if (!str_eql(get_str(wk, tgt->build_name), get_str(wk, tgt->soname))) {
		if (!path_join(soname_symlink, PATH_MAX, get_cstr(wk, tgt->build_dir),
			get_cstr(wk, tgt->soname))) {
			return false;
		}

		if (!fs_make_symlink(get_cstr(wk, tgt->build_name), soname_symlink, true)) {
			return false;
		}

		*soname_install = soname_symlink;
	}

	if (!str_eql(get_str(wk, tgt->soname), &WKSTR(plain_name))) {
		if (!path_join(plain_name_symlink, PATH_MAX, get_cstr(wk, tgt->build_dir), plain_name)) {
			return false;
		}

		if (!fs_make_symlink(get_cstr(wk, tgt->soname), plain_name_symlink, true)) {
			return false;
		}

		*plain_name_install = plain_name_symlink;
	}

	return true;
}

static bool
determine_target_build_name(struct workspace *wk, struct obj_build_target *tgt, obj sover, obj ver,
	obj name_pre, obj name_suff, char plain_name[BUF_SIZE_2k])
{
	const char *pref, *suff, *ver_suff = NULL;
	uint32_t i;

	switch (tgt->type) {
	case tgt_executable:
		pref = "";
		suff = NULL;
		break;
	case tgt_static_library:
		pref = "lib";
		suff = "a";
		break;
	case tgt_shared_module:
	case tgt_dynamic_library:
		pref = "lib";
		suff = "so";
		if (ver) {
			ver_suff = get_cstr(wk, ver);
		} else if (sover) {
			ver_suff = get_cstr(wk, sover);
		}
		break;
	default:
		assert(false && "unreachable");
		return false;
	}

	if (name_pre) {
		pref = get_cstr(wk, name_pre);
	}

	if (name_suff) {
		suff = get_cstr(wk, name_suff);
	}

	i = snprintf(plain_name, BUF_SIZE_2k, "%s%s", pref, get_cstr(wk, tgt->name));
	if (suff) {
		snprintf(&plain_name[i], BUF_SIZE_2k, ".%s", suff);
	}

	tgt->build_name = make_strf(wk, "%s%s%s", plain_name, ver_suff ? "." : "", ver_suff ? ver_suff : "");
	return true;
}

static bool
create_target(struct workspace *wk, struct args_norm *an, struct args_kw *akw,
	enum tgt_type type, bool ignore_sources, obj *res)
{
	char plain_name[BUF_SIZE_2k + 1] = { 0 };
	make_obj(wk, res, obj_build_target);
	struct obj_build_target *tgt = get_obj_build_target(wk, *res);
	tgt->type = type;
	tgt->name = an[0].val;
	tgt->cwd = current_project(wk)->cwd;
	tgt->build_dir = current_project(wk)->build_dir;
	make_obj(wk, &tgt->args, obj_dict);
	make_obj(wk, &tgt->src, obj_array);
	build_dep_init(wk, &tgt->dep_internal);

	{ // linker args (process before dependencies so link_with libs come first on link line
		if (akw[bt_kw_link_with].set) {
			if (!dep_process_link_with(wk, akw[bt_kw_link_with].node,
				akw[bt_kw_link_with].val, &tgt->dep_internal)) {
				return false;
			}
		}

		if (akw[bt_kw_link_whole].set) {
			if (!dep_process_link_whole(wk, akw[bt_kw_link_whole].node,
				akw[bt_kw_link_whole].val, &tgt->dep_internal)) {
				return false;
			}
		}
	}

	if (akw[bt_kw_dependencies].set) {
		dep_process_deps(wk, akw[bt_kw_dependencies].val, &tgt->dep_internal);
	}

	if (akw[bt_kw_override_options].set) { // override options
		if (!parse_and_set_override_options(wk,
			akw[bt_kw_override_options].node,
			akw[bt_kw_override_options].val,
			&tgt->override_options)) {
			return false;
		}
	}

	{ // build target flags
		{ // pic
			bool pic = false;
			if (akw[bt_kw_pic].set) {
				pic = get_obj_bool(wk, akw[bt_kw_pic].val);

				if (!pic && tgt->type & (tgt_dynamic_library | tgt_shared_module)) {
					interp_error(wk, akw[bt_kw_pic].node, "shared libraries must be compiled as pic");
					return false;
				}
			} else {
				bool staticpic = get_option_bool(wk, tgt->override_options, "b_staticpic", true);

				if (tgt->type & tgt_static_library) {
					pic = staticpic;
				} else if (tgt->type & (tgt_dynamic_library | tgt_shared_module)) {
					pic = true;
				}
			}

			if (pic) {
				tgt->flags |= build_tgt_flag_pic;
			}
		}

		{ // pie
			bool pie = false;

			if (akw[bt_kw_pie].set) {
				pie = get_obj_bool(wk, akw[bt_kw_pie].set);

				if (pie && (tgt->type & tgt_executable) != tgt_executable) {
					interp_error(wk, akw[bt_kw_pie].node, "pie cannot be set for non-executables");
					return false;
				}
			} else if ((tgt->type & tgt_executable) == tgt_executable) {
				pie = get_option_bool(wk, tgt->override_options, "b_pie", false);
			}

			if (pie) {
				tgt->flags |= build_tgt_flag_pie;
			}
		}

		if (akw[bt_kw_export_dynamic].set && get_obj_bool(wk, akw[bt_kw_export_dynamic].val)) {
			tgt->flags |= build_tgt_flag_export_dynamic;
		}

		if (!akw[bt_kw_build_by_default].set || get_obj_bool(wk, akw[bt_kw_build_by_default].val)) {
			tgt->flags |= build_tgt_flag_build_by_default;
		}

		struct args_kw *vis = &akw[bt_kw_gnu_symbol_visibility];
		if (vis->set && get_str(wk, vis->val)->len) {
			const struct str *str = get_str(wk, vis->val);
			if (str_eql(str, &WKSTR("default"))) {
				tgt->visibility = compiler_visibility_default;
			} else if (str_eql(str, &WKSTR("hidden"))) {
				tgt->visibility = compiler_visibility_hidden;
			} else if (str_eql(str, &WKSTR("internal"))) {
				tgt->visibility = compiler_visibility_internal;
			} else if (str_eql(str, &WKSTR("protected"))) {
				tgt->visibility = compiler_visibility_protected;
			} else if (str_eql(str, &WKSTR("inlineshidden"))) {
				tgt->visibility = compiler_visibility_inlineshidden;
			} else {
				interp_error(wk, vis->node, "unknown visibility '%s'", get_cstr(wk, vis->val));
				return false;
			}

			tgt->flags |= build_tgt_flag_visibility;
		}
	}

	obj sover = 0;
	if (akw[bt_kw_soversion].set) {
		if (!coerce_num_to_string(wk, akw[bt_kw_soversion].node, akw[bt_kw_soversion].val, &sover)) {
			return false;
		}
	}

	if (!determine_target_build_name(wk, tgt, sover, akw[bt_kw_version].val,
		akw[bt_kw_name_prefix].val, akw[bt_kw_name_suffix].val, plain_name)) {
		return false;
	}

	{ /* tgt_build_path */
		char path[PATH_MAX] = { 0 };
		if (!path_join(path, PATH_MAX, get_cstr(wk, tgt->build_dir), get_cstr(wk, tgt->build_name))) {
			return false;
		}

		tgt->build_path = make_str(wk, path);

		if (!path_add_suffix(path, PATH_MAX, ".p")) {
			return false;
		}

		tgt->private_path = make_str(wk, path);
	}

	bool implicit_include_directories =
		akw[bt_kw_implicit_include_directories].set
				? get_obj_bool(wk, akw[bt_kw_implicit_include_directories].val)
				: true;

	{ // sources
		if (akw[bt_kw_objects].set) {
			if (!coerce_files(wk, akw[bt_kw_objects].node, akw[bt_kw_objects].val, &tgt->objects)) {
				return false;
			}
		} else {
			make_obj(wk, &tgt->objects, obj_array);
		}

		if (!ignore_sources) {
			obj sources = an[1].val;

			if (akw[bt_kw_sources].set) {
				obj_array_extend(wk, sources, akw[bt_kw_sources].val);
			}

			obj_array_extend(wk, sources, tgt->dep_internal.sources);

			struct process_build_tgt_sources_ctx ctx = {
				.err_node = an[1].node,
				.res = tgt->src,
				.tgt_id = *res,
				.implicit_include_directories = implicit_include_directories,
			};

			if (!obj_array_foreach_flat(wk, sources, &ctx, process_build_tgt_sources_iter)) {
				return false;
			}

			obj deduped;
			obj_array_dedup(wk, tgt->src, &deduped);
			tgt->src = deduped;
		}

		if (!get_obj_array(wk, tgt->src)->len &&
		    !get_obj_array(wk, tgt->objects)->len &&
		    !akw[bt_kw_link_whole].set
		    && tgt->type != tgt_static_library) {
			uint32_t node = akw[bt_kw_sources].set? akw[bt_kw_sources].node : an[1].node;

			interp_error(wk, node, "target declared with no linkable sources");
			return false;
		}
	}

	{ // include directories
		obj inc_dirs;
		make_obj(wk, &inc_dirs, obj_array);
		uint32_t node = an[0].node; // TODO: not a very informative error node

		if (implicit_include_directories) {
			obj_array_push(wk, inc_dirs, current_project(wk)->cwd);
		}

		if (akw[bt_kw_include_directories].set) {
			node = akw[bt_kw_include_directories].node;

			obj_array_extend(wk, inc_dirs, akw[bt_kw_include_directories].val);
		}

		obj coerced;
		if (!coerce_include_dirs(wk, node, inc_dirs, false, &coerced)) {
			return false;
		}

		obj_array_extend_nodup(wk, coerced, tgt->dep_internal.include_directories);
		tgt->dep_internal.include_directories = coerced;
	}

	{ // compiler args
		static struct {
			enum build_target_kwargs kw;
			enum compiler_language l;
		} lang_args[] = {
			{ bt_kw_c_args, compiler_language_c },
			{ bt_kw_cpp_args, compiler_language_cpp },
			/* { bt_kw_objc_args, compiler_language_objc }, */
		};

		uint32_t i;
		for (i = 0; i < ARRAY_LEN(lang_args); ++i) {
			if (akw[lang_args[i].kw].set) {
				obj_dict_seti(wk, tgt->args, lang_args[i].l, akw[lang_args[i].kw].val);
			}
		}
	}

	const char *soname_install = NULL, *plain_name_install = NULL;

	// soname handling
	if (type & (tgt_dynamic_library | tgt_shared_module)) {
		setup_soname(wk, tgt, plain_name, sover, akw[bt_kw_version].val);

		if (type == tgt_dynamic_library) {
			if (!setup_shared_object_symlinks(wk, tgt, plain_name,
				&plain_name_install, &soname_install)) {
				return false;
			}
		}
	}

	// link depends
	if (akw[bt_kw_link_depends].set) {
		obj depends;
		if (!coerce_files(wk, akw[bt_kw_link_depends].node, akw[bt_kw_link_depends].val, &depends)) {
			return false;
		}

		tgt->link_depends = depends;
	}

	if (akw[bt_kw_install].set && get_obj_bool(wk, akw[bt_kw_install].val)) {
		tgt->flags |= build_tgt_flag_installed;

		obj install_dir = 0;
		if (akw[bt_kw_install_dir].set) {
			install_dir = akw[bt_kw_install_dir].val;
		} else {
			switch (type) {
			case tgt_executable:
				get_option_value(wk, current_project(wk), "bindir", &install_dir);
				break;
			case tgt_dynamic_library:
			case tgt_static_library:
			case tgt_shared_module:
				get_option_value(wk, current_project(wk), "libdir", &install_dir);
				break;
			default:
				assert(false && "unreachable");
				break;
			}
		}

		char install_src[PATH_MAX];
		if (!path_join(install_src, PATH_MAX, get_cstr(wk, tgt->build_dir), get_cstr(wk, tgt->build_name))) {
			return NULL;
		}

		char install_dest[PATH_MAX];
		if (!path_join(install_dest, PATH_MAX, get_cstr(wk, install_dir), get_cstr(wk, tgt->build_name))) {
			return NULL;
		}

		struct obj_install_target *install_tgt;
		if (!(install_tgt = push_install_target(wk, make_str(wk, install_src),
			make_str(wk, install_dest), akw[bt_kw_install_mode].val))) {
			return false;
		}

		install_tgt->build_target = true;

		if (soname_install) {
			push_install_target_install_dir(wk, make_str(wk, soname_install),
				install_dir, akw[bt_kw_install_mode].val);
		}

		if (plain_name_install) {
			push_install_target_install_dir(wk, make_str(wk, plain_name_install),
				install_dir, akw[bt_kw_install_mode].val);
		}
	}

	if (!build_tgt_determine_linker(wk, an[0].node, tgt)) {
		return false;
	}

	{ // rpaths
		if (akw[bt_kw_build_rpath].set) {
			obj_array_push(wk, tgt->dep_internal.rpath, akw[bt_kw_build_rpath].val);
		}

		if (akw[bt_kw_install_rpath].set) {
			obj_array_push(wk, tgt->dep_internal.rpath, akw[bt_kw_install_rpath].val);
		}
	}

	tgt->dep = (struct build_dep) {
		.link_language = tgt->dep_internal.link_language,
		.include_directories = tgt->dep_internal.include_directories,
		.order_deps = tgt->dep_internal.order_deps,
		.rpath = tgt->dep_internal.rpath,
		.raw = tgt->dep_internal.raw,
	};

	if (tgt->type == tgt_static_library) {
		tgt->dep.link_whole = tgt->dep_internal.link_whole;
		tgt->dep.link_with = tgt->dep_internal.link_with;
		tgt->dep.link_with_not_found = tgt->dep_internal.link_with_not_found;
		obj_array_dup(wk, tgt->dep_internal.link_args, &tgt->dep.link_args);
	}

	if (akw[bt_kw_link_args].set) {
		obj_array_extend(wk, tgt->dep_internal.link_args, akw[bt_kw_link_args].val);
	}

	LOG_I("added target %s", get_cstr(wk, tgt->build_name));
	obj_array_push(wk, current_project(wk)->targets, *res);
	return true;
}

static bool
typecheck_string_or_empty_array(struct workspace *wk, struct args_kw *kw)
{
	if (!kw->set) {
		return true;
	}

	enum obj_type t = get_obj_type(wk, kw->val);
	if (t == obj_string) {
		return true;
	} else if (t == obj_array && get_obj_array(wk, kw->val)->len == 0) {
		kw->set = false;
		kw->val = 0;
		return true;
	} else {
		interp_error(wk, kw->node, "expected string or [], got %s", obj_type_to_s(t));
		return false;
	}
}

static bool
tgt_common(struct workspace *wk, uint32_t args_node, obj *res, enum tgt_type type, enum tgt_type argtype, bool tgt_type_from_kw)
{
	struct args_norm an[] = { { obj_string }, { ARG_TYPE_GLOB | tc_coercible_files | tc_generated_list }, ARG_TYPE_NULL };

	struct args_kw akw[] = {
		[bt_kw_sources] = { "sources", ARG_TYPE_ARRAY_OF | tc_coercible_files | tc_generated_list },
		[bt_kw_include_directories] = { "include_directories", ARG_TYPE_ARRAY_OF | tc_coercible_inc },
		[bt_kw_implicit_include_directories] = { "implicit_include_directories", obj_bool },
		[bt_kw_dependencies] = { "dependencies", ARG_TYPE_ARRAY_OF | tc_dependency },
		[bt_kw_install] = { "install", obj_bool },
		[bt_kw_install_dir] = { "install_dir", obj_string },
		[bt_kw_install_mode] = { "install_mode", tc_install_mode_kw },
		[bt_kw_install_tag] = { "install_tag", tc_string }, // TODO
		[bt_kw_link_with] = { "link_with", tc_link_with_kw },
		[bt_kw_link_whole] = { "link_whole", tc_link_with_kw },
		[bt_kw_version] = { "version", obj_string },
		[bt_kw_build_by_default] = { "build_by_default", obj_bool },
		[bt_kw_extra_files] = { "extra_files", ARG_TYPE_ARRAY_OF | tc_coercible_files }, // ignored
		[bt_kw_target_type] = { "target_type", obj_string },
		[bt_kw_name_prefix] = { "name_prefix", tc_string | tc_array },
		[bt_kw_name_suffix] = { "name_suffix", tc_string | tc_array },
		[bt_kw_soversion] = { "soversion", tc_number | tc_string },
		[bt_kw_link_depends] = { "link_depends", ARG_TYPE_ARRAY_OF | tc_string | tc_file | tc_custom_target },
		[bt_kw_objects] = { "objects", ARG_TYPE_ARRAY_OF | tc_file | tc_string },
		[bt_kw_pic] = { "pic", obj_bool },
		[bt_kw_pie] = { "pie", obj_bool },
		[bt_kw_build_rpath] = { "build_rpath", obj_string },
		[bt_kw_install_rpath] = { "install_rpath", obj_string },
		[bt_kw_export_dynamic] = { "export_dynamic", obj_bool },
		[bt_kw_vs_module_defs] = { "vs_module_defs", tc_string | tc_file | tc_custom_target },
		[bt_kw_gnu_symbol_visibility] = { "gnu_symbol_visibility", obj_string },
		[bt_kw_native] = { "native", obj_bool },
		[bt_kw_darwin_versions] = { "darwin_versions", ARG_TYPE_ARRAY_OF | tc_string | tc_number },
		[bt_kw_implib] = { "implib", obj_bool },
		[bt_kw_gui_app] = { "gui_app", obj_bool },
		[bt_kw_link_language] = { "link_language", obj_string },
		[bt_kw_override_options] = { "override_options", ARG_TYPE_ARRAY_OF | obj_string },
		/* lang args */
		[bt_kw_c_pch] = { "c_pch", tc_string | tc_file, },
		[bt_kw_c_args] = { "c_args", ARG_TYPE_ARRAY_OF | obj_string },
		[bt_kw_cpp_args] = { "cpp_args", ARG_TYPE_ARRAY_OF | obj_string },
		[bt_kw_objc_args] = { "objc_args", ARG_TYPE_ARRAY_OF | obj_string },
		[bt_kw_link_args] = { "link_args", ARG_TYPE_ARRAY_OF | obj_string },
		0
	};

	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	if (tgt_type_from_kw) {
		if (!akw[bt_kw_target_type].set) {
			interp_error(wk, args_node, "missing required kwarg: %s", akw[bt_kw_target_type].key);
			return false;
		}

		if (!type_from_kw(wk, akw[bt_kw_target_type].node, akw[bt_kw_target_type].val, &type)) {
			return false;
		}
	} else {
		if (akw[bt_kw_target_type].set) {
			interp_error(wk, akw[bt_kw_target_type].node, "invalid kwarg");
			return false;
		}
	}

	static const enum tgt_type keyword_validity[bt_kwargs_count] = {
		[bt_kw_version] = tgt_dynamic_library,
		[bt_kw_soversion] = tgt_dynamic_library,
		[bt_kw_vs_module_defs] = tgt_dynamic_library | tgt_static_library | tgt_shared_module,
	};

	uint32_t i;
	for (i = 0; i < bt_kwargs_count; ++i) {
		if (keyword_validity[i]
		    && akw[i].set
		    && !(keyword_validity[i] & argtype)) {
			interp_error(wk, akw[i].node, "invalid kwarg");
			return false;
		}
	}

	if (!typecheck_string_or_empty_array(wk, &akw[bt_kw_name_suffix])) {
		return false;
	} else if (!typecheck_string_or_empty_array(wk, &akw[bt_kw_name_prefix])) {
		return false;
	}

	if (type == (tgt_static_library | tgt_dynamic_library)
	    && !akw[bt_kw_pic].set) {
		make_obj(wk, &akw[bt_kw_pic].val, obj_bool);
		set_obj_bool(wk, akw[bt_kw_pic].val, true);
		akw[bt_kw_pic].set = true;
	}

	bool multi_target = false;
	obj tgt = 0;

	for (i = 0; i <= tgt_type_count; ++i) {
		enum tgt_type t = 1 << i;

		if (!(type & t)) {
			continue;
		}

		if (tgt && !multi_target) {
			multi_target = true;
			make_obj(wk, res, obj_array);
			obj_array_push(wk, *res, tgt);

			// If this target is a multi-target (both_libraries),
			// set the objects argument with objects from the
			// previous target

			obj objects;
			if (!build_target_extract_all_objects(wk, an[0].node, tgt, &objects, true)) {
				return false;
			}

			if (akw[bt_kw_objects].set) {
				obj_array_extend(wk, objects, akw[bt_kw_objects].val);
			} else {
				akw[bt_kw_objects].set = true;
				akw[bt_kw_objects].val = objects;
				akw[bt_kw_objects].node = an[0].node;
			}
		}

		if (!create_target(wk, an, akw, t, multi_target, &tgt)) {
			return false;
		}

		if (multi_target) {
			obj_array_push(wk, *res, tgt);
		} else {
			*res = tgt;
		}
	}

	if (multi_target) {
		obj val;
		make_obj(wk, &val, obj_both_libs);
		struct obj_both_libs *both = get_obj_both_libs(wk, val);
		obj_array_index(wk, *res, 0, &both->static_lib);
		obj_array_index(wk, *res, 1, &both->dynamic_lib);
		*res = val;

		assert(get_obj_build_target(wk, both->static_lib)->type == tgt_static_library);
		assert(get_obj_build_target(wk, both->dynamic_lib)->type == tgt_dynamic_library);
	}

	return true;
}

bool
func_executable(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	return tgt_common(wk, args_node, res, tgt_executable, tgt_executable, false);
}

bool
func_static_library(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	return tgt_common(wk, args_node, res, tgt_static_library, tgt_static_library, false);
}

bool
func_shared_library(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	return tgt_common(wk, args_node, res, tgt_dynamic_library, tgt_dynamic_library, false);
}

bool
func_both_libraries(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	return tgt_common(wk, args_node, res, tgt_static_library | tgt_dynamic_library,
		tgt_static_library | tgt_dynamic_library, false);
}

bool
func_library(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	return tgt_common(wk, args_node, res, get_option_default_library(wk),
		tgt_static_library | tgt_dynamic_library, false);
}

bool
func_shared_module(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	return tgt_common(wk, args_node, res, tgt_shared_module,
		tgt_shared_module, false);
}

bool
func_build_target(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	return tgt_common(wk, args_node, res, 0,
		tgt_executable | tgt_static_library | tgt_dynamic_library, true);
}

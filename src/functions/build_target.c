/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: illiliti <illiliti@dimension.sh>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "coerce.h"
#include "error.h"
#include "functions/build_target.h"
#include "functions/common.h"
#include "functions/generator.h"
#include "lang/typecheck.h"
#include "log.h"
#include "platform/path.h"

bool
tgt_src_to_object_path(struct workspace *wk,
	const struct obj_build_target *tgt,
	obj src_file,
	bool relative,
	struct sbuf *res)
{
	obj src = *get_obj_file(wk, src_file);

	SBUF(private_path_rel);
	SBUF(rel);
	const char *base, *private_path = get_cstr(wk, tgt->private_path);

	if (relative) {
		path_relative_to(wk, &private_path_rel, wk->build_root, private_path);
		private_path = private_path_rel.buf;
	}

	if (path_is_subpath(get_cstr(wk, tgt->private_path), get_cstr(wk, src))) {
		// file is a source from a generated list
		base = get_cstr(wk, tgt->private_path);
	} else if (path_is_subpath(get_cstr(wk, tgt->build_dir), get_cstr(wk, src))) {
		// file is a generated source from custom_target / configure_file
		base = get_cstr(wk, tgt->build_dir);
	} else if (path_is_subpath(get_cstr(wk, tgt->cwd), get_cstr(wk, src))) {
		// file is in target cwd
		base = get_cstr(wk, tgt->cwd);
	} else if (path_is_subpath(wk->source_root, get_cstr(wk, src))) {
		// file is in source root
		base = wk->source_root;
	} else {
		// outside the source root
		base = NULL;
	}

	if (base) {
		path_relative_to(wk, &rel, base, get_cstr(wk, src));
	} else {
		path_copy(wk, &rel, get_cstr(wk, src));

		uint32_t i;
		for (i = 0; i < rel.len; ++i) {
			if (rel.buf[i] == PATH_SEP) {
				rel.buf[i] = '_';
			}
		}
	}

	path_join(wk, res, private_path, rel.buf);

	const char *ext = ".o";

	{
		enum compiler_language lang;
		obj comp_id;
		if (filename_to_compiler_language(res->buf, &lang)
			&& obj_dict_geti(wk, current_project(wk)->compilers, lang, &comp_id)) {
			ext = compilers[get_obj_compiler(wk, comp_id)->type].object_ext;
		}
	}

	sbuf_pushs(wk, res, ext);
	return true;
}

static bool
func_build_target_name(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = get_obj_build_target(wk, self)->name;
	return true;
}

static bool
func_build_target_full_path(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	struct obj_build_target *tgt = get_obj_build_target(wk, self);

	*res = tgt->build_path;
	return true;
}

struct build_target_extract_objects_ctx {
	uint32_t err_node;
	struct obj_build_target *tgt;
	obj tgt_id;
	obj *res;
};

static enum iteration_result
build_target_extract_objects_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct build_target_extract_objects_ctx *ctx = _ctx;
	obj file;
	enum obj_type t = get_obj_type(wk, val);

	if (!typecheck(wk, ctx->err_node, val, tc_file | tc_string | tc_custom_target | tc_generated_list)) {
		return false;
	}

	switch (t) {
	case obj_string: {
		if (!coerce_string_to_file(wk, get_cstr(wk, ctx->tgt->cwd), val, &file)) {
			return ir_err;
		}
		break;
	}
	case obj_file: file = val; break;
	case obj_custom_target: {
		struct obj_custom_target *tgt = get_obj_custom_target(wk, val);
		if (!obj_array_flatten_one(wk, tgt->output, &file)) {
			vm_error_at(wk, ctx->err_node, "cannot coerce custom_target with multiple outputs to file");
			return ir_err;
		}
		break;
	}
	case obj_generated_list: {
		obj res;
		if (!generated_list_process_for_target(wk, ctx->err_node, val, ctx->tgt_id, false, &res)) {
			return ir_err;
		}

		if (!obj_array_foreach(wk, res, ctx, build_target_extract_objects_iter)) {
			return ir_err;
		}
		return ir_cont;
	}
	default: UNREACHABLE_RETURN;
	}

	enum compiler_language l;
	if (!filename_to_compiler_language(get_file_path(wk, file), &l)) {
		return ir_cont;
	}

	switch (l) {
	case compiler_language_cpp_hdr:
	case compiler_language_c_hdr:
	case compiler_language_c_obj:
		// skip non-compileable sources
		return ir_cont;
	case compiler_language_assembly:
	case compiler_language_nasm:
	case compiler_language_c:
	case compiler_language_cpp:
	case compiler_language_llvm_ir:
	case compiler_language_objc: break;
	case compiler_language_null:
	case compiler_language_count: UNREACHABLE;
	}

	if (!obj_array_in(wk, ctx->tgt->src, file)) {
		vm_error_at(wk, ctx->err_node, "%o is not in target sources (%o)", file, ctx->tgt->src);
		return ir_err;
	}

	SBUF(dest_path);
	if (!tgt_src_to_object_path(wk, ctx->tgt, file, false, &dest_path)) {
		return ir_err;
	}

	obj new_file;
	make_obj(wk, &new_file, obj_file);
	*get_obj_file(wk, new_file) = sbuf_into_str(wk, &dest_path);
	obj_array_push(wk, *ctx->res, new_file);
	return ir_cont;
}

static bool
build_target_extract_objects(struct workspace *wk, obj self, uint32_t err_node, obj *res, obj arr)
{
	make_obj(wk, res, obj_array);

	struct build_target_extract_objects_ctx ctx = {
		.err_node = err_node,
		.res = res,
		.tgt = get_obj_build_target(wk, self),
		.tgt_id = self,
	};

	return obj_array_foreach_flat(wk, arr, &ctx, build_target_extract_objects_iter);
}

static bool
func_build_target_extract_objects(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[]
		= { { TYPE_TAG_GLOB | tc_string | tc_file | tc_custom_target | tc_generated_list }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	return build_target_extract_objects(wk, self, an[0].node, res, an[0].val);
}

static enum iteration_result
build_target_extract_all_objects_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct build_target_extract_objects_ctx *ctx = _ctx;

	return build_target_extract_objects_iter(wk, ctx, val);
}

bool
build_target_extract_all_objects(struct workspace *wk, uint32_t ip, obj self, obj *res, bool recursive)
{
	make_obj(wk, res, obj_array);

	struct build_target_extract_objects_ctx ctx = {
		.res = res,
		.tgt = get_obj_build_target(wk, self),
		.tgt_id = self,
	};

	if (!obj_array_foreach_flat(wk, ctx.tgt->src, &ctx, build_target_extract_all_objects_iter)) {
		return false;
	}

	if (recursive) {
		obj_array_extend(wk, *res, ctx.tgt->objects);
	}

	return true;
}

static bool
func_build_target_extract_all_objects(struct workspace *wk, obj self, obj *res)
{
	enum kwargs {
		kw_recursive,
	};
	struct args_kw akw[] = { [kw_recursive] = { "recursive", obj_bool }, 0 };
	if (!pop_args(wk, NULL, akw)) {
		return false;
	}

	bool recursive = akw[kw_recursive].set ? get_obj_bool(wk, akw[kw_recursive].val) : false;

	return build_target_extract_all_objects(wk, 0, self, res, recursive);
}

static bool
func_build_target_private_dir_include(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_include_directory);
	struct obj_include_directory *inc = get_obj_include_directory(wk, *res);

	inc->path = get_obj_build_target(wk, self)->private_path;
	return true;
}

static bool
func_build_target_found(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, true);
	return true;
}

const struct func_impl impl_tbl_build_target[] = {
	{ "extract_all_objects", func_build_target_extract_all_objects, tc_array },
	{ "extract_objects", func_build_target_extract_objects, tc_array },
	{ "found", func_build_target_found, tc_bool },
	{ "full_path", func_build_target_full_path, tc_string },
	{ "name", func_build_target_name, tc_string },
	{ "path", func_build_target_full_path, tc_string },
	{ "private_dir_include", func_build_target_private_dir_include, tc_string },
	{ NULL, NULL },
};

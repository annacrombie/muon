/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "coerce.h"
#include "error.h"
#include "functions/build_target.h"
#include "functions/common.h"
#include "functions/generator.h"
#include "functions/kernel/custom_target.h"
#include "lang/typecheck.h"
#include "log.h"
#include "platform/path.h"

struct generated_list_process_for_target_ctx {
	uint32_t node;
	struct obj_generator *g;
	struct obj_generated_list *gl;
	const char *dir;
	bool add_targets, generated_include;
	obj *res;

	// for generated_list_process_for_target_result_iter only
	obj name;
	obj custom_target, tmp_arr;
	struct obj_custom_target *t;
};

static enum iteration_result
generated_list_process_for_target_result_iter(struct workspace *wk, void *_ctx, obj file)
{
	struct generated_list_process_for_target_ctx *ctx = _ctx;

	obj_array_push(wk, ctx->tmp_arr, file);

	if (ctx->add_targets) {
		const char *generated_path = get_cstr(wk, *get_obj_file(wk, file));

		enum compiler_language l;
		if (!ctx->generated_include && filename_to_compiler_language(generated_path, &l)
			&& languages[l].is_header) {
			ctx->generated_include = true;
		}

		SBUF(rel);
		path_relative_to(wk, &rel, wk->build_root, generated_path);

		str_app(wk, &ctx->name, " ");
		str_app(wk, &ctx->name, rel.buf);
	}

	return ir_cont;
}

static enum iteration_result
generated_list_process_for_target_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct generated_list_process_for_target_ctx *ctx = _ctx;

	SBUF(path);
	const char *output_dir = ctx->dir;

	if (ctx->gl->preserve_path_from) {
		const char *src = get_file_path(wk, val), *base = get_cstr(wk, ctx->gl->preserve_path_from);
		assert(path_is_subpath(base, src));

		SBUF(dir);

		path_relative_to(wk, &path, base, src);
		path_dirname(wk, &dir, path.buf);
		path_join(wk, &path, ctx->dir, dir.buf);
		output_dir = path.buf;
	}

	struct make_custom_target_opts opts = {
		.input_node = ctx->node,
		.output_node = ctx->node,
		.command_node = ctx->node,
		.input_orig = val,
		.output_orig = ctx->g->output,
		.output_dir = output_dir,
		.build_dir = ctx->dir,
		.command_orig = ctx->g->raw_command,
		.depfile_orig = ctx->g->depfile,
		.capture = ctx->g->capture,
		.feed = ctx->g->feed,
		.extra_args = ctx->gl->extra_arguments,
		.extra_args_valid = true,
	};

	obj tgt;
	if (!make_custom_target(wk, &opts, &tgt)) {
		return ir_err;
	}

	struct obj_custom_target *t = get_obj_custom_target(wk, tgt);
	ctx->custom_target = tgt;
	ctx->t = t;
	make_obj(wk, &ctx->tmp_arr, obj_array);

	if (ctx->add_targets) {
		ctx->name = make_str(wk, "");
	}

	if (!obj_array_foreach(wk, t->output, ctx, generated_list_process_for_target_result_iter)) {
		return ir_err;
	}

	obj_array_extend_nodup(wk, *ctx->res, ctx->tmp_arr);

	if (ctx->add_targets) {
		ctx->t->name = make_strf(wk, "<gen:%s>", get_cstr(wk, ctx->name));
		if (ctx->g->depends) {
			obj_array_extend(wk, ctx->t->depends, ctx->g->depends);
		}
		obj_array_push(wk, current_project(wk)->targets, ctx->custom_target);
	}

	return ir_cont;
}

bool
generated_list_process_for_target(struct workspace *wk, uint32_t err_node, obj gl, obj tgt, bool add_targets, obj *res)
{
	struct obj_generated_list *list = get_obj_generated_list(wk, gl);

	enum obj_type t = get_obj_type(wk, tgt);

	const char *private_path;

	switch (t) {
	case obj_both_libs: tgt = get_obj_both_libs(wk, tgt)->dynamic_lib;
	/* fallthrough */
	case obj_build_target: private_path = get_cstr(wk, get_obj_build_target(wk, tgt)->private_path); break;
	case obj_custom_target: {
		private_path = get_cstr(wk, get_obj_custom_target(wk, tgt)->private_path);
		break;
	}
	default: UNREACHABLE;
	}

	make_obj(wk, res, obj_array);

	struct generated_list_process_for_target_ctx ctx = {
		.node = err_node,
		.g = get_obj_generator(wk, list->generator),
		.gl = list,
		.dir = private_path,
		.add_targets = add_targets,
		.res = res,
	};

	if (!obj_array_foreach(wk, list->input, &ctx, generated_list_process_for_target_iter)) {
		return false;
	}

	if (add_targets && t == obj_build_target && ctx.generated_include) {
		get_obj_build_target(wk, tgt)->flags |= build_tgt_generated_include;
	}

	return true;
}

struct check_preserve_path_from_ctx {
	const struct obj_generated_list *gl;
	uint32_t err_node;
};

static enum iteration_result
check_preserve_path_from_iter(struct workspace *wk, void *_ctx, obj f)
{
	const struct check_preserve_path_from_ctx *ctx = _ctx;

	const char *src = get_file_path(wk, f), *base = get_cstr(wk, ctx->gl->preserve_path_from);

	if (!path_is_subpath(base, src)) {
		vm_error_at(wk,
			ctx->err_node,
			"source file '%s' is not a subdir of preserve_path_from path '%s'",
			src,
			base);
		return ir_err;
	}

	return ir_cont;
}

static bool
func_generator_process(struct workspace *wk, obj gen, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | tc_coercible_files }, ARG_TYPE_NULL };
	enum kwargs {
		kw_extra_args,
		kw_preserve_path_from,
	};
	struct args_kw akw[] = { [kw_extra_args] = { "extra_args", TYPE_TAG_LISTIFY | obj_string },
		[kw_preserve_path_from] = { "preserve_path_from", obj_string },
		0 };

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	make_obj(wk, res, obj_generated_list);
	struct obj_generated_list *gl = get_obj_generated_list(wk, *res);
	gl->generator = gen;
	gl->extra_arguments = akw[kw_extra_args].val;
	gl->preserve_path_from = akw[kw_preserve_path_from].val;

	if (!coerce_files(wk, an[0].node, an[0].val, &gl->input)) {
		return false;
	}

	if (gl->preserve_path_from) {
		if (!path_is_absolute(get_cstr(wk, gl->preserve_path_from))) {
			vm_error_at(wk, akw[kw_preserve_path_from].node, "preserve_path_from must be an absolute path");
			return false;
		}

		struct check_preserve_path_from_ctx ctx = { .gl = gl, .err_node = akw[kw_preserve_path_from].node };

		if (!obj_array_foreach(wk, gl->input, &ctx, check_preserve_path_from_iter)) {
			return false;
		}
	}

	return true;
}

const struct func_impl impl_tbl_generator[] = {
	{ "process", func_generator_process, tc_generated_list },
	{ NULL, NULL },
};

#include "posix.h"

#include "backend/ninja.h"
#include "external/samurai.h"
#include "functions/default/setup.h"
#include "lang/eval.h"
#include "lang/interpreter.h"
#include "lang/serial.h"
#include "log.h"
#include "platform/filesystem.h"
#include "platform/path.h"
#include "tracy.h"

uint32_t func_setup_flags = 0;

bool
do_setup(struct workspace *wk)
{
	TracyCZoneAutoS;
	uint32_t project_id;

	TracyCZoneN(tctx_eval_project, "eval project", true);
	if (!eval_project(wk, NULL, wk->source_root, wk->build_root, &project_id)) {
		TracyCZoneEnd(tctx_eval_project);
		TracyCZoneAutoE;
		return false;
	}
	TracyCZoneEnd(tctx_eval_project);

	log_plain("\n");

	TracyCZoneN(tctx_write_backend, "write backend", true);
	if (!ninja_write_all(wk)) {
		TracyCZoneEnd(tctx_write_backend);
		TracyCZoneAutoE;
		return false;
	}
	TracyCZoneEnd(tctx_write_backend);

	workspace_print_summaries(wk);

	LOG_I("setup complete");
	TracyCZoneAutoE;
	return true;
}

struct set_options_ctx {
	struct workspace *sub_wk;
	uint32_t err_node, parent;
};

static enum iteration_result
set_options_iter(struct workspace *wk, void *_ctx, obj key, obj v)
{
	struct set_options_ctx *ctx = _ctx;
	enum obj_type t = get_obj_type(wk, v);

	if (t == obj_dict) {
		if (ctx->parent) {
			interp_error(wk, ctx->err_node, "options nested too deep");
			return ir_err;
		}

		ctx->parent = key;

		if (!obj_dict_foreach(wk, v, ctx, set_options_iter)) {
			return ir_err;
		}

		ctx->parent = 0;
		return ir_cont;
	}

	obj opt_val;

	if (t == obj_option) {
		opt_val = get_obj_option(wk, v)->val;
	} else {
		opt_val = v;
	}

	struct option_override oo = { .obj_value = true };
	if (ctx->parent && *get_cstr(wk, ctx->parent)) {
		oo.proj = make_str(ctx->sub_wk, get_cstr(wk, ctx->parent));
	}

	oo.name = str_clone(wk, ctx->sub_wk, key);

	if (!obj_clone(wk, ctx->sub_wk, opt_val, &oo.val)) {
		return ir_err;
	}

	darr_push(&ctx->sub_wk->option_overrides, &oo);

	return ir_cont;
}

static bool
set_options_from_file(struct workspace *wk, struct set_options_ctx *ctx, obj file)
{
	FILE *f;
	if (!(f = fs_fopen(get_file_path(wk, file), "r"))) {
		return false;
	}

	obj opts;
	if (!serial_load(wk, &opts, f)) {
		return false;
	}

	return obj_dict_foreach(wk, opts, ctx, set_options_iter);
}

bool
func_setup(struct workspace *wk, uint32_t _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_options,
		kw_source,
		kw_cross,
	};
	struct args_kw akw[] = {
		[kw_source] = { "source", obj_string },
		[kw_options] = { "options", obj_any },
		[kw_cross] = { "cross", obj_string },
		0
	};

	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	struct workspace sub_wk;
	bool ret = false;
	char build_ninja[PATH_MAX];

	if (akw[kw_source].set) {
		L("chdir to '%s'", get_cstr(wk, akw[kw_source].val));
		if (!path_chdir(get_cstr(wk, akw[kw_source].val))) {
			return false;
		}
	}

	workspace_init(&sub_wk);

	if (!workspace_setup_dirs(&sub_wk, get_cstr(wk, an[0].val), wk->argv0, true)) {
		goto ret;
	}

	LOG_I("setting up %s", sub_wk.build_root);

	if (akw[kw_options].set) {
		obj val;
		if (!obj_array_flatten_one(wk, akw[kw_options].val, &val)) {
			interp_error(wk, akw[kw_options].node, "could not flatten argument");
		}

		struct set_options_ctx ctx = {
			.sub_wk = &sub_wk,
			.err_node = akw[kw_options].node
		};

		enum obj_type t = get_obj_type(wk, val);
		if (t == obj_dict) {
			if (!obj_dict_foreach(wk, val, &ctx, set_options_iter)) {
				goto ret;
			}
		} else if (t == obj_file) {
			set_options_from_file(wk, &ctx, val);
		} else {
			interp_error(wk, akw[kw_options].node, "expected dict or file for options, got %s",
				obj_type_to_s(t));
			goto ret;
		}
	}

	if (!path_join(build_ninja, PATH_MAX, sub_wk.build_root, "build.ninja")) {
		goto ret;
	}

	if ((func_setup_flags & func_setup_flag_force) || !fs_file_exists(build_ninja)) {
		if (!do_setup(&sub_wk)) {
			goto ret;
		}
	}

	if (!(func_setup_flags & func_setup_flag_no_build) && have_samurai) {
		if (!path_chdir(sub_wk.build_root)) {
			return false;
		} else if (!muon_samu(0, (char *[]){ "<muon_samu>", NULL })) {
			return false;
		} else if (!path_chdir(sub_wk.source_root)) {
			return false;
		}
	}

	ret = true;
ret:
	workspace_destroy(&sub_wk);

	if (akw[kw_source].set) {
		if (!path_chdir(wk->source_root)) {
			return false;
		}
	}
	return ret;
}

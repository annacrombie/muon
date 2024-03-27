/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "args.h"
#include "backend/ninja.h"
#include "backend/ninja/alias_target.h"
#include "backend/ninja/build_target.h"
#include "backend/ninja/custom_target.h"
#include "backend/ninja/rules.h"
#include "backend/output.h"
#include "error.h"
#include "external/samurai.h"
#include "lang/serial.h"
#include "log.h"
#include "options.h"
#include "platform/filesystem.h"
#include "platform/mem.h"
#include "platform/path.h"
#include "platform/run_cmd.h"
#include "tracy.h"

struct check_tgt_ctx {
	bool need_phony;
};

static enum iteration_result
check_tgt_iter(struct workspace *wk, void *_ctx, obj tgt_id)
{
	struct check_tgt_ctx *ctx = _ctx;

	enum obj_type t = get_obj_type(wk, tgt_id);
	switch (t) {
	case obj_custom_target: {
		struct obj_custom_target *tgt = get_obj_custom_target(wk, tgt_id);

		if (tgt->flags & custom_target_build_always_stale) {
			ctx->need_phony = true;
		}
		break;
	}
	case obj_alias_target:
	case obj_build_target:
	case obj_both_libs: break;
	default: LOG_E("invalid tgt type '%s'", obj_type_to_s(t)); return ir_err;
	}

	return ir_cont;
}

static enum iteration_result
write_tgt_iter(struct workspace *wk, void *_ctx, obj tgt_id)
{
	TracyCZoneAutoS;
	enum iteration_result ret;
	struct write_tgt_ctx *ctx = _ctx;

	struct obj_clear_mark mk;
	obj_set_clear_mark(wk, &mk);

	const char *name = NULL;

	enum obj_type t = get_obj_type(wk, tgt_id);
	switch (t) {
	case obj_alias_target:
		ret = ninja_write_alias_tgt(wk, tgt_id, ctx);
		name = get_cstr(wk, get_obj_alias_target(wk, tgt_id)->name);
		break;
	case obj_both_libs: tgt_id = get_obj_both_libs(wk, tgt_id)->dynamic_lib;
	/* fallthrough */
	case obj_build_target:
		ret = ninja_write_build_tgt(wk, tgt_id, ctx);
		name = get_cstr(wk, get_obj_build_target(wk, tgt_id)->build_name);
		break;
	case obj_custom_target:
		ret = ninja_write_custom_tgt(wk, tgt_id, ctx);
		name = get_cstr(wk, get_obj_custom_target(wk, tgt_id)->name);
		break;
	default:
		LOG_E("invalid tgt type '%s'", obj_type_to_s(t));
		ret = ir_err;
		break;
	}

	if (!ret) {
		LOG_E("failed to write %s '%s'", obj_type_to_s(t), name);
	}

	obj_clear(wk, &mk);

	TracyCZoneAutoE;
	return ret;
}

struct write_build_ctx {
	obj compiler_rule_arr;
};

static bool
ninja_write_build(struct workspace *wk, void *_ctx, FILE *out)
{
	struct write_build_ctx *ctx = _ctx;
	struct check_tgt_ctx check_ctx = { 0 };

	uint32_t i;
	for (i = 0; i < wk->projects.len; ++i) {
		struct project *proj = arr_get(&wk->projects, i);
		if (proj->not_ok) {
			continue;
		}

		obj_array_foreach(wk, proj->targets, &check_ctx, check_tgt_iter);
	}

	if (!ninja_write_rules(out, wk, arr_get(&wk->projects, 0), check_ctx.need_phony, ctx->compiler_rule_arr)) {
		return false;
	}

	bool wrote_default = false;

	for (i = 0; i < wk->projects.len; ++i) {
		struct project *proj = arr_get(&wk->projects, i);
		if (proj->not_ok) {
			continue;
		}

		struct write_tgt_ctx ctx = { .out = out, .proj = proj };

		if (!obj_array_foreach(wk, proj->targets, &ctx, write_tgt_iter)) {
			LOG_E("failed to write rules for project %s", get_cstr(wk, proj->cfg.name));
			return false;
		}

		wrote_default |= ctx.wrote_default;
	}

	if (!wrote_default) {
		fprintf(out,
			"build muon_do_nothing: phony\n"
			"default muon_do_nothing\n");
	}

	return true;
}

static bool
ninja_write_tests(struct workspace *wk, void *_ctx, FILE *out)
{
	bool wrote_header = false;

	obj tests;
	make_obj(wk, &tests, obj_dict);

	uint32_t i;
	for (i = 0; i < wk->projects.len; ++i) {
		struct project *proj = arr_get(&wk->projects, i);
		if (proj->not_ok) {
			continue;
		}

		if (proj->tests && get_obj_array(wk, proj->tests)->len) {
			if (!wrote_header) {
				L("writing tests");
				wrote_header = true;
			}

			obj res, key;
			key = proj->cfg.name;

			if (obj_dict_index(wk, tests, key, &res)) {
				assert(false && "project defined multiple times");
			}

			obj arr;
			make_obj(wk, &arr, obj_array);

			obj_array_push(wk, arr, proj->tests);
			obj_array_push(wk, arr, proj->test_setups);
			obj_dict_set(wk, tests, key, arr);
		}
	}

	return serial_dump(wk, tests, out);
}

static bool
ninja_write_install(struct workspace *wk, void *_ctx, FILE *out)
{
	obj o;
	make_obj(wk, &o, obj_array);
	obj_array_push(wk, o, wk->install);
	obj_array_push(wk, o, wk->install_scripts);
	obj_array_push(wk, o, make_str(wk, wk->source_root));

	struct project *proj = arr_get(&wk->projects, 0);
	obj prefix;
	get_option_value(wk, proj, "prefix", &prefix);
	obj_array_push(wk, o, prefix);

	return serial_dump(wk, o, out);
}

static bool
ninja_write_compiler_check_cache(struct workspace *wk, void *_ctx, FILE *out)
{
	return serial_dump(wk, wk->compiler_check_cache, out);
}

static bool
ninja_write_summary_file(struct workspace *wk, void *_ctx, FILE *out)
{
	workspace_print_summaries(wk, out);
	return true;
}

static bool
ninja_write_option_info(struct workspace *wk, void *_ctx, FILE *out)
{
	obj arr;
	make_obj(wk, &arr, obj_array);
	obj_array_push(wk, arr, wk->global_opts);

	struct project *main_proj = arr_get(&wk->projects, 0);
	obj_array_push(wk, arr, main_proj->opts);

	return serial_dump(wk, arr, out);
}

bool
ninja_write_all(struct workspace *wk)
{
	struct write_build_ctx ctx = { 0 };
	make_obj(wk, &ctx.compiler_rule_arr, obj_array);

	if (!(with_open(wk->build_root, "build.ninja", wk, &ctx, ninja_write_build)
		    && with_open(wk->muon_private, output_path.tests, wk, NULL, ninja_write_tests)
		    && with_open(wk->muon_private, output_path.install, wk, NULL, ninja_write_install)
		    && with_open(wk->muon_private,
			    output_path.compiler_check_cache,
			    wk,
			    NULL,
			    ninja_write_compiler_check_cache)
		    && with_open(wk->muon_private, output_path.summary, wk, NULL, ninja_write_summary_file)
		    && with_open(wk->muon_private, output_path.option_info, wk, NULL, ninja_write_option_info))) {
		return false;
	}

	{ /* compile_commands.json */
		TracyCZoneN(tctx_compdb, "output compile_commands.json", true);

		obj compdb_args;
		make_obj(wk, &compdb_args, obj_array);
		obj_array_push(wk, compdb_args, make_str(wk, "-C"));
		obj_array_push(wk, compdb_args, make_str(wk, wk->build_root));
		obj_array_push(wk, compdb_args, make_str(wk, "-t"));
		obj_array_push(wk, compdb_args, make_str(wk, "compdb"));
		obj_array_extend_nodup(wk, compdb_args, ctx.compiler_rule_arr);
		if (!ninja_run(wk, compdb_args, wk->build_root, "compile_commands.json")) {
			LOG_E("error writing compile_commands.json");
		}

		TracyCZoneEnd(tctx_compdb);
	}

	return true;
}

bool
ninja_run(struct workspace *wk, obj args, const char *chdir, const char *capture)
{
	const char *argstr;
	uint32_t argstr_argc;

	bool ret = false;
	char *const *argv = NULL;
	uint32_t argc;
	SBUF_manual(cwd);

	if (chdir) {
		path_copy_cwd(NULL, &cwd);

		if (!path_chdir(chdir)) {
			goto ret;
		}
	}

	if (have_samurai) {
		join_args_argstr(wk, &argstr, &argstr_argc, args);
		argc = argstr_to_argv(argstr, argstr_argc, "samu", &argv);

		struct samu_opts samu_opts = { .out = stdout };
		if (capture) {
			if (!(samu_opts.out = fs_fopen(capture, "wb"))) {
				goto ret;
			}
		}

		ret = samu_main(argc, (char **)argv, &samu_opts);

		if (capture) {
			if (!fs_fclose(samu_opts.out)) {
				goto ret;
			}
		}
	} else {
		struct run_cmd_ctx cmd_ctx = { 0 };

		SBUF_manual(cmd);
		const char *prepend = NULL;

		obj ninja_opt_id;
		if (!get_option(wk, NULL, &WKSTR("env.NINJA"), &ninja_opt_id)) {
			UNREACHABLE;
		}
		const struct obj_option *ninja_opt = get_obj_option(wk, ninja_opt_id);

		if (ninja_opt->source == option_value_source_default && have_samurai) {
			obj cmd_arr;
			make_obj(wk, &cmd_arr, obj_array);
			obj_array_push(wk, cmd_arr, make_str(wk, wk->argv0));
			obj_array_push(wk, cmd_arr, make_str(wk, "samu"));
			obj_array_extend_nodup(wk, cmd_arr, args);
			args = cmd_arr;
		} else if (ninja_opt->source == option_value_source_default
			   && (fs_find_cmd(NULL, &cmd, "samu") || fs_find_cmd(NULL, &cmd, "ninja"))) {
			prepend = cmd.buf;
		} else {
			obj cmd_arr;
			obj_array_dup(wk, ninja_opt->val, &cmd_arr);
			obj_array_extend_nodup(wk, cmd_arr, args);
			args = cmd_arr;
		}

		join_args_argstr(wk, &argstr, &argstr_argc, args);
		argc = argstr_to_argv(argstr, argstr_argc, prepend, &argv);

		if (!capture) {
			cmd_ctx.flags |= run_cmd_ctx_flag_dont_capture;
		}

		if (!run_cmd_argv(&cmd_ctx, argv, NULL, 0)) {
			LOG_E("%s", cmd_ctx.err_msg);
			goto run_cmd_done;
		}

		if (capture) {
			if (!fs_write(capture, (uint8_t *)cmd_ctx.out.buf, cmd_ctx.out.len)) {
				goto run_cmd_done;
			}
		}

		ret = cmd_ctx.status == 0;
run_cmd_done:
		sbuf_destroy(&cmd);
		run_cmd_ctx_destroy(&cmd_ctx);
	}

ret:
	if (argv) {
		z_free((void *)argv);
	}

	if (chdir) {
		path_chdir(cwd.buf);
	}

	sbuf_destroy(&cwd);
	return ret;
}

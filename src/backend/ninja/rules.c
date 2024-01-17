/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "args.h"
#include "backend/common_args.h"
#include "backend/ninja/rules.h"
#include "backend/output.h"
#include "error.h"
#include "functions/machine.h"
#include "lang/workspace.h"
#include "log.h"
#include "platform/path.h"
#include "tracy.h"

struct write_compiler_rule_ctx {
	FILE *out;
	struct project *proj;
	struct obj_build_target *tgt;
	obj args, generic_rules;
};

static void
uniqify_name(struct workspace *wk, obj arr, obj name, obj *res)
{
	uint32_t x = 1;
	while (obj_array_in(wk, arr, name)) {
		name = make_strf(wk, "%s%d", get_cstr(wk, name), x);
		++x;
	}

	obj_array_push(wk, arr, name);
	*res = name;
}

static void
escape_rule(struct sbuf *buf)
{
	uint32_t i;
	for (i = 0; i < buf->len; ++i) {
		if (buf->buf[i] == '_'
		    || ('a' <= buf->buf[i] && buf->buf[i] <= 'z')
		    || ('A' <= buf->buf[i] && buf->buf[i] <= 'Z')
		    || ('0' <= buf->buf[i] && buf->buf[i] <= '9')) {
			continue;
		}

		buf->buf[i] = '_';
	}
}

static enum iteration_result
write_linker_rule_iter(struct workspace *wk, void *_ctx, enum compiler_language l, obj comp_id)
{
	struct write_compiler_rule_ctx *ctx = _ctx;
	struct obj_compiler *comp = get_obj_compiler(wk, comp_id);
	enum compiler_type t = comp->type;

	obj args;
	make_obj(wk, &args, obj_array);
	obj_array_extend(wk, args, comp->cmd_arr);
	obj_array_push(wk, args, make_str(wk, "$ARGS"));

	push_args(wk, args, compilers[t].args.output("$out"));
	obj_array_push(wk, args, make_str(wk, "$in"));
	obj_array_push(wk, args, make_str(wk, "$LINK_ARGS"));

	obj link_command = join_args_plain(wk, args);

	fprintf(ctx->out, "rule %s_%s_linker\n"
		" command = %s\n"
		" description = linking $out\n\n",
		get_cstr(wk, ctx->proj->rule_prefix),
		compiler_language_to_s(l),
		get_cstr(wk, link_command));

	return ir_cont;
}

static void
write_compiler_rule(struct workspace *wk, FILE *out, obj rule_args, obj rule_name, enum compiler_language l, obj comp_id)
{
	struct obj_compiler *comp = get_obj_compiler(wk, comp_id);
	enum compiler_type t = comp->type;

	const char *deps = NULL;
	switch (compilers[t].deps) {
	case compiler_deps_none:
		break;
	case compiler_deps_gcc:
		deps = "gcc";
		break;
	case compiler_deps_msvc:
		deps = "msvc";
		break;
	}

	obj args;
	make_obj(wk, &args, obj_array);
	obj_array_extend(wk, args, comp->cmd_arr);
	obj_array_push(wk, args, rule_args);

	if (compilers[t].deps) {
		push_args(wk, args, compilers[t].args.deps("$out", "${out}.d"));
	}
	push_args(wk, args, compilers[t].args.output("$out"));
	push_args(wk, args, compilers[t].args.compile_only());
	obj_array_push(wk, args, make_str(wk, "$in"));

	obj compile_command = join_args_plain(wk, args);

	fprintf(out, "rule %s\n"
		" command = %s\n",
		get_cstr(wk, rule_name),
		get_cstr(wk, compile_command));
	if (compilers[t].deps) {
		fprintf(out,
			" deps = %s\n"
			" depfile = ${out}.d\n",
			deps);
	}
	fprintf(out,
		" description = compiling %s $out\n\n",
		compiler_language_to_s(l));
}

static enum iteration_result
write_compiler_rule_iter(struct workspace *wk, void *_ctx, enum compiler_language l, obj comp_id)
{
	struct write_compiler_rule_ctx *ctx = _ctx;

	obj rule_name;
	{
		obj rule_name_arr;
		if (!obj_dict_geti(wk, ctx->tgt->required_compilers, l, &rule_name_arr)) {
			return ir_cont;
		}

		obj specialized_rule;
		obj_array_index(wk, rule_name_arr, 0, &rule_name);
		obj_array_index(wk, rule_name_arr, 1, &specialized_rule);

		if (!specialized_rule) {
			return ir_cont;
		}
	}

	obj rule_args;
	if (!obj_dict_geti(wk, ctx->args, l, &rule_args)) {
		UNREACHABLE;
	}

	write_compiler_rule(wk, ctx->out, rule_args, rule_name, l, comp_id);
	return ir_cont;
}

static enum iteration_result
write_compiler_rule_tgt_iter(struct workspace *wk, void *_ctx, obj tgt_id)
{
	struct write_compiler_rule_ctx *ctx = _ctx;
	enum iteration_result ret = ir_err;

	if (get_obj_type(wk, tgt_id) != obj_build_target) {
		return ir_cont;
	}

	struct obj_clear_mark mk;
	obj_set_clear_mark(wk, &mk);

	ctx->tgt = get_obj_build_target(wk, tgt_id);

	if (!build_target_args(wk, ctx->proj, ctx->tgt, &ctx->args)) {
		goto ret;
	}

	if (!obj_dict_foreach(wk, ctx->proj->compilers, ctx, write_compiler_rule_iter)) {
		goto ret;
	}

	ret = ir_cont;
ret:
	obj_clear(wk, &mk);
	return ret;
}

static enum iteration_result
write_generic_compiler_rule_iter(struct workspace *wk, void *_ctx, enum compiler_language l, obj comp_id)
{
	struct write_compiler_rule_ctx *ctx = _ctx;
	obj rule_name;

	if (!obj_dict_geti(wk, ctx->generic_rules, l, &rule_name)) {
		return ir_cont;
	}

	write_compiler_rule(wk, ctx->out, make_str(wk, "$ARGS"), rule_name, l, comp_id);
	return ir_cont;
}

struct name_compiler_rule_ctx {
	struct project *proj;
	struct obj_build_target *tgt;
	obj rule_prefix_arr;
	obj compiler_rule_arr;
	obj generic_rules;
};

static enum iteration_result
name_compiler_rule_iter(struct workspace *wk, void *_ctx, enum compiler_language l, uint32_t count)
{
	struct name_compiler_rule_ctx *ctx = _ctx;
	bool specialized_rule = count > 2;

	obj rule_name;
	SBUF(rule_name_buf);
	if (specialized_rule) {
		sbuf_pushf(wk, &rule_name_buf, "%s_%s_compiler_for_%s",
			get_cstr(wk, ctx->proj->rule_prefix),
			compiler_language_to_s(l),
			get_cstr(wk, ctx->tgt->build_name)
			);

		escape_rule(&rule_name_buf);
		obj name = sbuf_into_str(wk, &rule_name_buf);
		uniqify_name(wk, ctx->compiler_rule_arr, name, &rule_name);
	} else {
		if (!obj_dict_geti(wk, ctx->generic_rules, l, &rule_name)) {
			sbuf_pushf(wk, &rule_name_buf, "%s_%s_compiler",
				get_cstr(wk, ctx->proj->rule_prefix),
				compiler_language_to_s(l));

			escape_rule(&rule_name_buf);
			obj name = sbuf_into_str(wk, &rule_name_buf);
			uniqify_name(wk, ctx->compiler_rule_arr, name, &rule_name);
			obj_dict_seti(wk, ctx->generic_rules, l, rule_name);
		}
	}

	obj arr;
	make_obj(wk, &arr, obj_array);
	obj_array_push(wk, arr, rule_name);
	obj_array_push(wk, arr, specialized_rule);

	obj_dict_seti(wk, ctx->tgt->required_compilers, l, arr);
	return ir_cont;
}

static enum iteration_result
name_compiler_rule_tgt_iter(struct workspace *wk, void *_ctx, obj tgt_id)
{
	struct name_compiler_rule_ctx *ctx = _ctx;

	if (get_obj_type(wk, tgt_id) != obj_build_target) {
		return ir_cont;
	}

	ctx->tgt = get_obj_build_target(wk, tgt_id);

	if (!obj_dict_foreach(wk, ctx->tgt->required_compilers, ctx, name_compiler_rule_iter)) {
		return ir_err;
	}

	return ir_cont;
}

bool
ninja_write_rules(FILE *out, struct workspace *wk, struct project *main_proj,
	bool need_phony,
	obj compiler_rule_arr)
{
	TracyCZoneAutoS;
	bool res = false;

	fprintf(
		out,
		"# This is the build file for project \"%s\"\n"
		"# It is autogenerated by the muon build system.\n"
		"ninja_required_version = 1.7.1\n"
		"builddir = %s\n\n",
		get_cstr(wk, main_proj->cfg.name),
		output_path.private_dir
		);

	if (machine_system() == machine_system_windows) {
		fprintf(out,
			"rule static_linker\n"
			" command = sh -c \"rm -f $out && ar $LINK_ARGS $out $in\"\n"
			" description = linking static $out\n"
			"\n"
			);
	} else {
		fprintf(out,
			"rule static_linker\n"
			" command = rm -f $out && ar $LINK_ARGS $out $in\n"
			" description = linking static $out\n"
			"\n"
			);
	}

	fprintf(out,
		"rule CUSTOM_COMMAND\n"
		" command = $COMMAND\n"
		" description = $COMMAND\n"
		" restat = 1\n"
		"\n"
		"rule CUSTOM_COMMAND_DEP\n"
		" command = $COMMAND\n"
		" description = $COMMAND\n"
		" deps = gcc\n"
		" depfile = $DEPFILE\n"
		" restat = 1\n"
		"\n"
		);

	obj regen_cmd = join_args_shell(wk, regenerate_build_command(wk, false));

	fprintf(out,
		"rule REGENERATE_BUILD\n"
		" command = %s", get_cstr(wk, regen_cmd));

	fputs("\n description = Regenerating build files.\n"
		" generator = 1\n"
		"\n", out);

	obj regenerate_deps_rel;
	{
		obj deduped;
		obj_array_dedup(wk, wk->regenerate_deps, &deduped);
		relativize_paths(wk, deduped, true, &regenerate_deps_rel);
	}

	fprintf(out,
		"build build.ninja: REGENERATE_BUILD %s\n"
		" pool = console\n\n",
		get_cstr(wk, join_args_ninja(wk, regenerate_deps_rel))
		);

	if (need_phony) {
		fprintf(out, "build build_always_stale: phony\n\n");
	}

	obj rule_prefix_arr;
	make_obj(wk, &rule_prefix_arr, obj_array);
	uint32_t i;
	for (i = 0; i < wk->projects.len; ++i) {
		struct project *proj = arr_get(&wk->projects, i);
		if (proj->not_ok) {
			continue;
		}

		TracyCZoneN(tctx_name, "name rules", true);

		{ // determine project rule prefix
			SBUF(pre);
			sbuf_pushs(wk, &pre, get_cstr(wk, proj->cfg.name));
			escape_rule(&pre);
			uniqify_name(wk, rule_prefix_arr, sbuf_into_str(wk, &pre), &proj->rule_prefix);
		}

		obj generic_rules;
		make_obj(wk, &generic_rules, obj_dict);

		{
			struct name_compiler_rule_ctx ctx = {
				.proj = proj,
				.rule_prefix_arr = rule_prefix_arr,
				.compiler_rule_arr = compiler_rule_arr,
				.generic_rules = generic_rules,
			};

			if (!obj_array_foreach(wk, proj->targets, &ctx, name_compiler_rule_tgt_iter)) {
				goto ret;
			}
		}

		TracyCZoneEnd(tctx_name);

		{
			TracyCZoneN(tctx_rules, "write rules", true);

			struct write_compiler_rule_ctx ctx = {
				.out = out,
				.proj = proj,
				.generic_rules = generic_rules,
			};

			struct obj_clear_mark mk;
			obj_set_clear_mark(wk, &mk);

			if (!obj_array_foreach(wk, proj->targets, &ctx, write_compiler_rule_tgt_iter)) {
				goto ret;
			}

			if (!obj_dict_foreach(wk, proj->compilers, &ctx, write_generic_compiler_rule_iter)) {
				goto ret;
			}

			if (!obj_dict_foreach(wk, proj->compilers, &ctx, write_linker_rule_iter)) {
				goto ret;
			}

			obj_clear(wk, &mk);

			TracyCZoneEnd(tctx_rules);
		}
	}

	fprintf(out, "# targets\n\n");

	res = true;
ret:
	TracyCZoneAutoE;
	return res;
}

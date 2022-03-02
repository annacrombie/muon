#include "posix.h"

#include <string.h>

#include "args.h"
#include "backend/common_args.h"
#include "backend/ninja/build_target.h"
#include "functions/build_target.h"
#include "functions/dependency.h"
#include "lang/workspace.h"
#include "log.h"
#include "platform/filesystem.h"
#include "platform/path.h"
#include "tracy.h"

struct write_tgt_iter_ctx {
	FILE *out;
	const struct obj_build_target *tgt;
	const struct project *proj;
	struct dep_args_ctx args;
	obj joined_args;
	obj object_names;
	obj order_deps;
	bool have_order_deps;
	bool have_link_language;
	enum compiler_language link_language;
};

static enum iteration_result
write_tgt_sources_iter(struct workspace *wk, void *_ctx, obj val)
{
	TracyCZoneAutoS;
	struct write_tgt_iter_ctx *ctx = _ctx;
	const char *src = get_file_path(wk, val);

	enum compiler_language lang;
	enum compiler_type ct;

	{
		obj comp_id;

		// TODO put these checks into tgt creation
		if (!filename_to_compiler_language(src, &lang)) {
			/* LOG_E("unable to determine language for '%s'", get_cstr(wk, src->dat.file)); */

			TracyCZoneAutoE;
			return ir_cont;
		}

		if (languages[lang].is_header) {
			TracyCZoneAutoE;
			return ir_cont;
		} else if (languages[lang].is_linkable) {
			char path[PATH_MAX];
			if (!path_relative_to(path, PATH_MAX, wk->build_root, src)) {
				return ir_err;
			}
			obj_array_push(wk, ctx->object_names, make_str(wk, path));
			TracyCZoneAutoE;
			return ir_cont;
		}

		if (!obj_dict_geti(wk, ctx->proj->compilers, lang, &comp_id)) {
			LOG_E("no compiler for '%s'", compiler_language_to_s(lang));
			return ir_err;
		}

		ct = get_obj_compiler(wk, comp_id)->type;
	}

	/* build paths */
	char dest_path[PATH_MAX];
	if (!tgt_src_to_object_path(wk, ctx->tgt, val, true, dest_path)) {
		return ir_err;
	}

	char src_path[PATH_MAX];
	if (!path_relative_to(src_path, PATH_MAX, wk->build_root, src)) {
		return ir_err;
	}

	obj_array_push(wk, ctx->object_names, make_str(wk, dest_path));

	/* build rules and args */

	obj args_id;
	if (!obj_dict_geti(wk, ctx->joined_args, lang, &args_id)) {
		LOG_E("couldn't get args for language %s", compiler_language_to_s(lang));
		return ir_err;
	}

	char esc_dest_path[PATH_MAX], esc_path[PATH_MAX];
	if (!ninja_escape(esc_dest_path, PATH_MAX, dest_path)) {
		return false;
	} else if (!ninja_escape(esc_path, PATH_MAX, src_path)) {
		return false;
	}

	fprintf(ctx->out, "build %s: %s_COMPILER %s", esc_dest_path, compiler_language_to_s(lang), esc_path);
	if (ctx->have_order_deps) {
		fprintf(ctx->out, " || %s", get_cstr(wk, ctx->order_deps));
	}
	fputc('\n', ctx->out);

	fprintf(ctx->out,
		" ARGS = %s\n", get_cstr(wk, args_id));

	if (compilers[ct].deps) {
		if (!path_add_suffix(esc_dest_path, PATH_MAX, ".d")) {
			return false;
		}

		fprintf(ctx->out, " DEPFILE_UNQUOTED = %s\n", esc_dest_path);

		if (!shell_escape(esc_path, PATH_MAX, esc_dest_path)) {
			return false;
		}

		fprintf(ctx->out, " DEPFILE = %s\n", esc_path);
	}

	fputc('\n', ctx->out);

	TracyCZoneAutoE;
	return ir_cont;
}

static enum iteration_result
process_source_includes_iter(struct workspace *wk, void *_ctx, obj val)
{
	TracyCZoneAutoS;
	struct write_tgt_iter_ctx *ctx = _ctx;
	const char *src = get_file_path(wk, val);

	enum compiler_language fl;

	if (filename_to_compiler_language(src, &fl)
	    && !languages[fl].is_header) {
		/* LOG_E("unable to determine language for '%s'", get_cstr(wk, src->dat.file)); */
		TracyCZoneAutoE;
		return ir_cont;
	}

	char dir[PATH_MAX], path[PATH_MAX];

	if (!path_relative_to(path, PATH_MAX, wk->build_root, src)) {
		return ir_err;
	}

	obj_array_push(wk, ctx->order_deps, make_str(wk, path));
	ctx->have_order_deps = true;

	if (!path_dirname(dir, PATH_MAX, path)) {
		return ir_err;
	}

	obj_array_push(wk, ctx->args.include_dirs, make_str(wk, dir));

	TracyCZoneAutoE;
	return ir_cont;
}

static enum iteration_result
determine_linker_iter(struct workspace *wk, void *_ctx, obj val)
{
	TracyCZoneAutoS;
	struct write_tgt_iter_ctx *ctx = _ctx;

	enum compiler_language fl;

	if (!filename_to_compiler_language(get_file_path(wk, val), &fl)) {
		/* LOG_E("unable to determine language for '%s'", get_cstr(wk, src->dat.file)); */
		TracyCZoneAutoE;
		return ir_cont;
	}

	switch (fl) {
	case compiler_language_c_hdr:
	case compiler_language_cpp_hdr:
		TracyCZoneAutoE;
		return ir_cont;
	case compiler_language_c:
	case compiler_language_c_obj:
		if (!ctx->have_link_language) {
			ctx->link_language = compiler_language_c;
		}
		break;
	case compiler_language_cpp:
		if (!ctx->have_link_language
		    || ctx->link_language == compiler_language_c) {
			ctx->link_language = compiler_language_cpp;
		}
		break;
	case compiler_language_count:
		assert(false);
		return ir_err;
	}

	ctx->have_link_language = true;
	TracyCZoneAutoE;
	return ir_cont;
}

static enum iteration_result
tgt_args_includes_iter(struct workspace *wk, void *_ctx, obj inc_id)
{
	struct dep_args_ctx *ctx = _ctx;

	obj_array_push(wk, ctx->include_dirs, inc_id);
	return ir_cont;
}

static bool
tgt_args(struct workspace *wk, const struct obj_build_target *tgt, struct dep_args_ctx *ctx)
{
	TracyCZoneAutoS;

	// If we generated a header file for this target, add the private path
	// to the list of include directories.
	if (tgt->flags & build_tgt_generated_include) {
		const char *private_path = get_cstr(wk, tgt->private_path);

		// mkdir so that the include dir doesn't get pruned later on
		if (!fs_mkdir_p(private_path)) {
			return false;
		}

		obj_array_push(wk, ctx->include_dirs, make_str(wk, private_path));
	}

	if (tgt->include_directories) {
		TracyCZoneN(tctx, "include_directories", true);
		if (!obj_array_foreach_flat(wk, tgt->include_directories,
			ctx, tgt_args_includes_iter)) {
			return false;
		}
		TracyCZoneEnd(tctx);
	}

	L("include_len after include_dirs iter: %d", get_obj_array(wk, ctx->include_dirs)->len);

	if (tgt->deps) {
		TracyCZoneN(tctx, "deps", true);
		if (!deps_args(wk, tgt->deps, ctx)) {
			return false;
		}
		TracyCZoneEnd(tctx);
	}

	L("include_len after deps iter: %d", get_obj_array(wk, ctx->include_dirs)->len);

	if (tgt->link_with) {
		TracyCZoneN(tctx, "link_with", true);
		if (!deps_args_link_with_only(wk, tgt->link_with, ctx)) {
			return false;
		}
		TracyCZoneEnd(tctx);
	}

	if (tgt->link_args) {
		TracyCZoneN(tctx, "link_args", true);
		obj arr;
		obj_array_dup(wk, tgt->link_args, &arr);
		obj_array_extend(wk, ctx->link_args, arr);
		TracyCZoneEnd(tctx);
	}
	TracyCZoneAutoE;
	return true;
}

bool
ninja_write_build_tgt(struct workspace *wk, const struct project *proj, obj tgt_id, FILE *out)
{
	TracyCZoneAutoS;
	struct obj_build_target *tgt = get_obj_build_target(wk, tgt_id);
	LOG_I("writing rules for target '%s'", get_cstr(wk, tgt->build_name));

	struct write_tgt_iter_ctx ctx = {
		.tgt = tgt,
		.proj = proj,
		.out = out,
	};

	enum linker_type linker;
	{ /* determine linker */
		if (!obj_array_foreach(wk, tgt->src, &ctx, determine_linker_iter)) {
			goto err;
		}

		if (!ctx.have_link_language) {
			enum compiler_language clink_langs[] = {
				compiler_language_c,
				compiler_language_cpp,
			};

			obj comp;
			uint32_t i;
			for (i = 0; i < ARRAY_LEN(clink_langs); ++i) {
				if (obj_dict_geti(wk, ctx.proj->compilers, clink_langs[i], &comp)) {
					ctx.link_language = clink_langs[i];
					ctx.have_link_language  = true;
					break;
				}
			}
		}

		if (!ctx.have_link_language) {
			LOG_E("unable to determine linker for target");
			goto err;
		}

		obj comp_id;
		if (!obj_dict_geti(wk, ctx.proj->compilers, ctx.link_language, &comp_id)) {
			LOG_E("no compiler defined for language %s", compiler_language_to_s(ctx.link_language));
			goto err;
		}

		linker = compilers[get_obj_compiler(wk, comp_id)->type].linker;
	}

	make_obj(wk, &ctx.object_names, obj_array);
	make_obj(wk, &ctx.order_deps, obj_array);

	dep_args_ctx_init(wk, &ctx.args);
	ctx.args.relativize = true;
	ctx.args.recursive = true;

	if (!tgt_args(wk, tgt, &ctx.args)) {
		goto err;
	}

	/* sources includes */
	if (!obj_array_foreach(wk, tgt->src, &ctx, process_source_includes_iter)) {
		goto err;
	}

	if (!setup_compiler_args(wk, ctx.tgt, ctx.proj, ctx.args.include_dirs, ctx.args.compile_args, &ctx.joined_args)) {
		goto err;
	}

	ctx.order_deps = join_args_ninja(wk, ctx.order_deps);

	{ /* sources */
		if (!obj_array_foreach(wk, tgt->src, &ctx, write_tgt_sources_iter)) {
			goto err;
		}
	}

	obj implicit_deps = 0;
	if (tgt->type == tgt_executable) {
		struct setup_linker_args_ctx sctx = {
			.linker = linker,
			.link_lang = ctx.link_language,
			.args = &ctx.args
		};

		setup_linker_args(wk, ctx.proj, tgt, &sctx);

		if (get_obj_array(wk, ctx.args.link_with)->len) {
			implicit_deps = str_join(wk, make_str(wk, " | "), join_args_ninja(wk, ctx.args.link_with));
		}
	}

	const char *linker_type, *link_args;
	switch (tgt->type) {
	case tgt_shared_module:
	case tgt_dynamic_library:
	case tgt_executable:
		linker_type = compiler_language_to_s(ctx.link_language);

		if (tgt->type & (tgt_dynamic_library | tgt_shared_module)) {
			push_args(wk, ctx.args.link_args, linkers[linker].args.shared());
			push_args(wk, ctx.args.link_args, linkers[linker].args.soname(get_cstr(wk, tgt->soname)));

			if (tgt->type == tgt_shared_module) {
				push_args(wk, ctx.args.link_args, linkers[linker].args.allow_shlib_undefined());
			}
		}

		link_args = get_cstr(wk, join_args_shell_ninja(wk, ctx.args.link_args));
		break;
	case tgt_static_library:
		linker_type = "STATIC";
		link_args = "csrD";
		break;
	default:
		assert(false);
		goto err;
	}

	char esc_path[PATH_MAX];
	{
		char rel_build_path[PATH_MAX];
		if (!path_relative_to(rel_build_path, PATH_MAX, wk->build_root, get_cstr(wk, tgt->build_path))) {
			return false;
		}

		if (!ninja_escape(esc_path, PATH_MAX, rel_build_path)) {
			return false;
		}
	}

	fprintf(out, "build %s: %s_LINKER ", esc_path, linker_type);
	fputs(get_cstr(wk, join_args_ninja(wk, ctx.object_names)), out);
	if (implicit_deps) {
		fputs(get_cstr(wk, implicit_deps), out);
	}
	if (ctx.have_order_deps) {
		fputs(" || ", out);
		fputs(get_cstr(wk, ctx.order_deps), out);
	}
	fprintf(out, "\n LINK_ARGS = %s\n\n", link_args);

	TracyCZoneAutoE;
	return true;
err:
	TracyCZoneAutoE;
	return false;
}

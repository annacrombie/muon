/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: illiliti <illiliti@dimension.sh>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "backend/output.h"
#include "buf_size.h"
#include "cmd_install.h"
#include "functions/environment.h"
#include "lang/serial.h"
#include "log.h"
#include "platform/filesystem.h"
#include "platform/path.h"
#include "platform/rpath_fixer.h"
#include "platform/run_cmd.h"

struct copy_subdir_ctx {
	obj exclude_directories;
	obj exclude_files;
	bool has_perm;
	uint32_t perm;
	const char *src_base, *dest_base;
	const char *src_root;
	struct workspace *wk;
};

static enum iteration_result
copy_subdir_iter(void *_ctx, const char *path)
{
	struct copy_subdir_ctx *ctx = _ctx;
	SBUF(src);
	SBUF(dest);

	path_join(ctx->wk, &src, ctx->src_base, path);
	path_join(ctx->wk, &dest, ctx->dest_base, path);

	SBUF(rel);
	path_relative_to(ctx->wk, &rel, ctx->src_root, src.buf);
	obj rel_str = sbuf_into_str(ctx->wk, &rel);

	if (fs_dir_exists(src.buf)) {
		if (ctx->exclude_directories && obj_array_in(ctx->wk, ctx->exclude_directories, rel_str)) {
			LOG_I("skipping dir '%s'", src.buf);
			return ir_cont;
		}

		LOG_I("make dir '%s'", dest.buf);

		if (!fs_dir_exists(dest.buf)) {
			if (!fs_mkdir(dest.buf)) {
				return ir_err;
			}
		}

		struct copy_subdir_ctx new_ctx = {
			.exclude_directories = ctx->exclude_directories,
			.exclude_files = ctx->exclude_files,
			.has_perm = ctx->has_perm,
			.perm = ctx->perm,
			.src_root = ctx->src_root,
			.src_base = src.buf,
			.dest_base = dest.buf,
			.wk = ctx->wk,
		};

		if (!fs_dir_foreach(src.buf, &new_ctx, copy_subdir_iter)) {
			return ir_err;
		}
	} else if (fs_symlink_exists(src.buf) || fs_file_exists(src.buf)) {
		if (ctx->exclude_files && obj_array_in(ctx->wk, ctx->exclude_files, rel_str)) {
			LOG_I("skipping file '%s'", src.buf);
			return ir_cont;
		}

		LOG_I("install '%s' -> '%s'", src.buf, dest.buf);

		if (!fs_copy_file(src.buf, dest.buf)) {
			return ir_err;
		}
	} else {
		LOG_E("unhandled file type '%s'", path);
		return ir_err;
	}

	if (ctx->has_perm && !fs_chmod(dest.buf, ctx->perm)) {
		return ir_err;
	}

	return ir_cont;
}

struct install_ctx {
	struct install_options *opts;
	obj prefix;
	obj full_prefix;
	obj destdir;
};

static enum iteration_result
install_iter(struct workspace *wk, void *_ctx, obj v_id)
{
	struct install_ctx *ctx = _ctx;
	struct obj_install_target *in = get_obj_install_target(wk, v_id);

	SBUF(dest_dirname);
	const char *dest = get_cstr(wk, in->dest), *src = get_cstr(wk, in->src);

	assert(in->type == install_target_symlink || in->type == install_target_emptydir || path_is_absolute(src));

	SBUF(full_dest_dir);
	if (ctx->destdir) {
		path_join_absolute(wk, &full_dest_dir, get_cstr(wk, ctx->destdir), dest);
		dest = full_dest_dir.buf;
	}

	switch (in->type) {
	case install_target_default: LOG_I("install '%s' -> '%s'", src, dest); break;
	case install_target_subdir: LOG_I("install subdir '%s' -> '%s'", src, dest); break;
	case install_target_symlink: LOG_I("install symlink '%s' -> '%s'", dest, src); break;
	case install_target_emptydir: LOG_I("install emptydir '%s'", dest); break;
	default: abort();
	}

	if (ctx->opts->dry_run) {
		return ir_cont;
	}

	switch (in->type) {
	case install_target_default:
	case install_target_symlink:
		path_dirname(wk, &dest_dirname, dest);

		if (fs_exists(dest_dirname.buf) && !fs_dir_exists(dest_dirname.buf)) {
			LOG_E("dest '%s' exists and is not a directory", dest_dirname.buf);
			return ir_err;
		}

		if (!fs_mkdir_p(dest_dirname.buf)) {
			return ir_err;
		}

		if (in->type == install_target_default) {
			if (fs_dir_exists(src)) {
				if (!fs_copy_dir(src, dest)) {
					return ir_err;
				}
			} else {
				if (!fs_copy_file(src, dest)) {
					return ir_err;
				}
			}

			if (in->build_target) {
				if (!fix_rpaths(dest, wk->build_root)) {
					return ir_err;
				}
			}
		} else {
			if (!fs_make_symlink(src, dest, true)) {
				return ir_err;
			}
		}
		break;
	case install_target_subdir:
		if (!fs_mkdir_p(dest)) {
			return ir_err;
		}

		struct copy_subdir_ctx ctx = {
			.exclude_directories = in->exclude_directories,
			.exclude_files = in->exclude_files,
			.has_perm = in->has_perm,
			.perm = in->perm,
			.src_root = src,
			.src_base = src,
			.dest_base = dest,
			.wk = wk,
		};

		if (!fs_dir_foreach(src, &ctx, copy_subdir_iter)) {
			return ir_err;
		}
		break;
	case install_target_emptydir:
		if (!fs_mkdir_p(dest)) {
			return ir_err;
		}
		break;
	default: abort();
	}

	if (in->has_perm && !fs_chmod(dest, in->perm)) {
		return ir_err;
	}

	return ir_cont;
}

static enum iteration_result
install_scripts_iter(struct workspace *wk, void *_ctx, obj install_script)
{
	struct install_ctx *ctx = _ctx;

	obj install_script_dry_run, install_script_cmdline;
	obj_array_index(wk, install_script, 0, &install_script_dry_run);
	obj_array_index(wk, install_script, 1, &install_script_cmdline);

	bool script_can_dry_run = get_obj_bool(wk, install_script_dry_run);

	obj env;
	make_obj(wk, &env, obj_dict);
	if (ctx->destdir) {
		obj_dict_set(wk, env, make_str(wk, "DESTDIR"), ctx->destdir);
	}
	obj_dict_set(wk, env, make_str(wk, "MESON_INSTALL_PREFIX"), ctx->prefix);
	obj_dict_set(wk, env, make_str(wk, "MESON_INSTALL_DESTDIR_PREFIX"), ctx->full_prefix);
	if (ctx->opts->dry_run && script_can_dry_run) {
		obj_dict_set(wk, env, make_str(wk, "MESON_INSTALL_DRY_RUN"), make_str(wk, "1"));
	}
	set_default_environment_vars(wk, env, false);

	const char *argstr, *envstr;
	uint32_t argc, envc;
	env_to_envstr(wk, &envstr, &envc, env);
	join_args_argstr(wk, &argstr, &argc, install_script_cmdline);

	LOG_I("running install script '%s'", argstr);

	if (ctx->opts->dry_run && !script_can_dry_run) {
		return ir_cont;
	}

	struct run_cmd_ctx cmd_ctx = { 0 };
	if (!run_cmd(&cmd_ctx, argstr, argc, envstr, envc)) {
		LOG_E("failed to run install script: %s", cmd_ctx.err_msg);
		goto err;
	}

	if (cmd_ctx.status != 0) {
		LOG_E("install script failed");
		LOG_E("stdout: %s", cmd_ctx.out.buf);
		LOG_E("stderr: %s", cmd_ctx.err.buf);
		goto err;
	}

	run_cmd_ctx_destroy(&cmd_ctx);
	return ir_cont;
err:
	run_cmd_ctx_destroy(&cmd_ctx);
	return ir_err;
}

bool
install_run(struct install_options *opts)
{
	bool ret = true;
	SBUF_manual(install_src);
	path_join(NULL, &install_src, output_path.private_dir, output_path.install);

	FILE *f;
	f = fs_fopen(install_src.buf, "rb");
	sbuf_destroy(&install_src);

	if (!f) {
		return false;
	}

	struct workspace wk;
	workspace_init_bare(&wk);

	obj install;
	if (!serial_load(&wk, &install, f)) {
		LOG_E("failed to load %s", output_path.install);
		goto ret;
	} else if (!fs_fclose(f)) {
		goto ret;
	}

	struct install_ctx ctx = {
		.opts = opts,
	};

	obj install_targets, install_scripts, source_root;
	obj_array_index(&wk, install, 0, &install_targets);
	obj_array_index(&wk, install, 1, &install_scripts);
	obj_array_index(&wk, install, 2, &source_root);
	obj_array_index(&wk, install, 3, &ctx.prefix);

	SBUF(build_root);
	path_copy_cwd(&wk, &build_root);
	wk.build_root = get_cstr(&wk, sbuf_into_str(&wk, &build_root));
	wk.source_root = get_cstr(&wk, source_root);

	if ((opts->destdir)) {
		SBUF(full_prefix);
		SBUF(abs_destdir);
		path_make_absolute(&wk, &abs_destdir, opts->destdir);
		path_join_absolute(&wk, &full_prefix, abs_destdir.buf, get_cstr(&wk, ctx.prefix));

		ctx.full_prefix = sbuf_into_str(&wk, &full_prefix);
		ctx.destdir = sbuf_into_str(&wk, &abs_destdir);
	} else {
		ctx.full_prefix = ctx.prefix;
	}

	obj_array_foreach(&wk, install_targets, &ctx, install_iter);
	obj_array_foreach(&wk, install_scripts, &ctx, install_scripts_iter);

	ret = true;
ret:
	workspace_destroy_bare(&wk);
	return ret;
}

#include "posix.h"

#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "backend/output.h"
#include "buf_size.h"
#include "functions/environment.h"
#include "install.h"
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
	char src[PATH_MAX], dest[PATH_MAX];

	if (!path_join(src, PATH_MAX, ctx->src_base, path)) {
		return ir_err;
	} else if (!path_join(dest, PATH_MAX, ctx->dest_base, path)) {
		return ir_err;
	}

	char rel[PATH_MAX];
	if (!path_relative_to(rel, PATH_MAX, ctx->src_root, src)) {
		return ir_err;
	}

	if (fs_dir_exists(src)) {
		if (ctx->exclude_directories &&
		    obj_array_in(ctx->wk, ctx->exclude_directories, make_str(ctx->wk, rel))) {
			LOG_I("skipping dir '%s'", src);
			return ir_cont;
		}

		LOG_I("make dir '%s'", dest);

		if (!fs_dir_exists(dest)) {
			if (!fs_mkdir(dest)) {
				return ir_err;
			}
		}

		struct copy_subdir_ctx new_ctx = {
			.exclude_directories = ctx->exclude_directories,
			.exclude_files = ctx->exclude_files,
			.has_perm = ctx->has_perm,
			.perm = ctx->perm,
			.src_root = ctx->src_root,
			.src_base = src,
			.dest_base = dest,
			.wk = ctx->wk,
		};

		if (!fs_dir_foreach(src, &new_ctx, copy_subdir_iter)) {
			return ir_err;
		}
	} else if (fs_symlink_exists(src) || fs_file_exists(src)) {
		if (ctx->exclude_files &&
		    obj_array_in(ctx->wk, ctx->exclude_files, make_str(ctx->wk, rel))) {
			LOG_I("skipping file '%s'", src);
			return ir_cont;
		}

		LOG_I("install '%s' -> '%s'", src, dest);

		if (!fs_copy_file(src, dest)) {
			return ir_err;
		}
	} else {
		LOG_E("unhandled file type '%s'", path);
		return ir_err;
	}

	if (ctx->has_perm && !fs_chmod(dest, ctx->perm)) {
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

	char dest_dirname[PATH_MAX];
	const char *dest = get_cstr(wk, in->dest),
		   *src = get_cstr(wk, in->src);

	assert(in->type == install_target_symlink || in->type == install_target_emptydir || path_is_absolute(src));

	if (ctx->destdir) {
		static char full_dest_dir[PATH_MAX];
		if (!path_join_absolute(full_dest_dir, PATH_MAX, get_cstr(wk, ctx->destdir), dest)) {
			return ir_err;
		}

		dest = full_dest_dir;
	}

	switch (in->type) {
	case install_target_default:
		LOG_I("install '%s' -> '%s'", src, dest);
		break;
	case install_target_subdir:
		LOG_I("install subdir '%s' -> '%s'", src, dest);
		break;
	case install_target_symlink:
		LOG_I("install symlink '%s' -> '%s'", dest, src);
		break;
	case install_target_emptydir:
		LOG_I("install emptydir '%s'", dest);
		break;
	default:
		abort();
	}

	if (ctx->opts->dry_run) {
		return ir_cont;
	}

	switch (in->type) {
	case install_target_default:
	case install_target_symlink:
		if (!path_dirname(dest_dirname, PATH_MAX, dest)) {
			return ir_err;
		}

		if (fs_exists(dest_dirname) && !fs_dir_exists(dest_dirname)) {
			LOG_E("dest '%s' exists and is not a directory", dest_dirname);
			return ir_err;
		}

		if (!fs_mkdir_p(dest_dirname)) {
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
	default:
		abort();
	}

	if (in->has_perm && !fs_chmod(dest, in->perm)) {
		return ir_err;
	}

	return ir_cont;
}

static enum iteration_result
install_scripts_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct install_ctx *ctx = _ctx;

	obj env;
	make_obj(wk, &env, obj_dict);
	if (ctx->destdir) {
		obj_dict_set(wk, env, make_str(wk, "DESTDIR"), ctx->destdir);
	}
	obj_dict_set(wk, env, make_str(wk, "MESON_INSTALL_PREFIX"), ctx->prefix);
	obj_dict_set(wk, env, make_str(wk, "MESON_INSTALL_DESTDIR_PREFIX"), ctx->full_prefix);
	set_default_environment_vars(wk, env, false);

	const char *argstr, *envstr;
	env_to_envstr(wk, &envstr, env);
	join_args_argstr(wk, &argstr, v);

	LOG_I("running install script '%s'", argstr);

	if (ctx->opts->dry_run) {
		return ir_cont;
	}

	struct run_cmd_ctx cmd_ctx = { 0 };
	if (!run_cmd(&cmd_ctx, argstr, envstr)) {
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
	char install_src[PATH_MAX];
	if (!path_join(install_src, PATH_MAX, output_path.private_dir, output_path.install)) {
		return false;
	}

	FILE *f;
	if (!(f = fs_fopen(install_src, "r"))) {
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

	if (!path_cwd(wk.build_root, PATH_MAX)) {
		return false;
	}
	strncpy(wk.source_root, get_cstr(&wk, source_root), PATH_MAX - 1);

	const char *destdir;
	if ((destdir = getenv("DESTDIR"))) {
		char abs_destdir[PATH_MAX], full_prefix[PATH_MAX];
		if (!path_make_absolute(abs_destdir, PATH_MAX, destdir)) {
			return false;
		} else if (!path_join_absolute(full_prefix, PATH_MAX, abs_destdir,
			get_cstr(&wk, ctx.prefix))) {
			return false;
		}

		ctx.full_prefix = make_str(&wk, full_prefix);
		ctx.destdir = make_str(&wk, abs_destdir);
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

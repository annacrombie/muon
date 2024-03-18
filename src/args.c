/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: dffdff2423 <dffdff2423@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <assert.h>
#include <string.h>

#include "args.h"
#include "buf_size.h"
#include "compilers.h"
#include "functions/build_target.h"
#include "functions/environment.h"
#include "lang/workspace.h"
#include "log.h"
#include "platform/path.h"
#include "error.h"

void
push_args(struct workspace *wk, obj arr, const struct args *args)
{
	uint32_t i;
	for (i = 0; i < args->len; ++i) {
		obj_array_push(wk, arr, make_str(wk, args->args[i]));
	}
}

void
push_args_null_terminated(struct workspace *wk, obj arr, char *const *argv)
{
	char *const *arg;
	for (arg = argv; *arg; ++arg) {
		obj_array_push(wk, arr, make_str(wk, *arg));
	}
}

static void
simple_escape(struct workspace *wk, struct sbuf *sb, const char *str, const char *need_escaping, char esc_char)
{
	const char *s = str;

	for (; *s; ++s) {
		if (strchr(need_escaping, *s)) {
			sbuf_push(wk, sb, esc_char);
		} else if (*s == '\n') {
			assert(false && "newlines cannot be escaped");
		}

		sbuf_push(wk, sb, *s);
	}
}

void
ninja_escape(struct workspace *wk, struct sbuf *sb, const char *str)
{
	simple_escape(wk, sb, str, " :$", '$');
}

static void
shell_ninja_escape(struct workspace *wk, struct sbuf *sb, const char *str)
{
	SBUF_manual(tmp);

	shell_escape(wk, &tmp, str);
	simple_escape(wk, sb, tmp.buf, "$\n", '$');

	sbuf_destroy(&tmp);
}

void
pkgconf_escape(struct workspace *wk, struct sbuf *sb, const char *str)
{
	simple_escape(wk, sb, str, " ", '\\');
}

typedef void ((*escape_func)(struct workspace *wk, struct sbuf *sb, const char *str));

struct join_args_iter_ctx {
	uint32_t i, len;
	uint32_t *obj;
	escape_func escape;
};

static enum iteration_result
join_args_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct join_args_iter_ctx *ctx = _ctx;

	const char *s = get_cstr(wk, val);

	SBUF(esc);
	if (ctx->escape) {
		ctx->escape(wk, &esc, s);

		s = esc.buf;
	}

	str_app(wk, ctx->obj, s);

	if (ctx->i < ctx->len - 1) {
		str_app(wk, ctx->obj, " ");
	}

	++ctx->i;

	return ir_cont;
}

static obj
join_args(struct workspace *wk, obj arr, escape_func escape)
{
	obj o = make_str(wk, "");

	struct join_args_iter_ctx ctx = {
		.obj = &o,
		.len = get_obj_array(wk, arr)->len,
		.escape = escape
	};

	obj_array_foreach(wk, arr, &ctx, join_args_iter);
	return o;
}

obj
join_args_plain(struct workspace *wk, obj arr)
{
	return join_args(wk, arr, NULL);
}

obj
join_args_shell(struct workspace *wk, obj arr)
{
	return join_args(wk, arr, shell_escape);
}

obj
join_args_ninja(struct workspace *wk, obj arr)
{
	return join_args(wk, arr, ninja_escape);
}

obj
join_args_shell_ninja(struct workspace *wk, obj arr)
{
	return join_args(wk, arr, shell_ninja_escape);
}

obj
join_args_pkgconf(struct workspace *wk, obj arr)
{
	return join_args(wk, arr, pkgconf_escape);
}

struct arr_to_args_ctx {
	enum arr_to_args_flags mode;
	obj res;
};

static enum iteration_result
arr_to_args_iter(struct workspace *wk, void *_ctx, obj src)
{
	struct arr_to_args_ctx *ctx = _ctx;
	obj str;

	enum obj_type t = get_obj_type(wk, src);

	switch (t) {
	case obj_string:
		str = src;
		break;
	case obj_file:
		if (ctx->mode & arr_to_args_relativize_paths) {
			SBUF(rel);
			path_relative_to(wk, &rel, wk->build_root, get_file_path(wk, src));
			str = sbuf_into_str(wk, &rel);
			break;
		}
		str = *get_obj_file(wk, src);
		break;
	case obj_alias_target:
		if (!(ctx->mode & arr_to_args_alias_target)) {
			goto type_err;
		}
		str = get_obj_alias_target(wk, src)->name;
		break;
	case obj_both_libs:
		src = get_obj_both_libs(wk, src)->dynamic_lib;
	/* fallthrough */
	case obj_build_target: {
		if (!(ctx->mode & arr_to_args_build_target)) {
			goto type_err;
		}

		struct obj_build_target *tgt = get_obj_build_target(wk, src);

		SBUF(rel);
		if (ctx->mode & arr_to_args_relativize_paths) {
			path_relative_to(wk, &rel, wk->build_root, get_cstr(wk, tgt->build_path));
			str = sbuf_into_str(wk, &rel);
		} else {
			str = tgt->build_path;
		}

		break;
	}
	case obj_custom_target: {
		if (!(ctx->mode & arr_to_args_custom_target)) {
			goto type_err;
		}

		struct obj_custom_target* custom_tgt = get_obj_custom_target(wk, src);
		obj output_arr = custom_tgt->output;

		// run_target is a custom_target without an output, so we yield
		// the target's name and continue.
		if (!get_obj_type(wk, output_arr)) {
			obj_array_push(wk, ctx->res, custom_tgt->name);
			return ir_cont;
		}

		if (!obj_array_foreach(wk, output_arr, ctx, arr_to_args_iter)) {
			return ir_err;
		}
		return ir_cont;
	}
	case obj_external_program:
	case obj_python_installation:
		if (!(ctx->mode & arr_to_args_external_program)) {
			goto type_err;
		}

		obj_array_extend(wk, ctx->res, get_obj_external_program(wk, src)->cmd_array);
		return ir_cont;
	case obj_compiler:
		obj_array_extend(wk, ctx->res, get_obj_compiler(wk, src)->cmd_arr);
		return ir_cont;
	default:
type_err:
		LOG_E("cannot convert '%s' to argument", obj_type_to_s(t));
		return ir_err;
	}

	obj_array_push(wk, ctx->res, str);
	return ir_cont;
}

bool
arr_to_args(struct workspace *wk, enum arr_to_args_flags mode, obj arr, obj *res)
{
	make_obj(wk, res, obj_array);

	struct arr_to_args_ctx ctx = {
		.mode = mode,
		.res = *res
	};

	return obj_array_foreach_flat(wk, arr, &ctx, arr_to_args_iter);
}

struct join_args_argstr_ctx {
	obj str;
};

static enum iteration_result
join_args_argstr_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct join_args_argstr_ctx *ctx = _ctx;

	const struct str *s = get_str(wk, v);

	str_appn(wk, &ctx->str, s->s, s->len + 1);

	return ir_cont;
}

void
join_args_argstr(struct workspace *wk, const char **res, uint32_t *argc, obj arr)
{
	struct join_args_argstr_ctx ctx = {
		.str = make_str(wk, ""),
	};

	obj_array_foreach(wk, arr, &ctx, join_args_argstr_iter);

	*res = get_str(wk, ctx.str)->s;
	*argc = get_obj_array(wk, arr)->len;
}

struct env_to_envstr_ctx {
	obj str;
};

static enum iteration_result
env_to_envstr_dict_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	struct env_to_envstr_ctx *ctx = _ctx;

	const struct str *k = get_str(wk, key),
			 *v = get_str(wk, val);

	str_appn(wk, &ctx->str, k->s, k->len + 1);
	str_appn(wk, &ctx->str, v->s, v->len + 1);

	return ir_cont;
}

void
env_to_envstr(struct workspace *wk, const char **res, uint32_t *envc, obj val)
{
	struct env_to_envstr_ctx ctx = {
		.str = make_str(wk, ""),
	};

	obj dict;
	if (!environment_to_dict(wk, val, &dict)) {
		UNREACHABLE;
	}

	obj_dict_foreach(wk, dict, &ctx, env_to_envstr_dict_iter);

	*res = get_str(wk, ctx.str)->s;
	*envc = get_obj_dict(wk, dict)->len;
}

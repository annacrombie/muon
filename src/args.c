#include "posix.h"

#include <assert.h>
#include <string.h>

#include "args.h"
#include "buf_size.h"
#include "compilers.h"
#include "functions/build_target.h"
#include "functions/environment.h"
#include "lang/interpreter.h"
#include "lang/workspace.h"
#include "log.h"
#include "platform/path.h"

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

bool
shell_escape(char *buf, uint32_t len, const char *str)
{
	const char *need_escaping = "\"'$ \\><&#\n";
	const char *s;
	uint32_t bufi = 0;
	bool do_esc = false;

	for (s = str; *s; ++s) {
		if (strchr(need_escaping, *s)) {
			do_esc = true;
			break;
		}
	}

	if (!do_esc) {
		if (len <= strlen(str)) {
			goto trunc;
		}

		strncpy(buf, str, len);
		return true;
	}

	if (!buf_push(buf, len, &bufi, '\'')) {
		goto trunc;
	}

	for (s = str; *s; ++s) {
		if (*s == '\'') {
			if (!buf_pushs(buf, len, &bufi, "'\\''")) {
				goto trunc;
			}
		} else {
			if (!buf_push(buf, len, &bufi, *s)) {
				goto trunc;
			}
		}
	}

	if (!buf_push(buf, len, &bufi, '\'')) {
		goto trunc;
	} else if (!buf_push(buf, len, &bufi, 0)) {
		goto trunc;
	}

	return true;
trunc:
	LOG_E("shell escape truncated");
	return false;
}

static bool
simple_escape(char *buf, uint32_t len, const char *str, const char *need_escaping, char esc_char)
{
	const char *s = str;
	uint32_t bufi = 0;

	for (; *s; ++s) {
		if (strchr(need_escaping, *s)) {
			if (!buf_push(buf, len, &bufi, esc_char)) {
				goto trunc;
			}
		} else if (*s == '\n') {
			assert(false && "newlines cannot be escaped");
		}

		if (!buf_push(buf, len, &bufi, *s)) {
			goto trunc;
		}
	}

	if (!buf_push(buf, len, &bufi, 0)) {
		goto trunc;
	}
	return true;
trunc:
	LOG_E("ninja escape truncated");
	return false;
}

bool
ninja_escape(char *buf, uint32_t len, const char *str)
{
	return simple_escape(buf, len, str, " :$", '$');
}

static bool
shell_ninja_escape(char *buf, uint32_t len, const char *str)
{
	char tmp_buf[BUF_SIZE_4k];
	if (!shell_escape(tmp_buf, BUF_SIZE_4k, str)) {
		return false;
	} else if (!simple_escape(buf, len, tmp_buf, "$\n", '$')) {
		return false;
	}

	return true;
}

bool
pkgconf_escape(char *buf, uint32_t len, const char *str)
{
	return simple_escape(buf, len, str, " ", '\\');
}

typedef bool ((*escape_func)(char *buf, uint32_t len, const char *str));

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
	char buf[BUF_SIZE_4k];

	if (ctx->escape) {
		if (!ctx->escape(buf, BUF_SIZE_4k, s)) {
			return ir_err;
		}

		s = buf;
	}

	str_app(wk, *ctx->obj, s);

	if (ctx->i < ctx->len - 1) {
		str_app(wk, *ctx->obj, " ");
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

	if (!obj_array_foreach(wk, arr, &ctx, join_args_iter)) {
		assert(false);
		return 0;
	}

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
			char buf[PATH_MAX];
			if (!path_relative_to(buf, PATH_MAX, wk->build_root, get_file_path(wk, src))) {
				return ir_err;
			}
			str = make_str(wk, buf);
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

		char rel[PATH_MAX];
		if (ctx->mode & arr_to_args_relativize_paths) {
			if (!path_relative_to(rel, PATH_MAX, wk->build_root, get_cstr(wk, tgt->build_path))) {
				return false;
			}

			str = make_str(wk, rel);
		} else {
			str = tgt->build_path;
		}

		break;
	}
	case obj_custom_target: {
		if (!(ctx->mode & arr_to_args_custom_target)) {
			goto type_err;
		}

		obj output_arr = get_obj_custom_target(wk, src)->output;
		if (!obj_array_foreach(wk, output_arr, ctx, arr_to_args_iter)) {
			return ir_err;
		}
		return ir_cont;
	}
	case obj_external_program:
		if (!(ctx->mode & arr_to_args_external_program)) {
			goto type_err;
		}

		str = get_obj_external_program(wk, src)->full_path;
		break;
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

	str_appn(wk, ctx->str, s->s, s->len + 1);

	return ir_cont;
}

void
join_args_argstr(struct workspace *wk, const char **res, obj arr)
{
	struct join_args_argstr_ctx ctx = {
		.str = make_str(wk, ""),
	};

	obj_array_foreach(wk, arr, &ctx, join_args_argstr_iter);

	str_appn(wk, ctx.str, "\0", 1);
	*res = get_str(wk, ctx.str)->s;
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

	str_appn(wk, ctx->str, k->s, k->len + 1);
	str_appn(wk, ctx->str, v->s, v->len + 1);

	return ir_cont;
}

void
env_to_envstr(struct workspace *wk, const char **res, obj val)
{
	struct env_to_envstr_ctx ctx = {
		.str = make_str(wk, ""),
	};

	obj dict;

	switch (get_obj_type(wk, val)) {
	case obj_dict:
		dict = val;
		break;
	case obj_environment:
		dict = get_obj_environment(wk, val)->env;
		break;
	default:
		assert(false && "please call me with a dict or environment object");
		return;
	}

	obj_dict_foreach(wk, dict, &ctx, env_to_envstr_dict_iter);

	str_appn(wk, ctx.str, "\0", 1);
	*res = get_str(wk, ctx.str)->s;
}

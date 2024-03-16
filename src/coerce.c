/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <assert.h>
#include <inttypes.h>
#include <string.h>

#include "coerce.h"
#include "functions/environment.h"
#include "lang/typecheck.h"
#include "log.h"
#include "platform/filesystem.h"
#include "platform/path.h"

bool
coerce_environment_from_kwarg(struct workspace *wk, struct args_kw *kw, bool set_subdir, obj *res)
{
	if (kw->set) {
		if (get_obj_type(wk, kw->val) == obj_environment) {
			*res = kw->val;
		} else if (!coerce_key_value_dict(wk, kw->node, kw->val, res)) {
			return false;
		}
	} else {
		make_obj(wk, res, obj_dict);
	}

	set_default_environment_vars(wk, *res, set_subdir);
	return true;
}

struct coerce_environment_ctx {
	uint32_t err_node;
	obj res;
};

static enum iteration_result
coerce_environment_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct coerce_environment_ctx *ctx = _ctx;

	if (!typecheck(wk, ctx->err_node, val, obj_string)) {
		return false;
	}

	const struct str *ss = get_str(wk, val);
	if (str_has_null(ss)) {
		vm_error_at(wk, ctx->err_node, "environment string %o must not contain NUL", val);
		return ir_err;
	}

	const char *eql;
	if (!(eql = strchr(ss->s, '='))) {
		vm_error_at(wk, ctx->err_node, "invalid env element %o; env elements must be of the format key=value", val);
		return ir_err;
	}

	uint32_t key_len = eql - ss->s;
	obj key = make_strn(wk, ss->s, key_len);
	val = make_strn(wk, ss->s + key_len + 1, ss->len - (key_len + 1));

	obj_dict_set(wk, ctx->res, key, val);
	return ir_cont;
}

static enum iteration_result
typecheck_environment_dict_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	uint32_t err_node = *(uint32_t *)_ctx;
	const struct str *k = get_str(wk, key),
			 *v = get_str(wk, val);

	if (!k->len) {
		vm_error_at(wk, err_node, "environment key may not be an empty string (value is '%s')", v->s);
		return ir_err;
	} else if (str_has_null(k)) {
		vm_error_at(wk, err_node, "environment key may not contain NUL");
		return ir_err;
	} else if (str_has_null(v)) {
		vm_error_at(wk, err_node, "environment value may not contain NUL");
		return ir_err;
	} else if (strchr(k->s, '=')) {
		vm_error_at(wk, err_node, "environment key '%s' contains '='", k->s);
		return ir_err;
	}

	return ir_cont;
}

bool
coerce_key_value_dict(struct workspace *wk, uint32_t err_node, obj val, obj *res)
{
	make_obj(wk, res, obj_dict);

	struct coerce_environment_ctx ctx = {
		.err_node = err_node,
		.res = *res,
	};

	enum obj_type t = get_obj_type(wk, val);
	switch (t) {
	case obj_string:
		return coerce_environment_iter(wk, &ctx, val) != ir_err;
	case obj_array:
		return obj_array_foreach_flat(wk, val, &ctx, coerce_environment_iter);
	case obj_dict:
		if (!typecheck(wk, err_node, val, make_complex_type(wk, complex_type_nested, tc_dict, tc_string))) {
			return false;
		}

		*res = val;
		break;
	default:
		vm_error_at(wk, err_node, "unable to coerce type '%s' into key=value dict", obj_type_to_s(t));
		return false;
	}

	if (!obj_dict_foreach(wk, *res, &err_node, typecheck_environment_dict_iter)) {
		return false;
	}

	return true;
}

bool
coerce_include_type(struct workspace *wk, const struct str *str, uint32_t err_node, enum include_type *res)
{
	static const char *include_type_strs[] = {
		[include_type_preserve] = "preserve",
		[include_type_system] = "system",
		[include_type_non_system] = "non-system",
		0,
	};

	uint32_t i;
	for (i = 0; include_type_strs[i]; ++i) {
		if (str_eql(str, &WKSTR(include_type_strs[i]))) {
			*res = i;
			return true;
		}
	}

	vm_error_at(wk, err_node, "invalid value for include_type: %s", str->s);
	return false;
}

bool
coerce_num_to_string(struct workspace *wk, uint32_t node, obj val, obj *res)
{
	switch (get_obj_type(wk, val)) {
	case obj_number: {
		*res = make_strf(wk, "%" PRId64, get_obj_number(wk, val));
		break;
	}
	case obj_string: {
		*res = val;
		break;
	}
	default:
		vm_error_at(wk, node, "unable to coerce %o to string", val);
		return false;
	}

	return true;
}

bool
coerce_string(struct workspace *wk, uint32_t node, obj val, obj *res)
{
	switch (get_obj_type(wk, val)) {
	case obj_bool:
		if (get_obj_bool(wk, val)) {
			*res = make_str(wk, "true");
		} else {
			*res = make_str(wk, "false");
		}
		break;
	case obj_file:
		*res = *get_obj_file(wk, val);
		break;
	case obj_number: {
		*res = make_strf(wk, "%" PRId64, get_obj_number(wk, val));
		break;
	}
	case obj_string: {
		*res = val;
		break;
	}
	default:
		vm_error_at(wk, node, "unable to coerce %o to string", val);
		return false;
	}

	return true;
}

struct coerce_string_array_ctx {
	uint32_t node;
	obj arr;
};

static enum iteration_result
coerce_string_array_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct coerce_string_array_ctx *ctx = _ctx;
	obj res;

	if (!coerce_string(wk, ctx->node, val, &res)) {
		return ir_err;
	}

	obj_array_push(wk, ctx->arr, res);
	return ir_cont;
}

bool
coerce_string_array(struct workspace *wk, uint32_t node, obj arr, obj *res)
{
	make_obj(wk, res, obj_array);
	struct coerce_string_array_ctx ctx = {
		.node = node,
		.arr = *res,
	};

	return obj_array_foreach(wk, arr, &ctx, coerce_string_array_iter);
}

bool
coerce_executable(struct workspace *wk, uint32_t node, obj val, obj *res, obj *args)
{
	obj str;
	*args = 0;

	enum obj_type t = get_obj_type(wk, val);
	switch (t) {
	case obj_file:
		str = *get_obj_file(wk, val);
		break;
	case obj_both_libs:
		val = get_obj_both_libs(wk, val)->dynamic_lib;
	/* fallthrough */
	case obj_build_target: {
		struct obj_build_target *o = get_obj_build_target(wk, val);
		SBUF(dest);
		SBUF(rel);
		path_join(wk, &dest, get_cstr(wk, o->build_dir), get_cstr(wk, o->build_name));
		path_relative_to(wk, &rel, wk->build_root, dest.buf);
		path_executable(wk, &dest, rel.buf);
		str = sbuf_into_str(wk, &dest);
		break;
	}
	case obj_python_installation:
		val = get_obj_python_installation(wk, val)->prog;
		/* fallthrough */
	case obj_external_program: {
		struct obj_external_program *o = get_obj_external_program(wk, val);
		if (!o->found) {
			vm_error_at(wk, node, "a not found external_program cannot be used here");
			return ir_err;
		}

		obj_array_index(wk, o->cmd_array, 0, &str);
		if (get_obj_array(wk, o->cmd_array)->len > 1) {
			*args = obj_array_slice(wk, o->cmd_array, 1, -1);
		}
		break;
	}
	default:
		vm_error_at(wk, node, "unable to coerce '%s' into executable", obj_type_to_s(t));
		return false;
	}

	*res = str;
	return true;
}

bool
coerce_requirement(struct workspace *wk, struct args_kw *kw_required, enum requirement_type *requirement)
{
	if (kw_required->set) {
		enum obj_type t = get_obj_type(wk, kw_required->val);

		if (t == obj_bool) {
			if (get_obj_bool(wk, kw_required->val)) {
				*requirement = requirement_required;
			} else {
				*requirement = requirement_auto;
			}
		} else if (t == obj_feature_opt) {
			switch (get_obj_feature_opt(wk, kw_required->val)) {
			case feature_opt_disabled:
				*requirement = requirement_skip;
				break;
			case feature_opt_enabled:
				*requirement = requirement_required;
				break;
			case feature_opt_auto:
				*requirement = requirement_auto;
				break;
			}
		} else {
			vm_error_at(wk, kw_required->node, "expected type %s or %s, got %s",
				obj_type_to_s(obj_bool),
				obj_type_to_s(obj_feature_opt),
				obj_type_to_s(t)
				);
			return false;
		}
	} else {
		*requirement = requirement_required;
	}

	return true;
}

typedef bool (*exists_func)(const char *);

enum coerce_into_files_mode {
	mode_input,
	mode_output,
};

struct coerce_into_files_ctx {
	uint32_t node;
	obj arr;
	const char *type, *output_dir;
	exists_func exists;
	enum coerce_into_files_mode mode;
};

static enum iteration_result
coerce_custom_target_output_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct coerce_into_files_ctx *ctx = _ctx;

	obj_array_push(wk, ctx->arr, val);
	return ir_cont;
}

bool
coerce_string_to_file(struct workspace *wk, const char *dir, obj string, obj *res)
{
	const char *p = get_cstr(wk, string);
	SBUF(path);

	if (path_is_absolute(p)) {
		const struct str *ss = get_str(wk, string);
		path_copy(wk, &path, ss->s);
	} else {
		path_join(wk, &path, dir, p);
	}

	_path_normalize(wk, &path, true);

	make_obj(wk, res, obj_file);
	*get_obj_file(wk, *res) = sbuf_into_str(wk, &path);
	return true;
}

static bool
coerce_into_file(struct workspace *wk, struct coerce_into_files_ctx *ctx, obj val, obj *file)
{
	enum obj_type t = get_obj_type(wk, val);

	switch (t) {
	case obj_string: {
		SBUF(buf);

		switch (ctx->mode) {
		case mode_input:
			if (!coerce_string_to_file(wk, get_cstr(wk, current_project(wk)->cwd), val, file)) {
				return ir_err;
			}

			if (!ctx->exists(get_file_path(wk, *file))) {
				vm_error_at(wk, ctx->node, "%s %o does not exist", ctx->type, val);
				return ir_err;
			}
			break;
		case mode_output:
			if (!path_is_basename(get_cstr(wk, val))) {
				vm_error_at(wk, ctx->node, "output file '%s' contains path separators", get_cstr(wk, val));
				return ir_err;
			}

			path_join(wk, &buf, ctx->output_dir, get_cstr(wk, val));
			make_obj(wk, file, obj_file);
			*get_obj_file(wk, *file) = sbuf_into_str(wk, &buf);
			break;
		default:
			assert(false);
			return ir_err;
		}
		break;
	}
	case obj_both_libs:
		val = get_obj_both_libs(wk, val)->dynamic_lib;
	/* fallthrough */
	case obj_build_target: {
		if (ctx->mode == mode_output) {
			goto type_error;
		}

		struct obj_build_target *tgt = get_obj_build_target(wk, val);

		SBUF(path);
		path_join(wk, &path, get_cstr(wk, tgt->build_dir), get_cstr(wk, tgt->build_name));
		make_obj(wk, file, obj_file);
		*get_obj_file(wk, *file) = sbuf_into_str(wk, &path);
		break;
	}
	case obj_file:
		if (ctx->mode == mode_output) {
			goto type_error;
		}

		*file = val;
		break;
	default:
type_error:
		vm_error_at(wk, ctx->node, "unable to coerce object with type %s into %s",
			obj_type_to_s(t), ctx->type);
		return false;
	}

	return true;
}

static enum iteration_result
coerce_into_files_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct coerce_into_files_ctx *ctx = _ctx;
	enum obj_type t = get_obj_type(wk, val);

	switch (t) {
	case obj_custom_target: {
		if (ctx->mode == mode_output) {
			goto type_error;
		}

		if (!obj_array_foreach(wk, get_obj_custom_target(wk, val)->output,
			ctx, coerce_custom_target_output_iter)) {
			return ir_err;
		}
		break;
	}
	case obj_string:
	case obj_file:
	case obj_both_libs:
	case obj_build_target: {
		obj file;
		if (!coerce_into_file(wk, ctx, val, &file)) {
			return ir_err;
		}

		obj_array_push(wk, ctx->arr, file);
		break;
	}
	default:
type_error:
		vm_error_at(wk, ctx->node, "unable to coerce object with type %s into %s",
			obj_type_to_s(t), ctx->type);
		return ir_err;
	}

	return ir_cont;
}

static bool
_coerce_files(struct workspace *wk, uint32_t node, obj val, obj *res,
	const char *type_name, exists_func exists, enum coerce_into_files_mode mode,
	const char *output_dir)
{
	make_obj(wk, res, obj_array);

	struct coerce_into_files_ctx ctx = {
		.node = node,
		.arr = *res,
		.type = type_name,
		.exists = exists,
		.mode = mode,
		.output_dir = output_dir,
	};

	switch (get_obj_type(wk, val)) {
	case obj_array:
		return obj_array_foreach_flat(wk, val, &ctx, coerce_into_files_iter);
	default:
		switch (coerce_into_files_iter(wk, &ctx, val)) {
		case ir_err:
			return false;
		default:
			return true;
		}
	}
}

bool
coerce_output_files(struct workspace *wk, uint32_t node, obj val, const char *output_dir, obj *res)
{
	return _coerce_files(wk, node, val, res, "output file", NULL, mode_output, output_dir);
}

bool
coerce_files(struct workspace *wk, uint32_t node, obj val, obj *res)
{
	return _coerce_files(wk, node, val, res, "file", fs_file_exists, mode_input, 0);
}

bool
coerce_file(struct workspace *wk, uint32_t node, obj val, obj *res)
{
	struct coerce_into_files_ctx ctx = {
		.node = node,
		.arr = *res,
		.type = "file",
		.exists = fs_file_exists,
		.mode = mode_input,
	};

	return coerce_into_file(wk, &ctx, val, res);
}

bool
coerce_dirs(struct workspace *wk, uint32_t node, obj val, obj *res)
{
	return _coerce_files(wk, node, val, res, "directory", fs_dir_exists, mode_input, 0);
}

struct include_directories_iter_ctx {
	uint32_t node;
	obj res;
	bool is_system;
};

static enum iteration_result
include_directories_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct include_directories_iter_ctx *ctx = _ctx;
	enum obj_type t = get_obj_type(wk, v);

	if (t == obj_include_directory) {
		obj_array_push(wk, ctx->res, v);
		return ir_cont;
	} else if (t != obj_string) {
		vm_error_at(wk, ctx->node, "unable to coerce %o to include_directory", v);
		return ir_err;
	}

	obj path = v;
	SBUF(buf1);
	SBUF(buf2);
	const char *p = get_cstr(wk, path);

	if (!path_is_absolute(p)) {
		SBUF(abs);
		path_join(wk, &abs, get_cstr(wk, current_project(wk)->cwd), p);
		path = sbuf_into_str(wk, &abs);
	}

	p = get_cstr(wk, path);

	if (!fs_dir_exists(p)) {
		vm_error_at(wk, ctx->node, "directory '%s' does not exist", get_cstr(wk, path));
		return ir_err;
	}

	obj inc;
	struct obj_include_directory *d;

	if (path_is_subpath(wk->source_root, p)) {
		path_relative_to(wk, &buf1, wk->source_root, p);
		path_join(wk, &buf2, wk->build_root, buf1.buf);

		make_obj(wk, &inc, obj_include_directory);
		d = get_obj_include_directory(wk, inc);
		d->path = sbuf_into_str(wk, &buf2);
		d->is_system = ctx->is_system;
		obj_array_push(wk, ctx->res, inc);
	}

	make_obj(wk, &inc, obj_include_directory);
	d = get_obj_include_directory(wk, inc);
	d->path = path;
	d->is_system = ctx->is_system;
	obj_array_push(wk, ctx->res, inc);

	return ir_cont;
}

bool
coerce_include_dirs(struct workspace *wk, uint32_t node, obj val, bool is_system, obj *res)
{
	struct include_directories_iter_ctx ctx = {
		.node = node,
		.is_system = is_system,
	};

	make_obj(wk, &ctx.res, obj_array);
	if (!obj_array_foreach_flat(wk, val, &ctx, include_directories_iter)) {
		return false;
	}

	*res = ctx.res;
	return true;
}

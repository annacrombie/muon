/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: Simon Zeni <simon@bl4ckb0ne.ca>
 * SPDX-FileCopyrightText: illiliti <illiliti@dimension.sh>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "buf_size.h"
#include "error.h"
#include "lang/interpreter.h"
#include "lang/object.h"
#include "lang/parser.h"
#include "lang/typecheck.h"
#include "log.h"
#include "options.h"
#include "platform/mem.h"
#include "tracy.h"

static void *
get_obj_internal(struct workspace *wk, obj id, enum obj_type type)
{
	struct obj_internal *o = bucket_arr_get(&wk->objs, id);
	if (o->t != type) {
		LOG_E("internal type error, expected %s but got %s",
			obj_type_to_s(type), obj_type_to_s(o->t));
		abort();
		return NULL;
	}

	switch (type) {
	case obj_bool:
	case obj_file:
	case obj_feature_opt:
		return &o->val;
		break;

	case obj_number:
	case obj_string:
	case obj_array:
	case obj_dict:
	case obj_compiler:
	case obj_build_target:
	case obj_custom_target:
	case obj_subproject:
	case obj_dependency:
	case obj_external_program:
	case obj_python_installation:
	case obj_run_result:
	case obj_configuration_data:
	case obj_test:
	case obj_module:
	case obj_install_target:
	case obj_environment:
	case obj_include_directory:
	case obj_option:
	case obj_generator:
	case obj_generated_list:
	case obj_alias_target:
	case obj_both_libs:
	case obj_typeinfo:
	case obj_source_set:
	case obj_source_configuration:
	case obj_iterator:
	case obj_func:
		return bucket_arr_get(&wk->obj_aos[o->t - _obj_aos_start], o->val);

	case obj_null:
	case obj_meson:
	case obj_disabler:
	case obj_machine:
		LOG_E("tried to get singleton object of type %s",
			obj_type_to_s(type));
		abort();

	default:
		assert(false && "tried to get invalid object type");
		return NULL;
	}
}

enum obj_type
get_obj_type(struct workspace *wk, obj id)
{
	struct obj_internal *o = bucket_arr_get(&wk->objs, id);
	return o->t;
}

bool
get_obj_bool(struct workspace *wk, obj o)
{
	return *(bool *)get_obj_internal(wk, o, obj_bool);
}

int64_t
get_obj_number(struct workspace *wk, obj o)
{
	return *(int64_t *)get_obj_internal(wk, o, obj_number);
}

void
set_obj_bool(struct workspace *wk, obj o, bool v)
{
	*(bool *)get_obj_internal(wk, o, obj_bool) = v;
}

void
set_obj_number(struct workspace *wk, obj o, int64_t v)
{
	*(int64_t *)get_obj_internal(wk, o, obj_number) = v;
}

obj *
get_obj_file(struct workspace *wk, obj o)
{
	return get_obj_internal(wk, o, obj_file);
}

const char *
get_file_path(struct workspace *wk, obj o)
{
	return get_cstr(wk, *get_obj_file(wk, o));
}

const struct str *
get_str(struct workspace *wk, obj s)
{
	return get_obj_internal(wk, s, obj_string);
}

enum feature_opt_state
get_obj_feature_opt(struct workspace *wk, obj fo)
{
	enum feature_opt_state state;
	state = *(enum feature_opt_state *)get_obj_internal(wk, fo, obj_feature_opt);

	if (state == feature_opt_auto) {
		obj auto_features_opt;
		// NOTE: wk->global_opts won't be initialized if we are loading
		// a serial dump
		if (wk->global_opts
		    && get_option(wk, NULL, &WKSTR("auto_features"), &auto_features_opt)) {
			struct obj_option *opt = get_obj_option(wk, auto_features_opt);
			return *(enum feature_opt_state *)get_obj_internal(wk, opt->val, obj_feature_opt);
		} else {
			return state;
		}
	} else {
		return state;
	}
}

void
set_obj_feature_opt(struct workspace *wk, obj fo, enum feature_opt_state state)
{
	*(enum feature_opt_state *)get_obj_internal(wk, fo, obj_feature_opt)
		= state;
}

#define OBJ_GETTER(type) \
	struct type * \
	get_ ## type(struct workspace *wk, obj o) \
	{ \
		return get_obj_internal(wk, o, type); \
	}

OBJ_GETTER(obj_array)
OBJ_GETTER(obj_dict)
OBJ_GETTER(obj_compiler)
OBJ_GETTER(obj_build_target)
OBJ_GETTER(obj_custom_target)
OBJ_GETTER(obj_subproject)
OBJ_GETTER(obj_dependency)
OBJ_GETTER(obj_external_program)
OBJ_GETTER(obj_python_installation)
OBJ_GETTER(obj_run_result)
OBJ_GETTER(obj_configuration_data)
OBJ_GETTER(obj_test)
OBJ_GETTER(obj_module)
OBJ_GETTER(obj_install_target)
OBJ_GETTER(obj_environment)
OBJ_GETTER(obj_include_directory)
OBJ_GETTER(obj_option)
OBJ_GETTER(obj_generator)
OBJ_GETTER(obj_generated_list)
OBJ_GETTER(obj_alias_target)
OBJ_GETTER(obj_both_libs)
OBJ_GETTER(obj_typeinfo)
OBJ_GETTER(obj_func)
OBJ_GETTER(obj_source_set)
OBJ_GETTER(obj_source_configuration)
OBJ_GETTER(obj_iterator)

#undef OBJ_GETTER

void
make_obj(struct workspace *wk, obj *id, enum obj_type type)
{
	uint32_t val;
	*id = wk->objs.len;

	switch (type) {
	case obj_bool:
	case obj_file:
	case obj_null:
	case obj_meson:
	case obj_disabler:
	case obj_machine:
	case obj_feature_opt:
		val = 0;
		break;

	case obj_number:
	case obj_string:
	case obj_array:
	case obj_dict:
	case obj_compiler:
	case obj_build_target:
	case obj_custom_target:
	case obj_subproject:
	case obj_dependency:
	case obj_external_program:
	case obj_python_installation:
	case obj_run_result:
	case obj_configuration_data:
	case obj_test:
	case obj_module:
	case obj_install_target:
	case obj_environment:
	case obj_include_directory:
	case obj_option:
	case obj_generator:
	case obj_generated_list:
	case obj_alias_target:
	case obj_both_libs:
	case obj_source_set:
	case obj_source_configuration:
	case obj_iterator:
	case obj_typeinfo:
	case obj_func:
	{
		struct bucket_arr *ba = &wk->obj_aos[type - _obj_aos_start];
		val = ba->len;
		bucket_arr_pushn(ba, NULL, 0, 1);
		break;
	}
	default:
		assert(false && "tried to make invalid object type");
	}

	bucket_arr_push(&wk->objs, &(struct obj_internal){ .t = type, .val = val });
#ifdef TRACY_ENABLE
	if (wk->tracy.is_master_workspace) {
		uint64_t mem = 0;
		mem += bucket_arr_size(&wk->objs);
		uint32_t i;
		for (i = 0; i < obj_type_count - _obj_aos_start; ++i) {
			mem += bucket_arr_size(&wk->obj_aos[i]);
		}
	#define MB(b) ((double)b / 1048576.0)
		TracyCPlot("objects", wk->objs.len);
		TracyCPlot("object memory (mb)", MB(mem));
		TracyCPlot("string memory (mb)", MB(bucket_arr_size(&wk->chrs)));
	#undef MB
	}
#endif
}

void
obj_set_clear_mark(struct workspace *wk, struct obj_clear_mark *mk)
{
	wk->obj_clear_mark_set = true;
	mk->obji = wk->objs.len;

	bucket_arr_save(&wk->chrs, &mk->chrs);
	bucket_arr_save(&wk->objs, &mk->objs);
	uint32_t i;
	for (i = 0; i < obj_type_count - _obj_aos_start; ++i) {
		bucket_arr_save(&wk->obj_aos[i], &mk->obj_aos[i]);
	}
}

void
obj_clear(struct workspace *wk, const struct obj_clear_mark *mk)
{
	struct obj_internal *o;
	struct str *ss;
	uint32_t i;
	for (i = mk->obji; i < wk->objs.len; ++i) {
		o = bucket_arr_get(&wk->objs, i);
		if (o->t == obj_string) {
			ss = bucket_arr_get(
				&wk->obj_aos[obj_string - _obj_aos_start], o->val);

			if (ss->flags & str_flag_big) {
				z_free((void *)ss->s);
			}
		}
	}

	bucket_arr_restore(&wk->objs, &mk->objs);
	bucket_arr_restore(&wk->chrs, &mk->chrs);

	for (i = 0; i < obj_type_count - _obj_aos_start; ++i) {
		bucket_arr_restore(&wk->obj_aos[i], &mk->obj_aos[i]);
	}
}

static struct
{
	enum obj_type t;
	const char *name;
} obj_names[obj_type_count] = {
	{ .t = obj_null, .name = "void" },
	{ .t = obj_compiler, .name = "compiler" },
	{ .t = obj_dependency, .name = "dep" },
	{ .t = obj_meson, .name = "meson" },
	{ .t = obj_string, .name = "str" },
	{ .t = obj_number, .name = "int" },
	{ .t = obj_array, .name = "list" },
	{ .t = obj_dict, .name = "dict" },
	{ .t = obj_bool, .name = "bool" },
	{ .t = obj_file, .name = "file" },
	{ .t = obj_build_target, .name = "build_tgt" },
	{ .t = obj_subproject, .name = "subproject" },
	{ .t = obj_machine, .name = "build_machine" },
	{ .t = obj_feature_opt, .name = "feature" },
	{ .t = obj_external_program, .name = "external_program" },
	{ .t = obj_python_installation, .name = "python_installation" },
	{ .t = obj_run_result, .name = "runresult" },
	{ .t = obj_configuration_data, .name = "cfg_data" },
	{ .t = obj_custom_target, .name = "custom_tgt" },
	{ .t = obj_test, .name = "test" },
	{ .t = obj_module, .name = "module" },
	{ .t = obj_install_target, .name = "install_tgt" },
	{ .t = obj_environment, .name = "env" },
	{ .t = obj_include_directory, .name = "inc" },
	{ .t = obj_option, .name = "option" },
	{ .t = obj_disabler, .name = "disabler" },
	{ .t = obj_generator, .name = "generator" },
	{ .t = obj_generated_list, .name = "generated_list" },
	{ .t = obj_alias_target, .name = "alias_tgt" },
	{ .t = obj_both_libs, .name = "both_libs" },
	{ .t = obj_typeinfo, .name = "typeinfo" },
	{ .t = obj_func, .name = "func" },
	{ .t = obj_source_set, .name = "source_set" },
	{ .t = obj_source_configuration, .name = "source_configuration" },
	{ .t = obj_iterator, .name = "iterator" },
};

const char *
obj_type_to_s(enum obj_type t)
{
	uint32_t i;
	for (i = 0; i < obj_type_count; ++i) {
		if (obj_names[i].t == t) {
			return obj_names[i].name;
		}
	}

	assert(false && "unreachable");
	return NULL;
}

bool
s_to_type_tag(const char *s, type_tag *t)
{
	uint32_t i;
	for (i = 0; i < obj_type_count; ++i) {
		if (strcmp(s, obj_names[i].name) == 0) {
			*t = obj_type_to_tc_type(obj_names[i].t);
			return true;
		}
	}

	struct { type_tag t; const char *name; } extra_types[] = {
		{ .t = tc_exe, .name = "exe" },
		{ .t = tc_any, .name = "any" },
		{ .t = TYPE_TAG_LISTIFY, .name = "listify" },
		{ .t = TYPE_TAG_GLOB, .name = "glob" },
	};

	for (i = 0; i < ARRAY_LEN(extra_types); ++i) {
		if (strcmp(s, extra_types[i].name) == 0) {
			*t = extra_types[i].t;
			return true;
		}
	}

	return false;
}

struct obj_equal_iter_ctx {
	obj other_container;
	uint32_t i;
};

static enum iteration_result
obj_equal_array_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct obj_equal_iter_ctx *ctx = _ctx;
	obj r;

	obj_array_index(wk, ctx->other_container, ctx->i, &r);

	if (!obj_equal(wk, val, r)) {
		return ir_err;
	}

	++ctx->i;
	return ir_cont;
}

static enum iteration_result
obj_equal_dict_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	struct obj_equal_iter_ctx *ctx = _ctx;
	obj r;

	if (!obj_dict_index(wk, ctx->other_container, key, &r)) {
		return ir_err;
	} else if (!obj_equal(wk, val, r)) {
		return ir_err;
	}

	return ir_cont;
}

bool
obj_equal(struct workspace *wk, obj left, obj right)
{
	if (left == right) {
		return true;
	}

	enum obj_type t = get_obj_type(wk, left);
	if (t != get_obj_type(wk, right)) {
		return false;
	}

	switch (t) {
	case obj_string:
		return str_eql(get_str(wk, left), get_str(wk, right));
	case obj_file:
		return str_eql(get_str(wk, *get_obj_file(wk, left)),
			get_str(wk, *get_obj_file(wk, right)));
	case obj_number:
		return get_obj_number(wk, left) == get_obj_number(wk, right);
	case obj_bool:
		return get_obj_bool(wk, left) == get_obj_bool(wk, right);
	case obj_array: {
		struct obj_equal_iter_ctx ctx = {
			.other_container = right,
		};

		struct obj_array *l = get_obj_array(wk, left),
				 *r = get_obj_array(wk, right);

		return l->len == r->len
		       && obj_array_foreach(wk, left, &ctx, obj_equal_array_iter);
	}
	case obj_feature_opt: {
		return get_obj_feature_opt(wk, left)
		       == get_obj_feature_opt(wk, right);
	}
	case obj_include_directory: {
		struct obj_include_directory *l, *r;
		l = get_obj_include_directory(wk, left);
		r = get_obj_include_directory(wk, right);

		return l->is_system == r->is_system
		       && obj_equal(wk, l->path, r->path);
	}
	case obj_dict: {
		struct obj_equal_iter_ctx ctx = {
			.other_container = right,
		};

		struct obj_dict *l = get_obj_dict(wk, left),
				*r = get_obj_dict(wk, right);

		return l->len == r->len
		       && obj_dict_foreach(wk, left, &ctx, obj_equal_dict_iter);
	}
	default:
		/* LOG_W("TODO: compare %s", obj_type_to_s(t)); */
		return false;
	}
}

/*
 * arrays
 */

bool
obj_array_foreach(struct workspace *wk, obj arr, void *ctx, obj_array_iterator cb)
{
	struct obj_array *a = get_obj_array(wk, arr);

	if (!a->len) {
		return true;
	}

	while (true) {
		switch (cb(wk, ctx, a->val)) {
		case ir_cont:
			break;
		case ir_done:
			return true;
		case ir_err:
			return false;
		}

		if (!a->have_next) {
			break;
		}

		a = get_obj_array(wk, a->next);
	}

	return true;
}

struct obj_array_foreach_flat_ctx {
	void *usr_ctx;
	obj_array_iterator cb;
};

static enum iteration_result
obj_array_foreach_flat_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct obj_array_foreach_flat_ctx *ctx = _ctx;

	if (get_obj_type(wk, val) == obj_array) {
		if (!obj_array_foreach(wk, val, ctx, obj_array_foreach_flat_iter)) {
			return ir_err;
		} else {
			return ir_cont;
		}
	} else if (get_obj_type(wk, val) == obj_typeinfo
		   && get_obj_typeinfo(wk, val)->type == tc_array) {
		// skip typeinfo arrays as they wouldn't be yielded if they
		// were real arrays
		return ir_cont;
	} else {
		return ctx->cb(wk, ctx->usr_ctx, val);
	}

	return ir_cont;
}

bool
obj_array_foreach_flat(struct workspace *wk, obj arr, void *usr_ctx, obj_array_iterator cb)
{
	struct obj_array_foreach_flat_ctx ctx = {
		.usr_ctx = usr_ctx,
		.cb = cb,
	};

	return obj_array_foreach(wk, arr, &ctx, obj_array_foreach_flat_iter);
}

void
obj_array_push(struct workspace *wk, obj arr, obj child)
{
	obj child_arr;
	struct obj_array *a, *tail, *c;

	if (!(a = get_obj_array(wk, arr))->len) {
		a->tail = arr;
		a->len = 1;
		a->val = child;
		a->have_next = false;
		return;
	}

	make_obj(wk, &child_arr, obj_array);
	c = get_obj_array(wk, child_arr);
	c->val = child;

	a = get_obj_array(wk, arr);
	tail = get_obj_array(wk, a->tail);
	assert(!tail->have_next);

	tail->have_next = true;
	tail->next = child_arr;

	a->tail = child_arr;
	++a->len;
}

void
obj_array_prepend(struct workspace *wk, obj *arr, obj val)
{
	obj prepend;
	make_obj(wk, &prepend, obj_array);
	obj_array_push(wk, prepend, val);
	obj_array_extend_nodup(wk, prepend, *arr);
	*arr = prepend;
}

struct obj_array_index_of_iter_ctx {
	obj l;
	bool res;
	uint32_t i;
};

static enum iteration_result
obj_array_index_of_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct obj_array_index_of_iter_ctx *ctx = _ctx;

	if (obj_equal(wk, ctx->l, v)) {
		ctx->res = true;
		return ir_done;
	}

	++ctx->i;
	return ir_cont;
}

bool
obj_array_index_of(struct workspace *wk, obj arr, obj val, uint32_t *idx)
{
	struct obj_array_index_of_iter_ctx ctx = { .l = val };
	obj_array_foreach(wk, arr, &ctx, obj_array_index_of_iter);

	*idx = ctx.i;
	return ctx.res;
}

bool
obj_array_in(struct workspace *wk, obj arr, obj val)
{
	uint32_t _;
	return obj_array_index_of(wk, arr, val, &_);
}

struct obj_array_index_iter_ctx { obj res, i, tgt; };

static enum iteration_result
obj_array_index_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct obj_array_index_iter_ctx *ctx = _ctx;

	if (ctx->i == ctx->tgt) {
		ctx->res = v;
		return ir_done;
	}

	++ctx->i;
	return ir_cont;
}

void
obj_array_index(struct workspace *wk, obj arr, int64_t i, obj *res)
{
	struct obj_array_index_iter_ctx ctx = { .tgt = i };
	assert(i >= 0 && i < get_obj_array(wk, arr)->len);
	obj_array_foreach(wk, arr, &ctx, obj_array_index_iter);
	*res = ctx.res;
}

obj
obj_array_get_tail(struct workspace *wk, obj arr)
{
	return get_obj_array(wk, get_obj_array(wk, arr)->tail)->val;
}

struct obj_array_dup_ctx { obj *arr; };

static enum iteration_result
obj_array_dup_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct obj_array_dup_ctx *ctx = _ctx;
	obj_array_push(wk, *ctx->arr, v);
	return ir_cont;
}

void
obj_array_dup(struct workspace *wk, obj arr, obj *res)
{
	struct obj_array_dup_ctx ctx = { .arr = res };
	make_obj(wk, res, obj_array);
	obj_array_foreach(wk, arr, &ctx, obj_array_dup_iter);
}

void
obj_array_extend_nodup(struct workspace *wk, obj arr, obj arr2)
{
	struct obj_array *a, *b, *tail;

	if (!(b = get_obj_array(wk, arr2))->len) {
		return;
	}

	if (!(a = get_obj_array(wk, arr))->len) {
		struct obj_array_dup_ctx ctx = { .arr = &arr };
		obj_array_foreach(wk, arr2, &ctx, obj_array_dup_iter);
		return;
	}

	tail = get_obj_array(wk, a->tail);
	assert(!tail->have_next);
	tail->have_next = true;
	tail->next = arr2;

	a->tail = b->tail;
	a->len += b->len;
}

void
obj_array_extend(struct workspace *wk, obj arr, obj arr2)
{
	obj dup;
	obj_array_dup(wk, arr2, &dup);
	obj_array_extend_nodup(wk, arr, dup);
}

struct obj_array_join_ctx {
	obj *res;
	const struct str *join;
	uint32_t i, len;
};

static enum iteration_result
obj_array_join_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct obj_array_join_ctx *ctx = _ctx;

	if (!typecheck_simple_err(wk, val, obj_string)) {
		return ir_err;
	}

	const struct str *ss = get_str(wk, val);

	str_appn(wk, ctx->res, ss->s, ss->len);

	if (ctx->i < ctx->len - 1) {
		str_appn(wk, ctx->res, ctx->join->s, ctx->join->len);
	}

	++ctx->i;

	return ir_cont;
}

static enum iteration_result
obj_array_flat_len_iter(struct workspace *wk, void *_ctx, obj _)
{
	uint32_t *len = _ctx;
	++(*len);
	return ir_cont;
}

static uint32_t
obj_array_flat_len(struct workspace *wk, obj arr)
{
	uint32_t len = 0;
	obj_array_foreach_flat(wk, arr, &len, obj_array_flat_len_iter);
	return len;
}

bool
obj_array_join(struct workspace *wk, bool flat, obj arr, obj join, obj *res)
{
	*res = make_str(wk, "");

	if (!typecheck_simple_err(wk, join, obj_string)) {
		return false;
	}

	struct obj_array_join_ctx ctx = {
		.join = get_str(wk, join),
		.res = res,
	};

	if (flat) {
		ctx.len = obj_array_flat_len(wk, arr);
		return obj_array_foreach_flat(wk, arr, &ctx, obj_array_join_iter);
	} else {
		ctx.len = get_obj_array(wk, arr)->len;
		return obj_array_foreach(wk, arr, &ctx, obj_array_join_iter);
	}
}

void
obj_array_tail(struct workspace *wk, obj arr, obj *res)
{
	const struct obj_array *a = get_obj_array(wk, arr);

	if (a->have_next) {
		struct obj_array *n = get_obj_array(wk, a->next);
		n->tail = a->tail;
		n->len = a->len;
		*res = a->next;
	} else {
		// the tail of a zero or single element array is an empty array
		make_obj(wk, res, obj_array);
	}
}

void
obj_array_set(struct workspace *wk, obj arr, int64_t i, obj v)
{
	assert(i >= 0 && i < get_obj_array(wk, arr)->len);

	uint32_t j = 0;

	while (true) {
		if (j == i) {
			get_obj_array(wk, arr)->val = v;
			return;
		}

		assert(get_obj_array(wk, arr)->have_next);
		arr = get_obj_array(wk, arr)->next;
		++j;
	}

	assert(false && "unreachable");
}

void
obj_array_del(struct workspace *wk, obj arr, int64_t i)
{
	uint32_t j = 0;
	obj p = arr;
	struct obj_array *head = get_obj_array(wk, arr),
			 *prev, *next,
			 *del;

	assert(i >= 0 && i < head->len);

	if (i == 0) {
		if (head->have_next) {
			next = get_obj_array(wk, head->next);
			next->len = head->len - 1;
			next->tail = head->tail;
			*head = *next;
		} else {
			*head = (struct obj_array) { 0 };
		}

		return;
	}

	while (true) {
		if (j == i) {
			del = get_obj_array(wk, arr);
			break;
		}

		p = arr;
		assert(get_obj_array(wk, arr)->have_next);
		arr = get_obj_array(wk, arr)->next;
		++j;
	}

	prev = get_obj_array(wk, p);

	if (del->have_next) {
		prev->next = del->next;
	} else {
		head->tail = p;
		prev->have_next = false;
	}

	--head->len;
}

obj
obj_array_pop(struct workspace *wk, obj arr)
{
	obj t = obj_array_get_tail(wk, arr);
	obj_array_del(wk, arr, get_obj_array(wk, arr)->len - 1);
	return t;
}

static enum iteration_result
obj_array_dedup_iter(struct workspace *wk, void *_ctx, obj val)
{
	if (hash_get(&wk->obj_hash, &val)) {
		return ir_cont;
	}
	hash_set(&wk->obj_hash, &val, true);

	obj *res = _ctx;
	if (!obj_array_in(wk, *res, val)) {
		obj_array_push(wk, *res, val);
	}

	return ir_cont;
}

void
obj_array_dedup(struct workspace *wk, obj arr, obj *res)
{
	hash_clear(&wk->obj_hash);

	make_obj(wk, res, obj_array);
	obj_array_foreach(wk, arr, res, obj_array_dedup_iter);
}

void
obj_array_dedup_in_place(struct workspace *wk, obj *arr)
{
	if (!*arr) {
		return;
	}

	obj dedupd;
	obj_array_dedup(wk, *arr, &dedupd);
	*arr = dedupd;
}

bool
obj_array_flatten_one(struct workspace *wk, obj val, obj *res)
{
	enum obj_type t = get_obj_type(wk, val);

	if (t == obj_array) {
		struct obj_array *v = get_obj_array(wk, val);

		if (v->len == 1) {
			obj_array_index(wk, val, 0, res);
		} else {
			return false;
		}
	} else {
		*res = val;
	}

	return true;
}

int32_t
obj_array_sort_by_str(struct workspace *wk, void *_ctx, obj a, obj b)
{
	const struct str *sa = get_str(wk, a),
			 *sb = get_str(wk, b);

	uint32_t min = sa->len > sb->len ? sb->len : sa->len;

	return memcmp(sa->s, sb->s, min);
}

static enum iteration_result
obj_array_sort_push_to_da_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct arr *da = _ctx;
	arr_push(da, &v);
	return ir_cont;
}

struct obj_array_sort_ctx {
	struct workspace *wk;
	void *usr_ctx;
	obj_array_sort_func func;
};

static int32_t
obj_array_sort_wrapper(const void *a, const void *b, void *_ctx)
{
	struct obj_array_sort_ctx *ctx = _ctx;

	return ctx->func(ctx->wk, ctx->usr_ctx, *(obj *)a, *(obj *)b);
}

void
obj_array_sort(struct workspace *wk, void *usr_ctx, obj arr, obj_array_sort_func func, obj *res)
{
	uint32_t len = get_obj_array(wk, arr)->len;

	if (!len) {
		*res = arr;
		return;
	}

	struct arr da;
	arr_init(&da, len, sizeof(obj));
	obj_array_foreach(wk, arr, &da, obj_array_sort_push_to_da_iter);

	struct obj_array_sort_ctx ctx = { .wk = wk, .usr_ctx = usr_ctx, .func = func, };

	arr_sort(&da, &ctx, obj_array_sort_wrapper);

	make_obj(wk, res, obj_array);

	uint32_t i;
	for (i = 0; i < da.len; ++i) {
		obj_array_push(wk, *res, *(obj *)arr_get(&da, i));
	}

	arr_destroy(&da);
}

struct obj_array_slice_ctx {
	int64_t i, i0, i1;
	obj res;
};

static enum iteration_result
obj_array_slice_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct obj_array_slice_ctx *ctx = _ctx;

	if (ctx->i >= ctx->i0) {
		obj_array_push(wk, ctx->res, v);
	}

	++ctx->i;

	if (ctx->i > ctx->i1) {
		return ir_done;
	}

	return ir_cont;
}

obj
obj_array_slice(struct workspace *wk, obj a, int64_t i0, int64_t i1)
{
	struct obj_array *arr = get_obj_array(wk, a);
	if (!(bounds_adjust(wk, arr->len, &i0) && bounds_adjust(wk, arr->len, &i1))) {
		assert(false && "index out of bounds");
	}

	struct obj_array_slice_ctx ctx = {
		.i0 = i0,
		.i1 = i1,
	};

	make_obj(wk, &ctx.res, obj_array);

	obj_array_foreach(wk, a, &ctx, obj_array_slice_iter);

	return ctx.res;
}

/*
 * dictionaries
 */

bool
obj_dict_foreach(struct workspace *wk, obj dict, void *ctx, obj_dict_iterator cb)
{
	struct obj_dict *d = get_obj_dict(wk, dict);
	if (!d->len) {
		return true;
	}

	if (d->flags & obj_dict_flag_big) {
		uint32_t i;
		struct hash *h = bucket_arr_get(&wk->dict_hashes, d->data);
		for (i = 0; i < h->keys.len; ++i) {
			void *_key = arr_get(&h->keys, i);
			uint64_t *_val = hash_get(h, _key);
			obj key = *_val >> 32;
			obj val = *_val & 0xffffffff;

			switch (cb(wk, ctx, key, val)) {
			case ir_cont:
				break;
			case ir_done:
				return true;
			case ir_err:
				return false;
			}
		}
	} else {
		struct obj_dict_elem *e = bucket_arr_get(&wk->dict_elems, d->data);

		while (true) {
			switch (cb(wk, ctx, e->key, e->val)) {
			case ir_cont:
				break;
			case ir_done:
				return true;
			case ir_err:
				return false;
			}

			if (!e->next) {
				break;
			}
			e = bucket_arr_get(&wk->dict_elems, e->next);
		}
	}

	return true;
}

struct obj_dict_dup_ctx { obj *dict; };

static enum iteration_result
obj_dict_dup_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	struct obj_dict_dup_ctx *ctx = _ctx;
	obj_dict_set(wk, *ctx->dict, key, val);
	return ir_cont;
}

void
obj_dict_dup(struct workspace *wk, obj dict, obj *res)
{
	struct obj_dict_dup_ctx ctx = { .dict = res };
	make_obj(wk, res, obj_dict);
	obj_dict_foreach(wk, dict, &ctx, obj_dict_dup_iter);
}

static enum iteration_result
obj_dict_merge_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	obj *res = _ctx;

	obj_dict_set(wk, *res, key, val);

	return ir_cont;
}

void
obj_dict_merge_nodup(struct workspace *wk, obj dict, obj dict2)
{
	obj_dict_foreach(wk, dict2, &dict, obj_dict_merge_iter);
}

void
obj_dict_merge(struct workspace *wk, obj dict, obj dict2, obj *res)
{
	obj_dict_dup(wk, dict, res);
	obj_dict_merge_nodup(wk, *res, dict2);
}

union obj_dict_key_comparison_key {
	struct str string;
	uint32_t num;
};

/* other is marked uint32_t since it can be used to represent an obj or a number */
typedef bool ((*obj_dict_key_comparison_func)(struct workspace *wk, union obj_dict_key_comparison_key *key, uint32_t other));

static bool
obj_dict_key_comparison_func_string(struct workspace *wk, union obj_dict_key_comparison_key *key, obj other)
{
	const struct str *ss_a = get_str(wk, other);
	return str_eql(ss_a, &key->string);
}

static bool
obj_dict_key_comparison_func_int(struct workspace *wk, union obj_dict_key_comparison_key *key, uint32_t other)
{
	return key->num == other;
}

static bool
_obj_dict_index(struct workspace *wk, obj dict,
	union obj_dict_key_comparison_key *key,
	obj_dict_key_comparison_func comp,
	obj **res, uint64_t **ures)

{
	TracyCZoneAutoS;
	struct obj_dict *d = get_obj_dict(wk, dict);
	if (!d->len) {
		TracyCZoneAutoE;
		return false;
	}

	if (d->flags & obj_dict_flag_big) {
		struct hash *h = bucket_arr_get(&wk->dict_hashes, d->data);
		if (d->flags & obj_dict_flag_int_key) {
			*ures = hash_get(h, &key->num);
		} else {
			*ures = hash_get_strn(h, key->string.s, key->string.len);
		}

		if (*ures) {
			TracyCZoneAutoE;
			return true;
		}
	} else {
		struct obj_dict_elem *e = bucket_arr_get(&wk->dict_elems, d->data);

		while (true) {
			if (comp(wk, key, e->key)) {
				*res = &e->val;
				TracyCZoneAutoE;
				return true;
			}

			if (!e->next) {
				break;
			}
			e = bucket_arr_get(&wk->dict_elems, e->next);
		}
	}

	TracyCZoneAutoE;
	return false;
}

bool
obj_dict_index_strn(struct workspace *wk, obj dict, const char *str,
	uint32_t len, obj *res)
{
	uint64_t *ur = 0;
	obj *r = 0;
	union obj_dict_key_comparison_key key = {
		.string = { .s = str, .len = len, }
	};

	if (!_obj_dict_index(wk, dict, &key,
		obj_dict_key_comparison_func_string, &r, &ur)) {
		return false;
	}

	*res = r ? *r : (*ur & 0xffffffff);

	return true;
}

bool
obj_dict_index_str(struct workspace *wk, obj dict, const char *str, obj *res)
{
	return obj_dict_index_strn(wk, dict, str, strlen(str), res);
}

bool
obj_dict_index(struct workspace *wk, obj dict, obj key, obj *res)
{
	const struct str *k = get_str(wk, key);
	return obj_dict_index_strn(wk, dict, k->s, k->len, res);
}

bool
obj_dict_in(struct workspace *wk, obj dict, obj key)
{
	obj res;
	return obj_dict_index(wk, dict, key, &res);
}

static void
_obj_dict_set(struct workspace *wk, obj dict,
	union obj_dict_key_comparison_key *k,
	obj_dict_key_comparison_func comp,
	obj key, obj val)
{
	struct obj_dict *d = get_obj_dict(wk, dict);

	assert(key);

	/* empty dict */
	if (!d->len) {
		uint32_t e_idx = wk->dict_elems.len;
		bucket_arr_push(&wk->dict_elems, &(struct obj_dict_elem) { .key = key, .val = val });
		d->data = e_idx;
		d->tail = e_idx;
		++d->len;
		return;
	}

	if (!(d->flags & obj_dict_flag_dont_expand) && !(d->flags & obj_dict_flag_big) && d->len >= 15) {
		struct obj_dict_elem *e = bucket_arr_get(&wk->dict_elems, d->data);
		uint32_t h_idx = wk->dict_hashes.len;
		struct hash *h = bucket_arr_push(&wk->dict_hashes, &(struct hash) { 0 });
		if (d->flags & obj_dict_flag_int_key) {
			hash_init(h, 16, sizeof(obj));
		} else {
			hash_init_str(h, 16);
		}
		d->data = h_idx;
		d->tail = 0; // unnecessary but nice

		while (true) {
			uint64_t uv = ((uint64_t)(e->key) << 32) | e->val;

			if (d->flags & obj_dict_flag_int_key) {
				hash_set(h, &key, uv);
			} else {
				const struct str *ss = get_str(wk, e->key);
				/* LO("setting %s, %d to %ld, (%o=%o)\n", ss->s, ss->len, uv, (obj)(uv >> 32), (obj)(uv & 0xffffffff)); */
				hash_set_strn(h, ss->s, ss->len, uv);
			}

			if (!e->next) {
				break;
			}
			e = bucket_arr_get(&wk->dict_elems, e->next);
		}
		d->flags |= obj_dict_flag_big;
	}

	uint64_t *ur;
	obj *r = 0;
	if (_obj_dict_index(wk, dict, k, comp, &r, &ur)) {
		if (r) {
			*r = val;
		} else {
			*ur = ((uint64_t)key << 32) | val;
		}
		return;
	}

	/* set new value */
	if ((d->flags & obj_dict_flag_big)) {
		struct hash *h = bucket_arr_get(&wk->dict_hashes, d->data);
		if (d->flags & obj_dict_flag_int_key) {
			hash_set(h, &key, val);
		} else {
			const struct str *ss = get_str(wk, key);
			hash_set_strn(h, ss->s, ss->len, ((uint64_t)key << 32) | val);
		}
		d->len = h->len;
	} else {
		uint32_t e_idx = wk->dict_elems.len;
		bucket_arr_push(&wk->dict_elems, &(struct obj_dict_elem) { .key = key, .val = val, });

		struct obj_dict_elem *tail = bucket_arr_get(&wk->dict_elems, d->tail);
		tail->next = e_idx;

		d->tail = e_idx;
		++d->len;
	}
}

void
obj_dict_set(struct workspace *wk, obj dict, obj key, obj val)
{
	union obj_dict_key_comparison_key k = {
		.string = *get_str(wk, key),
	};
	_obj_dict_set(wk, dict, &k, obj_dict_key_comparison_func_string, key, val);
}

static void
_obj_dict_del(struct workspace *wk, obj dict,
	union obj_dict_key_comparison_key *key,
	obj_dict_key_comparison_func comp)
{
	struct obj_dict *d = get_obj_dict(wk, dict);
	if (!d->len) {
		return;
	}

	if (d->flags & obj_dict_flag_big) {
		struct hash *h = bucket_arr_get(&wk->dict_hashes, d->data);

		if (d->flags & obj_dict_flag_int_key) {
			hash_unset(h, &key->num);
		} else {
			hash_unset_strn(h, key->string.s, key->string.len);
		}

		return;
	}

	uint32_t cur_id = d->data, prev_id = 0;
	bool found = false;
	struct obj_dict_elem *prev, *e = bucket_arr_get(&wk->dict_elems, d->data);

	while (true) {
		if (comp(wk, key, e->key)) {
			found = true;
			break;
		}

		prev_id = cur_id;
		if (!e->next) {
			break;
		}
		cur_id = e->next;
		e = bucket_arr_get(&wk->dict_elems, e->next);
	}

	if (!found) {
		return;
	}

	--d->len;
	if (cur_id == d->data) {
		d->data = e->next;
	} else {
		prev = bucket_arr_get(&wk->dict_elems, prev_id);
		if (e->next) {
			prev->next = e->next;
		} else {
			d->tail = prev_id;
			prev->next = 0;
		}
	}
}

void
obj_dict_del_strn(struct workspace *wk, obj dict, const char *str, uint32_t len)
{
	union obj_dict_key_comparison_key key = { .string = { .s = str, .len = len, } };
	_obj_dict_del(wk, dict, &key, obj_dict_key_comparison_func_string);
}

void
obj_dict_del_str(struct workspace *wk, obj dict, const char *str)
{
	obj_dict_del_strn(wk, dict, str, strlen(str));
}

void
obj_dict_del(struct workspace *wk, obj dict, obj key)
{
	const struct str *k = get_str(wk, key);
	obj_dict_del_strn(wk, dict, k->s, k->len);
}

/* dict convienence functions */

void
obj_dict_seti(struct workspace *wk, obj dict, uint32_t key, obj val)
{
	union obj_dict_key_comparison_key k = { .num = key };
	_obj_dict_set(wk, dict, &k, obj_dict_key_comparison_func_int, key, val);
}

bool
obj_dict_geti(struct workspace *wk, obj dict, uint32_t key, obj *val)
{
	uint64_t *ur = 0;
	obj *r = 0;
	if (_obj_dict_index(wk, dict,
		&(union obj_dict_key_comparison_key){ .num = key },
		obj_dict_key_comparison_func_int, &r, &ur)) {
		*val = r ? *r : (*ur & 0xffffffff);
		return true;
	}

	return false;
}

/* */

struct obj_iterable_foreach_ctx {
	void *ctx;
	obj_dict_iterator cb;
};

static enum iteration_result
obj_iterable_foreach_array_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct obj_iterable_foreach_ctx *ctx = _ctx;
	return ctx->cb(wk, ctx->ctx, val, 0);
}

bool
obj_iterable_foreach(struct workspace *wk, obj dict_or_array, void *ctx, obj_dict_iterator cb)
{
	switch (get_obj_type(wk, dict_or_array)) {
	case obj_dict: {
		return obj_dict_foreach(wk, dict_or_array, ctx, cb);
	}
	case obj_array: {
		return obj_array_foreach(wk, dict_or_array, &(struct obj_iterable_foreach_ctx){
				.ctx = ctx,
				.cb = cb,
			}, obj_iterable_foreach_array_iter);
	}
	default:
		UNREACHABLE_RETURN;
	}
}

/* */

struct obj_clone_ctx {
	struct workspace *wk_dest;
	obj container;
};

static enum iteration_result
obj_clone_array_iter(struct workspace *wk_src, void *_ctx, obj val)
{
	struct obj_clone_ctx *ctx = _ctx;

	obj dest_val;

	if (!obj_clone(wk_src, ctx->wk_dest, val, &dest_val)) {
		return ir_err;
	}

	obj_array_push(ctx->wk_dest, ctx->container, dest_val);
	return ir_cont;
}

static enum iteration_result
obj_clone_dict_iter(struct workspace *wk_src, void *_ctx, obj key, obj val)
{
	struct obj_clone_ctx *ctx = _ctx;

	obj dest_key, dest_val;

	if (!obj_clone(wk_src, ctx->wk_dest, key, &dest_key)) {
		return ir_err;
	} else if (!obj_clone(wk_src, ctx->wk_dest, val, &dest_val)) {
		return ir_err;
	}

	obj_dict_set(ctx->wk_dest, ctx->container, dest_key, dest_val);
	return ir_cont;
}

bool
obj_clone(struct workspace *wk_src, struct workspace *wk_dest, obj val, obj *ret)
{
	if (val >= wk_src->objs.len) {
		LOG_E("invalid object");
		return false;
	}

	enum obj_type t = get_obj_type(wk_src, val);
	/* L("cloning %s", obj_type_to_s(t)); */

	switch (t) {
	case obj_null:
		*ret = 0;
		return true;
	case obj_number:
		make_obj(wk_dest, ret, t);
		set_obj_number(wk_dest, *ret, get_obj_number(wk_src, val));
		return true;
	case obj_bool:
		make_obj(wk_dest, ret, t);
		set_obj_bool(wk_dest, *ret, get_obj_bool(wk_src, val));
		return true;
	case obj_string: {
		*ret = str_clone(wk_src, wk_dest, val);
		return true;
	}
	case obj_file:
		make_obj(wk_dest, ret, t);
		*get_obj_file(wk_dest, *ret) = str_clone(wk_src, wk_dest, *get_obj_file(wk_src, val));
		return true;
	case obj_array:
		make_obj(wk_dest, ret, t);
		return obj_array_foreach(wk_src, val, &(struct obj_clone_ctx) {
			.container = *ret, .wk_dest = wk_dest
		}, obj_clone_array_iter);
	case obj_dict:
		make_obj(wk_dest, ret, t);
		struct obj_dict *d = get_obj_dict(wk_dest, *ret);
		d->flags |= obj_dict_flag_dont_expand;
		bool status = obj_dict_foreach(wk_src, val, &(struct obj_clone_ctx) {
			.container = *ret, .wk_dest = wk_dest
		}, obj_clone_dict_iter);
		d->flags &= ~obj_dict_flag_dont_expand;
		return status;
	case obj_test: {
		make_obj(wk_dest, ret, t);
		struct obj_test *test = get_obj_test(wk_src, val),
				*o = get_obj_test(wk_dest, *ret);
		*o = *test;

		o->name = str_clone(wk_src, wk_dest, test->name);
		o->exe = str_clone(wk_src, wk_dest, test->exe);
		if (test->workdir) {
			o->workdir = str_clone(wk_src, wk_dest, test->workdir);
		}

		if (!obj_clone(wk_src, wk_dest, test->args, &o->args)) {
			return false;
		}

		if (!obj_clone(wk_src, wk_dest, test->env, &o->env)) {
			return false;
		}

		if (!obj_clone(wk_src, wk_dest, test->suites, &o->suites)) {
			return false;
		}

		if (!obj_clone(wk_src, wk_dest, test->depends, &o->depends)) {
			return false;
		}

		if (!obj_clone(wk_src, wk_dest, test->timeout, &o->timeout)) {
			return false;
		}

		if (!obj_clone(wk_src, wk_dest, test->priority, &o->priority)) {
			return false;
		}
		return true;
	}
	case obj_install_target: {
		make_obj(wk_dest, ret, t);
		struct obj_install_target *in = get_obj_install_target(wk_src, val),
					  *o = get_obj_install_target(wk_dest, *ret);

		o->src = str_clone(wk_src, wk_dest, in->src);
		o->dest = str_clone(wk_src, wk_dest, in->dest);
		o->build_target = in->build_target;
		o->type = in->type;

		o->has_perm = in->has_perm;
		o->perm = in->perm;

		if (!obj_clone(wk_src, wk_dest, in->exclude_directories, &o->exclude_directories)) {
			return false;
		}
		if (!obj_clone(wk_src, wk_dest, in->exclude_files, &o->exclude_files)) {
			return false;
		}
		return true;
	}
	case obj_environment: {
		make_obj(wk_dest, ret, obj_environment);
		struct obj_environment *env = get_obj_environment(wk_src, val),
				       *o = get_obj_environment(wk_dest, *ret);

		if (!obj_clone(wk_src, wk_dest, env->actions, &o->actions)) {
			return false;
		}
		return true;
	}
	case obj_option: {
		make_obj(wk_dest, ret, t);
		struct obj_option *opt = get_obj_option(wk_src, val),
				  *o = get_obj_option(wk_dest, *ret);

		o->source = opt->source;
		o->type = opt->type;
		o->builtin = opt->builtin;
		o->yield = opt->yield;

		if (!obj_clone(wk_src, wk_dest, opt->name, &o->name)) {
			return false;
		}

		if (!obj_clone(wk_src, wk_dest, opt->val, &o->val)) {
			return false;
		}

		if (!obj_clone(wk_src, wk_dest, opt->choices, &o->choices)) {
			return false;
		}

		if (!obj_clone(wk_src, wk_dest, opt->max, &o->max)) {
			return false;
		}

		if (!obj_clone(wk_src, wk_dest, opt->min, &o->min)) {
			return false;
		}

		if (!obj_clone(wk_src, wk_dest, opt->deprecated, &o->deprecated)) {
			return false;
		}

		if (!obj_clone(wk_src, wk_dest, opt->description, &o->description)) {
			return false;
		}
		return true;
	}
	case obj_feature_opt: {
		make_obj(wk_dest, ret, t);

		set_obj_feature_opt(wk_dest, *ret, get_obj_feature_opt(wk_src, val));
		return true;
	}
	case obj_configuration_data: {
		make_obj(wk_dest, ret, t);
		struct obj_configuration_data *conf = get_obj_configuration_data(wk_src, val),
					      *o = get_obj_configuration_data(wk_dest, *ret);

		if (!obj_clone(wk_src, wk_dest, conf->dict, &o->dict)) {
			return false;
		}
		return true;
	}
	case obj_run_result: {
		make_obj(wk_dest, ret, t);
		struct obj_run_result *rr = get_obj_run_result(wk_src, val),
				      *o = get_obj_run_result(wk_dest, *ret);

		*o = *rr;

		if (!obj_clone(wk_src, wk_dest, rr->out, &o->out)) {
			return false;
		} else if (!obj_clone(wk_src, wk_dest, rr->err, &o->err)) {
			return false;
		}
		return true;
	}
	default:
		LOG_E("unable to clone '%s'", obj_type_to_s(t));
		return false;
	}
}

struct obj_to_s_opts {
	bool pretty;
	uint32_t indent;
};

struct obj_to_s_ctx {
	struct sbuf *sb;
	struct obj_to_s_opts *opts;
	uint32_t cont_i, cont_len;
};

static void obj_to_s_opts(struct workspace *wk, obj o, struct sbuf *sb, struct obj_to_s_opts *opts);

static void
obj_to_s_indent(struct workspace *wk, struct obj_to_s_ctx *ctx)
{
	if (!ctx->opts->pretty) {
		return;
	}

	uint32_t i;
	for (i = 0; i < ctx->opts->indent; ++i) {
		sbuf_pushs(wk, ctx->sb, "  ");
	}
}

static void
obj_to_s_pretty_newline(struct workspace *wk, struct obj_to_s_ctx *ctx)
{
	if (!ctx->opts->pretty) {
		return;
	}

	sbuf_push(wk, ctx->sb, '\n');
	obj_to_s_indent(wk, ctx);
}

static enum iteration_result
obj_to_s_array_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct obj_to_s_ctx *ctx = _ctx;

	obj_to_s_opts(wk, val, ctx->sb, ctx->opts);

	if (ctx->cont_i < ctx->cont_len - 1) {
		sbuf_pushs(wk, ctx->sb, ",");
		obj_to_s_pretty_newline(wk, ctx);
	}

	++ctx->cont_i;
	return ir_cont;
}

static enum iteration_result
obj_to_s_dict_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	struct obj_to_s_ctx *ctx = _ctx;

	obj_to_s_opts(wk, key, ctx->sb, ctx->opts);

	sbuf_pushs(wk, ctx->sb, ": ");

	obj_to_s_opts(wk, val, ctx->sb, ctx->opts);

	if (ctx->cont_i < ctx->cont_len - 1) {
		sbuf_pushs(wk, ctx->sb, ", ");
		obj_to_s_pretty_newline(wk, ctx);
	}

	++ctx->cont_i;
	return ir_cont;
}

static void
obj_to_s_str(struct workspace *wk, struct obj_to_s_ctx *ctx, obj s)
{
	sbuf_push(wk, ctx->sb, '\'');
	str_unescape(wk, ctx->sb, get_str(wk, s), true);
	sbuf_push(wk, ctx->sb, '\'');
}

static void
obj_to_s_opts(struct workspace *wk, obj o, struct sbuf *sb, struct obj_to_s_opts *opts)
{
	struct obj_to_s_ctx ctx = { .sb = sb, .opts = opts };
	enum obj_type t = get_obj_type(wk, o);

	switch (t) {
	case obj_include_directory: {
		struct obj_include_directory *inc = get_obj_include_directory(wk, o);
		sbuf_pushs(wk, sb, "<include_directory ");
		obj_to_s_str(wk, &ctx, inc->path);
		sbuf_pushs(wk, sb, ">");
		break;
	}
	case obj_dependency: {
		struct obj_dependency *dep = get_obj_dependency(wk, o);
		sbuf_pushs(wk, sb, "<dependency ");
		if (dep->name) {
			obj_to_s_str(wk, &ctx, dep->name);
		}

		sbuf_pushf(wk, sb, " | found: %s, pkgconf: %s>",
			dep->flags & dep_flag_found ? "yes" : "no",
			dep->type == dependency_type_pkgconf ? "yes" : "no"
			);
		break;
	}
	case obj_alias_target:
		sbuf_pushs(wk, sb, "<alias_target ");
		obj_to_s_str(wk, &ctx, get_obj_alias_target(wk, o)->name);
		sbuf_pushs(wk, sb, ">");
		break;
	case obj_build_target: {
		struct obj_build_target *tgt = get_obj_build_target(wk, o);
		const char *type = NULL;
		switch (tgt->type) {
		case tgt_executable:
			type = "executable";
			break;
		case tgt_static_library:
			type = "static_library";
			break;
		case tgt_dynamic_library:
			type = "shared_library";
			break;
		case tgt_shared_module:
			type = "shared_module";
			break;
		}

		sbuf_pushf(wk, sb, "<%s ", type);
		obj_to_s_str(wk, &ctx, tgt->name);
		sbuf_pushs(wk, sb, ">");

		break;
	}
	case obj_feature_opt:
		switch (get_obj_feature_opt(wk, o)) {
		case feature_opt_auto:
			sbuf_pushs(wk, sb, "'auto'");
			break;
		case feature_opt_enabled:
			sbuf_pushs(wk, sb, "'enabled'");
			break;
		case feature_opt_disabled:
			sbuf_pushs(wk, sb, "'disabled'");
			break;
		}

		break;
	case obj_test: {
		struct obj_test *test = get_obj_test(wk, o);
		sbuf_pushs(wk, sb, "test(");
		obj_to_s_str(wk, &ctx, test->name);
		sbuf_pushs(wk, sb, ", ");
		obj_to_s_str(wk, &ctx, test->exe);

		if (test->args) {
			sbuf_pushs(wk, sb, ", args: ");
			obj_to_s_opts(wk, test->args, sb, opts);
		}

		if (test->should_fail) {
			sbuf_pushs(wk, sb, ", should_fail: true");

		}

		sbuf_pushs(wk, sb, ")");
		break;
	}
	case obj_file:
		sbuf_pushs(wk, sb, "<file ");
		obj_to_s_str(wk, &ctx, *get_obj_file(wk, o));
		sbuf_pushs(wk, sb, ">");
		break;
	case obj_string: {
		obj_to_s_str(wk, &ctx, o);
		break;
	}
	case obj_number:
		sbuf_pushf(wk, sb, "%" PRId64, get_obj_number(wk, o));
		break;
	case obj_bool:
		sbuf_pushs(wk, sb, get_obj_bool(wk, o) ? "true" : "false");
		break;
	case obj_array:
		ctx.cont_len = get_obj_array(wk, o)->len;

		sbuf_pushs(wk, sb, "[");
		++opts->indent;
		obj_to_s_pretty_newline(wk, &ctx);

		obj_array_foreach(wk, o, &ctx, obj_to_s_array_iter);

		--opts->indent;
		obj_to_s_pretty_newline(wk, &ctx);
		sbuf_pushs(wk, sb, "]");
		break;
	case obj_dict:
		ctx.cont_len = get_obj_dict(wk, o)->len;

		sbuf_pushs(wk, sb, "{");
		++opts->indent;
		obj_to_s_pretty_newline(wk, &ctx);

		obj_dict_foreach(wk, o, &ctx, obj_to_s_dict_iter);

		--opts->indent;
		obj_to_s_pretty_newline(wk, &ctx);
		sbuf_pushs(wk, sb, "}");
		break;
	case obj_python_installation: {
		struct obj_python_installation *py = get_obj_python_installation(wk, o);
		sbuf_pushf(wk, sb, "<%s prog: ", obj_type_to_s(t));
		obj_to_s_opts(wk, py->prog, sb, opts);

		if (get_obj_external_program(wk, py->prog)->found) {
			sbuf_pushf(wk, sb, ", language_version: %s", get_cstr(wk, py->language_version));
			sbuf_pushs(wk, sb, ", sysconfig_paths: ");
			obj_to_s_opts(wk, py->sysconfig_paths, sb, opts);
			sbuf_pushs(wk, sb, ", sysconfig_vars: ");
			obj_to_s_opts(wk, py->sysconfig_vars, sb, opts);
		}

		sbuf_pushs(wk, sb, ">");
		break;
	}
	case obj_external_program: {
		struct obj_external_program *prog = get_obj_external_program(wk, o);
		sbuf_pushf(wk, sb, "<%s found: %s",
			obj_type_to_s(t), prog->found ? "true" : "false");

		if (prog->found) {
			sbuf_pushs(wk, sb, ", cmd_array: ");
			obj_to_s_opts(wk, prog->cmd_array, sb, opts);
		}

		sbuf_pushs(wk, sb, ">");
		break;
	}
	case obj_option: {
		struct obj_option *opt = get_obj_option(wk, o);
		sbuf_pushs(wk, sb, "<option ");

		obj_to_s_opts(wk, opt->val, sb, opts);

		sbuf_pushs(wk, sb, ">");
		break;
	}
	case obj_generated_list: {
		struct obj_generated_list *gl = get_obj_generated_list(wk, o);
		sbuf_pushs(wk, sb, "<generated_list input: ");

		obj_to_s_opts(wk, gl->input, sb, opts);

		/* sbuf_pushs(wk, sb, ", extra_args: "); */
		/* obj_to_s_opts(wk, gl->extra_arguments, sb, opts); */

		/* sbuf_pushs(wk, sb, ", preserve_path_from: "); */
		/* obj_to_s_opts(wk, gl->preserve_path_from, sb, opts); */

		sbuf_pushs(wk, sb, ">");

		break;
	}
	default:
		sbuf_pushf(wk, sb, "<obj %s>", obj_type_to_s(t));
	}
}

void
obj_to_s(struct workspace *wk, obj o, struct sbuf *sb)
{
	struct obj_to_s_opts opts = {
		.pretty = false,
	};

	obj_to_s_opts(wk, o, sb, &opts);
}

#define FMT_PARTIAL(arg) \
	if (arg_width.have && arg_prec.have) { \
		sbuf_pushf(wk, sb, fmt_buf, arg_width.val, arg_prec.val, arg); \
	} else if (arg_width.have) { \
		sbuf_pushf(wk, sb, fmt_buf, arg_width.val, arg); \
	} else if (arg_prec.have) { \
		sbuf_pushf(wk, sb, fmt_buf, arg_prec.val, arg); \
	} else { \
		sbuf_pushf(wk, sb, fmt_buf, arg); \
	}

bool
obj_vasprintf(struct workspace *wk, struct sbuf *sb, const char *fmt, va_list ap)
{
	const char *fmt_start;
	bool got_hash;

	struct {
		int val;
		bool have;
	} arg_width = { 0 }, arg_prec = { 0 };

	for (; *fmt; ++fmt) {
		if (*fmt == '%') {
			arg_width.have = false;
			arg_prec.have = false;

			got_hash = false;
			fmt_start = fmt;
			++fmt;

			// skip flags
			while (strchr("#0- +", *fmt)) {
				if (*fmt == '#') {
					got_hash = true;
				}
				++fmt;
			}

			// skip field width
			if (*fmt == '*') {
				arg_width.val = va_arg(ap, int);
				arg_width.have = true;
				++fmt;
			} else {
				while (strchr("1234567890", *fmt)) {
					++fmt;
				}
			}

			// skip precision
			if (*fmt == '.') {
				++fmt;

				if (*fmt == '*') {
					arg_prec.val = va_arg(ap, int);
					arg_prec.have = true;
					++fmt;
				} else {
					while (strchr("1234567890", *fmt)) {
						++fmt;
					}
				}
			}

			enum {
				il_norm,
				il_long,
				il_long_long,
			} int_len = il_norm;

			switch (*fmt) {
			case 'l':
				int_len = il_long;
				++fmt;
				break;
			case 'h': case 'L': case 'j': case 'z': case 't':
				assert(false && "unimplemented length modifier");
				break;
			}

			if (int_len == il_long && *fmt == 'l') {
				int_len = il_long_long;
				++fmt;
			}

			if (*fmt == 'o') {
				obj o = va_arg(ap, unsigned int);
				if (get_obj_type(wk, o) == obj_string && got_hash) {
					str_unescape(wk, sb, get_str(wk, o), false);
				} else {
					struct obj_to_s_opts opts = { 0 };

					if (got_hash) {
						opts.pretty = true;
					}

					obj_to_s_opts(wk, o, sb, &opts);
				}

				continue;
			}

			char fmt_buf[BUF_SIZE_1k + 1] = { 0 }; \
			uint32_t len = fmt - fmt_start + 1; \
			assert(len < BUF_SIZE_1k && "format specifier too long"); \
			memcpy(fmt_buf, fmt_start, len);

			switch (*fmt) {
			case 'c':
			case 'd': case 'i':
				switch (int_len) {
				case il_norm: {
					FMT_PARTIAL(va_arg(ap, int));
					break;
				}
				case il_long: {
					FMT_PARTIAL(va_arg(ap, long));
					break;
				}
				case il_long_long: {
					FMT_PARTIAL(va_arg(ap, long long));
					break;
				}
				}
				break;
			case 'u': case 'x': case 'X':
				switch (int_len) {
				case il_norm: {
					FMT_PARTIAL(va_arg(ap, unsigned int));

					break;
				}
				case il_long: {
					FMT_PARTIAL(va_arg(ap, unsigned long));
					break;
				}
				case il_long_long: {
					FMT_PARTIAL(va_arg(ap, unsigned long long));
					break;
				}
				}
				break;
			case 'e': case 'E':
			case 'f': case 'F':
			case 'g': case 'G':
			case 'a': case 'A': {
				FMT_PARTIAL(va_arg(ap, double));
				break;
			}
			case 's': {
				FMT_PARTIAL(va_arg(ap, char *));
				break;
			}
			case 'p': {
				FMT_PARTIAL(va_arg(ap, void *));
				break;
			}
			case 'n':
			case '%':
				break;
			default:
				assert(false && "unrecognized format");
				break;
			}
		} else {
			sbuf_push(wk, sb, *fmt);
		}
	}

	va_end(ap);
	return true;
}

bool
obj_asprintf(struct workspace *wk, struct sbuf *sb, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	bool ret = obj_vasprintf(wk, sb, fmt, ap);
	va_end(ap);
	return ret;
}

uint32_t
obj_vsnprintf(struct workspace *wk, char *buf, uint32_t len, const char *fmt, va_list ap)
{
	SBUF(sbuf);

	if (!obj_vasprintf(wk, &sbuf, fmt, ap)) {
		return 0;
	}
	uint32_t copy = sbuf.len > len - 1 ? len - 1 : sbuf.len;

	strncpy(buf, sbuf.buf, len - 1);
	return copy;
}

uint32_t
obj_snprintf(struct workspace *wk, char *buf, uint32_t len, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	bool ret = obj_vsnprintf(wk, buf, len, fmt, ap);
	va_end(ap);
	return ret;
}

bool
obj_vfprintf(struct workspace *wk, FILE *f, const char *fmt, va_list ap)
{
	struct sbuf sb = { .flags = sbuf_flag_write, .buf = (void *)f };

	return obj_vasprintf(wk, &sb, fmt, ap);
}

bool
obj_fprintf(struct workspace *wk, FILE *f, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	bool ret = obj_vfprintf(wk, f, fmt, ap);
	va_end(ap);
	return ret;
}

bool
obj_printf(struct workspace *wk, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	bool ret = obj_vfprintf(wk, stdout, fmt, ap);
	va_end(ap);
	return ret;
}

/*
 * inspect - obj_to_s + more detail for some objects
 */

static void
obj_inspect_dep(struct workspace *wk, FILE *out, const char *pre, struct build_dep *dep)
{
	obj_fprintf(wk, out, "%slink_language: %s\n", pre, compiler_language_to_s(dep->link_language));
	obj_fprintf(wk, out, "%slink_whole: %o\n", pre, dep->link_whole);
	obj_fprintf(wk, out, "%slink_with: %o\n", pre, dep->link_with);
	obj_fprintf(wk, out, "%slink_with_not_found: %o\n", pre, dep->link_with_not_found);
	obj_fprintf(wk, out, "%slink_args: %o\n", pre, dep->link_args);
	obj_fprintf(wk, out, "%scompile_args: %o\n", pre, dep->compile_args);
	obj_fprintf(wk, out, "%sinclude_directories: %o\n", pre, dep->include_directories);
	obj_fprintf(wk, out, "%ssources: %o\n", pre, dep->sources);
	obj_fprintf(wk, out, "%sobjects: %o\n", pre, dep->objects);
	obj_fprintf(wk, out, "%sorder_deps: %o\n", pre, dep->order_deps);
	obj_fprintf(wk, out, "%srpath: %o\n", pre, dep->rpath);
}

void
obj_inspect(struct workspace *wk, FILE *out, obj val)
{
	switch (get_obj_type(wk, val)) {
	case obj_build_target: {
		struct obj_build_target *tgt = get_obj_build_target(wk, val);

		fprintf(out, "build_target:\n");
		if (tgt->name) {
			obj_fprintf(wk, out, "    name: %o,\n", tgt->name);
		}
		obj_fprintf(wk, out, "    dep:\n");
		obj_inspect_dep(wk, out, "        ", &tgt->dep);
		obj_fprintf(wk, out, "    dep_internal:\n");
		obj_inspect_dep(wk, out, "        ", &tgt->dep_internal);
		break;
	}
	case obj_dependency: {
		struct obj_dependency *dep = get_obj_dependency(wk, val);

		fprintf(out, "dependency:\n");

		if (dep->name) {
			obj_fprintf(wk, out, "    name: %o\n", dep->name);
		}
		if (dep->version) {
			obj_fprintf(wk, out, "    version: %o\n", dep->version);
		}
		if (dep->variables) {
			obj_fprintf(wk, out, "    variables: '%o'\n", dep->variables);
		}

		obj_fprintf(wk, out, "    type: %d\n", dep->type);
		obj_fprintf(wk, out, "    dep:\n");

		obj_inspect_dep(wk, out, "        ", &dep->dep);
		break;
	}
	default:
		obj_fprintf(wk, out, "%o\n", val);
	}
}

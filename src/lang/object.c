#include "posix.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "buf_size.h"
#include "error.h"
#include "lang/interpreter.h"
#include "lang/object.h"
#include "lang/parser.h"
#include "log.h"
#include "options.h"

uint32_t
obj_type_to_tc_type(enum obj_type t)
{
	assert(t && t - 1 < tc_type_count);
	return (1 << (t - 1)) | obj_typechecking_type_tag;
}

static void *
get_obj_internal(struct workspace *wk, obj id, enum obj_type type)
{
	struct obj_internal *o = bucket_array_get(&wk->objs, id);
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
		return bucket_array_get(&wk->obj_aos[o->t - _obj_aos_start], o->val);

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
	struct obj_internal *o = bucket_array_get(&wk->objs, id);
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
	{
		struct bucket_array *ba = &wk->obj_aos[type - _obj_aos_start];
		val = ba->len;
		bucket_array_pushn(ba, NULL, 0, 1);
		break;
	}
	default:
		assert(false && "tried to make invalid object type");
	}

	bucket_array_push(&wk->objs, &(struct obj_internal){ .t = type, .val = val });
}

const char *
obj_type_to_s(enum obj_type t)
{
	switch (t) {
	case obj_null: return "null";
	case obj_compiler: return "compiler";
	case obj_dependency: return "dependency";
	case obj_meson: return "meson";
	case obj_string: return "string";
	case obj_number: return "number";
	case obj_array: return "array";
	case obj_dict: return "dict";
	case obj_bool: return "bool";
	case obj_file: return "file";
	case obj_build_target: return "build_target";
	case obj_subproject: return "subproject";
	case obj_machine: return "machine";
	case obj_feature_opt: return "feature_opt";
	case obj_external_program: return "external_program";
	case obj_run_result: return "run_result";
	case obj_configuration_data: return "configuration_data";
	case obj_custom_target: return "custom_target";
	case obj_test: return "test";
	case obj_module: return "module";
	case obj_install_target: return "install_target";
	case obj_environment: return "environment";
	case obj_include_directory: return "include_directory";
	case obj_option: return "option";
	case obj_disabler: return "disabler";
	case obj_generator: return "generator";
	case obj_generated_list: return "generated_list";
	case obj_alias_target: return "alias_target";
	case obj_both_libs: return "both_libs";
	case obj_typeinfo: return "typeinfo";

	case obj_type_count:
		assert(false); return "uh oh";
	}

	assert(false && "unreachable");
	return NULL;
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

	str_appn(wk, *ctx->res, ss->s, ss->len);

	if (ctx->i < ctx->len - 1) {
		str_appn(wk, *ctx->res, ctx->join->s, ctx->join->len);
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

void
obj_array_flatten(struct workspace *wk, obj arr, obj *res)
{
	struct obj_array_dup_ctx ctx = { .arr = res };
	make_obj(wk, res, obj_array);
	obj_array_foreach_flat(wk, arr, &ctx, obj_array_dup_iter);
}

/*
 * dictionaries
 */

bool
obj_dict_foreach(struct workspace *wk, obj dict, void *ctx, obj_dict_iterator cb)
{
	if (!get_obj_dict(wk, dict)->len) {
		return true;
	}

	while (true) {
		switch (cb(wk, ctx, get_obj_dict(wk, dict)->key, get_obj_dict(wk, dict)->val)) {
		case ir_cont:
			break;
		case ir_done:
			return true;
		case ir_err:
			return false;
		}

		if (!get_obj_dict(wk, dict)->have_next) {
			break;
		}
		dict = get_obj_dict(wk, dict)->next;
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
obj_dict_key_comparison_func_objstr(struct workspace *wk, union obj_dict_key_comparison_key *key, obj other)
{
	return str_eql(get_str(wk, key->num), get_str(wk, other));
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
	obj **res)

{
	if (!get_obj_dict(wk, dict)->len) {
		return false;
	}

	while (true) {
		if (comp(wk, key, get_obj_dict(wk, dict)->key)) {
			*res = &get_obj_dict(wk, dict)->val;
			return true;
		}

		if (!get_obj_dict(wk, dict)->have_next) {
			break;
		}
		dict = get_obj_dict(wk, dict)->next;
	}

	return false;
}

bool
obj_dict_index_strn(struct workspace *wk, obj dict, const char *str,
	uint32_t len, obj *res)
{
	obj *r;
	union obj_dict_key_comparison_key key = {
		.string = { .s = str, .len = len, }
	};

	if (!_obj_dict_index(wk, dict, &key,
		obj_dict_key_comparison_func_string, &r)) {
		return false;
	}

	*res = *r;

	return true;
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
	obj_dict_key_comparison_func comp, obj key, obj val)
{
	struct obj_dict *d; //, *tail;
	obj tail;

	/* empty dict */
	if (!(d = get_obj_dict(wk, dict))->len) {
		d->key = key;
		d->val = val;
		d->tail = dict;
		++d->len;
		return;
	}

	obj *r;
	union obj_dict_key_comparison_key k = { .num = key };
	if (_obj_dict_index(wk, dict, &k, comp, &r)) {
		*r = val;
		return;
	}

	/* set new value */
	make_obj(wk, &tail, obj_dict);
	d = get_obj_dict(wk, tail);
	d->key = key;
	d->val = val;

	d = get_obj_dict(wk, get_obj_dict(wk, dict)->tail);
	assert(!d->have_next);
	d->have_next = true;
	d->next = tail;

	d = get_obj_dict(wk, dict);

	d->tail = tail;
	++d->len;
}

void
obj_dict_set(struct workspace *wk, obj dict, obj key, obj val)
{
	_obj_dict_set(wk, dict, obj_dict_key_comparison_func_objstr, key, val);
}

/* dict convienence functions */

void
obj_dict_seti(struct workspace *wk, obj dict, uint32_t key, obj val)
{
	_obj_dict_set(wk, dict, obj_dict_key_comparison_func_int, key, val);
}

bool
obj_dict_geti(struct workspace *wk, obj dict, uint32_t key, obj *val)
{
	obj *r;
	if (_obj_dict_index(wk, dict,
		&(union obj_dict_key_comparison_key){ .num = key },
		obj_dict_key_comparison_func_int, &r)) {
		*val = *r;
		return true;
	}

	return false;
}

struct obj_dict_index_values_ctx {
	uint32_t tgt, i;
	obj *res;
};

static enum iteration_result
obj_dict_index_values_iter(struct workspace *wk_src, void *_ctx, obj _key, obj val)
{
	struct obj_dict_index_values_ctx *ctx = _ctx;
	if (ctx->i == ctx->tgt) {
		*ctx->res = val;
		return ir_done;
	}

	ctx->i += 1;
	return ir_cont;
}

void
obj_dict_index_values(struct workspace *wk, obj dict, uint32_t i, obj *res)
{
	assert(i < get_obj_dict(wk, dict)->len);
	struct obj_dict_index_values_ctx ctx = { .tgt = i, .res = res };
	obj_dict_foreach(wk, dict, &ctx, obj_dict_index_values_iter);
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
		UNREACHABLE;
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
		return obj_dict_foreach(wk_src, val, &(struct obj_clone_ctx) {
			.container = *ret, .wk_dest = wk_dest
		}, obj_clone_dict_iter);
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

		if (!obj_clone(wk_src, wk_dest, env->env, &o->env)) {
			return false;
		}
		return true;
	}
	case obj_option: {
		make_obj(wk_dest, ret, t);
		struct obj_option *opt = get_obj_option(wk_src, val),
				  *o = get_obj_option(wk_dest, *ret);

		o->type = opt->type;

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
	default:
		LOG_E("unable to clone '%s'", obj_type_to_s(t));
		return false;
	}
}

struct obj_to_s_ctx {
	char *buf;
	uint32_t i, len;
	uint32_t cont_i, cont_len;
};

static void _obj_to_s(struct workspace *wk, obj obj, char *buf, uint32_t len, uint32_t *w);

__attribute__ ((format(printf, 2, 3)))
static void
obj_to_s_buf_push(struct obj_to_s_ctx *ctx, const char *fmt, ...)
{
	if (ctx->i >= ctx->len) {
		return;
	}

	va_list ap;
	va_start(ap, fmt);

	ctx->i += vsnprintf(&ctx->buf[ctx->i], ctx->len - ctx->i, fmt, ap);
	if (ctx->i > ctx->len) {
		ctx->i = ctx->len;
	}

	va_end(ap);
}

static enum iteration_result
obj_to_s_array_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct obj_to_s_ctx *ctx = _ctx;
	uint32_t w;

	_obj_to_s(wk, val, &ctx->buf[ctx->i], ctx->len - ctx->i, &w);

	ctx->i += w;

	if (ctx->cont_i < ctx->cont_len - 1) {
		obj_to_s_buf_push(ctx, ", ");
	}

	++ctx->cont_i;
	return ir_cont;
}

static enum iteration_result
obj_to_s_dict_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	struct obj_to_s_ctx *ctx = _ctx;
	uint32_t w;

	_obj_to_s(wk, key, &ctx->buf[ctx->i], ctx->len - ctx->i, &w);
	ctx->i += w;

	obj_to_s_buf_push(ctx, ": ");

	_obj_to_s(wk, val, &ctx->buf[ctx->i], ctx->len - ctx->i, &w);
	ctx->i += w;

	if (ctx->cont_i < ctx->cont_len - 1) {
		obj_to_s_buf_push(ctx, ", ");
	}

	++ctx->cont_i;
	return ir_cont;
}
static void
obj_to_s_str(struct workspace *wk, struct obj_to_s_ctx *ctx, obj s)
{
	obj_to_s_buf_push(ctx, "'");

	uint32_t w = 0;
	if (!str_unescape(&ctx->buf[ctx->i], ctx->len - ctx->i, get_str(wk, s), &w)) {
		return;
	}
	assert(ctx->i + w <= ctx->len);
	ctx->i += w;

	obj_to_s_buf_push(ctx, "'");
	return;
}

static void
_obj_to_s(struct workspace *wk, obj obj, char *buf, uint32_t len, uint32_t *w)
{
	if (!len) {
		*w = 0;
		return;
	}

	struct obj_to_s_ctx ctx = { .buf = buf, .len = len };
	enum obj_type t = get_obj_type(wk, obj);

	switch (t) {
	case obj_include_directory: {
		struct obj_include_directory *inc = get_obj_include_directory(wk, obj);
		obj_to_s_buf_push(&ctx, "<include_directory ");
		obj_to_s_str(wk, &ctx, inc->path);
		obj_to_s_buf_push(&ctx, ">");
		break;
	}
	case obj_dependency: {
		struct obj_dependency *dep = get_obj_dependency(wk, obj);
		obj_to_s_buf_push(&ctx, "<dependency ");
		if (dep->name) {
			obj_to_s_str(wk, &ctx, dep->name);
		}

		obj_to_s_buf_push(&ctx, " | found: %s, pkgconf: %s>",
			dep->flags & dep_flag_found ? "yes" : "no",
			dep->type == dependency_type_pkgconf ? "yes" : "no"
			);
		break;
	}
	case obj_alias_target:
		obj_to_s_buf_push(&ctx, "<alias_target ");
		obj_to_s_str(wk, &ctx, get_obj_alias_target(wk, obj)->name);
		obj_to_s_buf_push(&ctx, ">");
		break;
	case obj_build_target: {
		struct obj_build_target *tgt = get_obj_build_target(wk, obj);
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

		obj_to_s_buf_push(&ctx, "<%s ", type);
		obj_to_s_str(wk, &ctx, tgt->name);
		obj_to_s_buf_push(&ctx, ">");

		break;
	}
	case obj_feature_opt:
		switch (get_obj_feature_opt(wk, obj)) {
		case feature_opt_auto:
			ctx.i += snprintf(buf, len, "'auto'");
			break;
		case feature_opt_enabled:
			ctx.i += snprintf(buf, len, "'enabled'");
			break;
		case feature_opt_disabled:
			ctx.i += snprintf(buf, len, "'disabled'");
			break;
		}

		break;
	case obj_test: {
		struct obj_test *test = get_obj_test(wk, obj);
		obj_to_s_buf_push(&ctx, "test(");
		obj_to_s_str(wk, &ctx, test->name);
		obj_to_s_buf_push(&ctx, ", ");
		obj_to_s_str(wk, &ctx, test->exe);

		if (test->args) {
			obj_to_s_buf_push(&ctx, ", args: ");

			uint32_t w;
			_obj_to_s(wk, test->args, &ctx.buf[ctx.i], ctx.len - ctx.i, &w);
			ctx.i += w;
		}

		if (test->should_fail) {
			obj_to_s_buf_push(&ctx, ", should_fail: true");

		}

		obj_to_s_buf_push(&ctx, ")");
		break;
	}
	case obj_file:
		obj_to_s_buf_push(&ctx, "<file ");
		obj_to_s_str(wk, &ctx, *get_obj_file(wk, obj));
		obj_to_s_buf_push(&ctx, ">");
		break;
	case obj_string: {
		obj_to_s_str(wk, &ctx, obj);
		break;
	}
	case obj_number:
		obj_to_s_buf_push(&ctx, "%" PRId64, get_obj_number(wk, obj));
		break;
	case obj_bool:
		obj_to_s_buf_push(&ctx, get_obj_bool(wk, obj) ? "true" : "false");
		break;
	case obj_array:
		ctx.cont_len = get_obj_array(wk, obj)->len;

		obj_to_s_buf_push(&ctx, "[");
		obj_array_foreach(wk, obj, &ctx, obj_to_s_array_iter);
		obj_to_s_buf_push(&ctx, "]");
		break;
	case obj_dict:
		ctx.cont_len = get_obj_dict(wk, obj)->len;

		obj_to_s_buf_push(&ctx, "{");
		obj_dict_foreach(wk, obj, &ctx, obj_to_s_dict_iter);
		obj_to_s_buf_push(&ctx, "}");
		break;
	case obj_external_program: {
		struct obj_external_program *prog = get_obj_external_program(wk, obj);
		obj_to_s_buf_push(&ctx, "<%s found: %s",
			obj_type_to_s(t), prog->found ? "true" : "false");

		if (prog->found) {
			obj_to_s_buf_push(&ctx, ", path: ");
			obj_to_s_str(wk, &ctx, prog->full_path);
		}

		obj_to_s_buf_push(&ctx, ">");
		break;
	}
	case obj_option: {
		struct obj_option *opt = get_obj_option(wk, obj);
		obj_to_s_buf_push(&ctx, "<option ");

		uint32_t w;
		_obj_to_s(wk, opt->val, &ctx.buf[ctx.i], ctx.len - ctx.i, &w);
		ctx.i += w;

		obj_to_s_buf_push(&ctx, ">");
		break;
	}
	default:
		obj_to_s_buf_push(&ctx, "<obj %s>", obj_type_to_s(t));
	}

	*w = ctx.i;
}

void
obj_to_s(struct workspace *wk, obj o, char *buf, uint32_t len)
{
	uint32_t w;
	_obj_to_s(wk, o, buf, len, &w);
}

bool
obj_vsnprintf(struct workspace *wk, char *out_buf, uint32_t buflen, const char *fmt, va_list ap)
{
#define CHECK_TRUNC(len) if (bufi + len > buflen) goto would_truncate

	const char *fmt_start;
	uint32_t bufi = 0, len;
	obj obj;
	bool got_object, quote_string;

	union {
		int _int;
		long _lint;
		long long _llint;
		unsigned int _uint;
		unsigned long _luint;
		unsigned long long _lluint;
		double _double;
		char *_charp;
		void *_voidp;
	} arg = { 0 };

	struct {
		int val;
		bool have;
	} arg_width, arg_prec;

	for (; *fmt; ++fmt) {
		if (*fmt == '%') {
			arg_width.have = false;
			arg_prec.have = false;

			got_object = false;
			quote_string = true;
			fmt_start = fmt;
			++fmt;

			// skip flags
			while (strchr("#0- +", *fmt)) {
				if (*fmt == '#') {
					quote_string = false;
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

			switch (*fmt) {
			case 'c':
			case 'd': case 'i':
				switch (int_len) {
				case il_norm:
					arg._int = va_arg(ap, int);
					break;
				case il_long:
					arg._lint = va_arg(ap, long);
					break;
				case il_long_long:
					arg._llint = va_arg(ap, long long);
					break;
				}
				break;
			case 'u': case 'x': case 'X':
				switch (int_len) {
				case il_norm:
					arg._uint = va_arg(ap, unsigned int);
					break;
				case il_long:
					arg._luint = va_arg(ap, unsigned long);
					break;
				case il_long_long:
					arg._lluint = va_arg(ap, unsigned long long);
					break;
				}
				break;
			case 'e': case 'E':
			case 'f': case 'F':
			case 'g': case 'G':
			case 'a': case 'A':
				arg._double = va_arg(ap, double);
				break;
			case 's':
				arg._charp = va_arg(ap, char *);
				break;
			case 'p':
				arg._voidp = va_arg(ap, void *);
				break;
			case 'n':
			case '%':
				break;
			case 'o':
				got_object = true;
				obj = va_arg(ap, unsigned int);
				break;
			default:
				assert(false && "unrecognized format");
				break;
			}

			if (got_object) {
				uint32_t w;
				if (get_obj_type(wk, obj) == obj_string && !quote_string) {
					str_unescape(&out_buf[bufi], buflen - bufi, get_str(wk, obj), &w);
				} else {
					_obj_to_s(wk, obj, &out_buf[bufi], buflen - bufi, &w);
				}

				bufi += w;

				out_buf[bufi] = 0;
			} else {
				char fmt_buf[BUF_SIZE_1k + 1] = { 0 };
				len = fmt - fmt_start + 1;
				assert(len < BUF_SIZE_1k && "format specifier too long");
				memcpy(fmt_buf, fmt_start, len);

				// There is no portable way to create a
				// va_list, so we have to enumerate all of the
				// different possibilities
				uint32_t len;
				if (arg_width.have && arg_prec.have) {
					len = snprintf(&out_buf[bufi], buflen - bufi, fmt_buf, arg_width.val, arg_prec.val, arg);
				} else if (arg_width.have) {
					len = snprintf(&out_buf[bufi], buflen - bufi, fmt_buf, arg_width.val, arg);
				} else if (arg_prec.have) {
					len = snprintf(&out_buf[bufi], buflen - bufi, fmt_buf, arg_prec.val, arg);
				} else {
					len = snprintf(&out_buf[bufi], buflen - bufi, fmt_buf, arg);
				}

				bufi += len;
			}
		} else {
			CHECK_TRUNC(1);
			out_buf[bufi] = *fmt;
			++bufi;
		}
	}

	CHECK_TRUNC(1);
	out_buf[bufi] = 0;
	++bufi;

	va_end(ap);
	return true;
would_truncate:
	va_end(ap);
	return false;
#undef CHECK_TRUNC
}

bool
obj_snprintf(struct workspace *wk, char *out_buf, uint32_t buflen, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	bool ret = obj_vsnprintf(wk, out_buf, buflen, fmt, ap);
	va_end(ap);
	return ret;
}

bool
obj_vfprintf(struct workspace *wk, FILE *f, const char *fmt, va_list ap)
{
	static char buf[BUF_SIZE_32k];
	bool ret = obj_vsnprintf(wk, buf, BUF_SIZE_32k, fmt, ap);
	fputs(buf, f);
	return ret;
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

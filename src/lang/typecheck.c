/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <inttypes.h>

#include "error.h"
#include "lang/typecheck.h"
#include "lang/workspace.h"
#include "log.h"

type_tag
obj_type_to_tc_type(enum obj_type t)
{
	if (!t) {
		return obj_typechecking_type_tag;
	}

	assert(t - 1 < tc_type_count);
	return (((type_tag)1) << (t - 1)) | obj_typechecking_type_tag;
}

type_tag
make_complex_type(struct workspace *wk, enum complex_type t, type_tag type, type_tag subtype)
{
	struct bucket_arr *typeinfo_arr = &wk->vm.objects.obj_aos[obj_typeinfo - _obj_aos_start];
	uint32_t idx = typeinfo_arr->len;
	bucket_arr_push(typeinfo_arr, &(struct obj_typeinfo){ .type = type, .subtype = subtype });
	return COMPLEX_TYPE(idx, t);
}

/*
 * ----------------------------------------------------------------------------
 *  type to typestr
 * ----------------------------------------------------------------------------
 */

static obj typechecking_type_to_str(struct workspace *wk, type_tag t);

static obj
simple_type_to_arr(struct workspace *wk, type_tag t)
{
	obj expected_types;
	make_obj(wk, &expected_types, obj_array);

	if (!(t & obj_typechecking_type_tag)) {
		t = obj_type_to_tc_type(t);
	}

	if ((t & tc_any) == tc_any) {
		obj_array_push(wk, expected_types, make_str(wk, "any"));
		t &= ~tc_any;
	} else if ((t & tc_exe) == tc_exe) {
		obj_array_push(wk, expected_types, make_str(wk, "exe"));
		t &= ~tc_exe;
	}

	type_tag ot;
	for (ot = 1; ot <= tc_type_count; ++ot) {
		type_tag tc = obj_type_to_tc_type(ot);
		if ((t & tc) != tc) {
			continue;
		}

		obj_array_push(wk, expected_types, make_str(wk, obj_type_to_s(ot)));
	}

	if (!get_obj_array(wk, expected_types)->len) {
		obj_array_push(wk, expected_types, make_str(wk, "void"));
	}

	obj sorted;
	obj_array_sort(wk, NULL, expected_types, obj_array_sort_by_str, &sorted);

	return sorted;
}

obj
typechecking_type_to_arr(struct workspace *wk, type_tag t)
{
	if (!(t & TYPE_TAG_COMPLEX)) {
		return simple_type_to_arr(wk, t);
	}

	uint32_t idx = COMPLEX_TYPE_INDEX(t);
	enum complex_type ct = COMPLEX_TYPE_TYPE(t);

	struct bucket_arr *typeinfo_arr = &wk->vm.objects.obj_aos[obj_typeinfo - _obj_aos_start];
	struct obj_typeinfo *ti = bucket_arr_get(typeinfo_arr, idx);

	obj typestr = typechecking_type_to_str(wk, ti->type);

	if (!ti->subtype) {
		obj res;
		make_obj(wk, &res, obj_array);
		obj_array_push(wk, res, typestr);
		return res;
	}

	switch (ct) {
	case complex_type_or: {
		obj subtype_arr = typechecking_type_to_arr(wk, ti->subtype);
		obj_array_push(wk, subtype_arr, typestr);
		obj sorted;
		obj_array_sort(wk, NULL, subtype_arr, obj_array_sort_by_str, &sorted);
		return sorted;
	}
	case complex_type_nested: {
		str_appf(wk, &typestr, "[%s]", typechecking_type_to_s(wk, ti->subtype));

		obj res;
		make_obj(wk, &res, obj_array);
		obj_array_push(wk, res, typestr);
		return res;
	}
	}

	UNREACHABLE_RETURN;
}

static obj
typechecking_type_to_str(struct workspace *wk, type_tag t)
{
	obj typestr;

	t &= ~TYPE_TAG_ALLOW_VOID;

	const char *modifier = 0;
	if (t & TYPE_TAG_GLOB) {
		t &= ~TYPE_TAG_GLOB;
		modifier = "glob";
	} else if ((t & TYPE_TAG_LISTIFY)) {
		t &= ~TYPE_TAG_LISTIFY;
		modifier = "listify";
	}

	obj_array_join(wk, false, typechecking_type_to_arr(wk, t), make_str(wk, "|"), &typestr);
	if (modifier) {
		typestr = make_strf(wk, "%s[%s]", modifier, get_cstr(wk, typestr));
	}

	return typestr;
}

const char *
typechecking_type_to_s(struct workspace *wk, type_tag t)
{
	return get_cstr(wk, typechecking_type_to_str(wk, t));
}

/*
 * ----------------------------------------------------------------------------
 *  obj to typestr
 * ----------------------------------------------------------------------------
 */

struct obj_type_to_typestr_ctx {
	obj arr;
};

obj obj_type_to_typestr(struct workspace *wk, obj o);

static enum iteration_result
obj_type_to_typestr_arr_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct obj_type_to_typestr_ctx *ctx = _ctx;

	obj s = obj_type_to_typestr(wk, v);

	if (!obj_array_in(wk, ctx->arr, s)) {
		obj_array_push(wk, ctx->arr, s);
	}
	return ir_cont;
}

static enum iteration_result
obj_type_to_typestr_dict_iter(struct workspace *wk, void *_ctx, obj _k, obj v)
{
	return obj_type_to_typestr_arr_iter(wk, _ctx, v);
}

obj
obj_type_to_typestr(struct workspace *wk, obj o)
{
	enum obj_type t = get_obj_type(wk, o);

	if (t == obj_typeinfo) {
		obj str = typechecking_type_to_str(wk, get_obj_typeinfo(wk, o)->type);
		return str;
	}

	obj str = make_str(wk, obj_type_to_s(t));

	switch (t) {
	case obj_dict:
	case obj_array: {
		struct obj_type_to_typestr_ctx ctx = { 0 };
		make_obj(wk, &ctx.arr, obj_array);

		if (t == obj_dict) {
			obj_dict_foreach(wk, o, &ctx, obj_type_to_typestr_dict_iter);
		} else {
			obj_array_foreach(wk, o, &ctx, obj_type_to_typestr_arr_iter);
		}

		obj sorted;
		obj_array_sort(wk, NULL, ctx.arr, obj_array_sort_by_str, &sorted);

		obj subtypestr;
		obj_array_join(wk, false, sorted, make_str(wk, "|"), &subtypestr);
		str_appf(wk, &str, "[%s]", get_cstr(wk, subtypestr));
		break;
	}
	default: break;
	}

	return str;
}

/*
 * ----------------------------------------------------------------------------
 * typechecking
 * ----------------------------------------------------------------------------
 */

struct typecheck_nested_type_ctx {
	type_tag type;
};

static bool typecheck_complex_type(struct workspace *wk, obj got_obj, type_tag got_type, type_tag type);

static enum iteration_result
typecheck_nested_type_arr_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct typecheck_nested_type_ctx *ctx = _ctx;

	if (!typecheck_complex_type(wk, v, obj_type_to_tc_type(get_obj_type(wk, v)), ctx->type)) {
		return ir_err;
	}

	return ir_cont;
}

static enum iteration_result
typecheck_nested_type_dict_iter(struct workspace *wk, void *_ctx, obj _k, obj v)
{
	return typecheck_nested_type_arr_iter(wk, _ctx, v);
}

static bool
typecheck_complex_type(struct workspace *wk, obj got_obj, type_tag got_type, type_tag type)
{
	/* L("typechecking 0%016lx (%s) vs 0%016lx (%s)", got_type, typechecking_type_to_s(wk, got_type), type, typechecking_type_to_s(wk, type)); */

	if (!(type & TYPE_TAG_COMPLEX)) {
		got_type &= ~obj_typechecking_type_tag;

		if (!got_type && ((type & TYPE_TAG_ALLOW_VOID) || !(type & ~TYPE_TAG_MASK))) {
			return true;
		}

		if (!got_type && type == tc_func) {
			assert(false);
		}

		type |= tc_disabler; // always allow disabler type
		type &= ~(obj_typechecking_type_tag | TYPE_TAG_ALLOW_VOID);

		assert(!(got_type & TYPE_TAG_MASK));
		assert(!(type & TYPE_TAG_MASK));

		return (!got_type && !type) || (got_type & type);
	}

	uint32_t idx = COMPLEX_TYPE_INDEX(type);
	enum complex_type ct = COMPLEX_TYPE_TYPE(type);

	struct bucket_arr *typeinfo_arr = &wk->vm.objects.obj_aos[obj_typeinfo - _obj_aos_start];
	struct obj_typeinfo *ti = bucket_arr_get(typeinfo_arr, idx);

	switch (ct) {
	case complex_type_or: {
		return typecheck_complex_type(wk, got_obj, got_type, ti->type)
		       || typecheck_complex_type(wk, got_obj, got_type, ti->subtype);
	}
	case complex_type_nested: {
		if (!typecheck_complex_type(wk, got_obj, got_type, ti->type)) {
			return false;
		}

		if (get_obj_type(wk, got_obj) == obj_typeinfo) {
			// currently typeinfo types don't contain nested type
			// information
			return true;
		}

		struct typecheck_nested_type_ctx ctx = {
			.type = ti->subtype,
		};

		if (ti->type == tc_array) {
			return obj_array_foreach(wk, got_obj, &ctx, typecheck_nested_type_arr_iter);
		} else if (ti->type == tc_dict) {
			return obj_dict_foreach(wk, got_obj, &ctx, typecheck_nested_type_dict_iter);
		}
	}
	}

	UNREACHABLE_RETURN;
}

bool
typecheck_custom(struct workspace *wk, uint32_t ip, obj got_obj, type_tag type, const char *fmt)
{
	type_tag got_type;

	{
		enum obj_type t = get_obj_type(wk, got_obj);
		if (t == obj_typeinfo) {
			got_type = get_obj_typeinfo(wk, got_obj)->type;
		} else {
			got_type = obj_type_to_tc_type(t);
		}
	}

	if (!(type & obj_typechecking_type_tag)) {
		type = obj_type_to_tc_type(type);
	}

	if (!typecheck_complex_type(wk, got_obj, got_type, type)) {
		if (fmt) {
			vm_error_at(wk,
				ip,
				fmt,
				typechecking_type_to_s(wk, type),
				get_cstr(wk, obj_type_to_typestr(wk, got_obj)));
		}
		return false;
	}

	return true;
}

bool
typecheck(struct workspace *wk, uint32_t ip, obj obj_id, type_tag type)
{
	return typecheck_custom(wk, ip, obj_id, type, "expected type %s, got %s");
}

bool
typecheck_simple_err(struct workspace *wk, obj o, type_tag type)
{
	type_tag got = get_obj_type(wk, o);

	if (got != type) {
		LOG_E("expected type %s, got %s", obj_type_to_s(type), obj_type_to_s(got));
		return false;
	}

	return true;
}

bool
bounds_adjust(uint32_t len, int64_t *i)
{
	if (*i < 0) {
		*i += len;
	}

	return *i < len;
}

bool
boundscheck(struct workspace *wk, uint32_t ip, uint32_t len, int64_t *i)
{
	if (!bounds_adjust(len, i)) {
		vm_error_at(wk, ip, "index %" PRId64 " out of bounds", *i);
		return false;
	}

	return true;
}

bool
rangecheck(struct workspace *wk, uint32_t ip, int64_t min, int64_t max, int64_t n)
{
	if (n < min || n > max) {
		vm_error_at(wk, ip, "number %" PRId64 " out of bounds (%" PRId64 ", %" PRId64 ")", n, min, max);
		return false;
	}

	return true;
}

/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: Harley Swick <fancycade@mycanofbeans.com>
 * SPDX-FileCopyrightText: Eli Schwartz <eschwartz@archlinux.org>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <stdlib.h>
#include <string.h>

#include "buf_size.h"
#include "coerce.h"
#include "functions/common.h"
#include "functions/string.h"
#include "guess.h"
#include "lang/typecheck.h"
#include "log.h"
#include "rpmvercmp.h"
#include "util.h"

static bool
func_strip(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string, .optional = true }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	*res = str_strip(wk, get_str(wk, rcvr), an[0].set ? get_str(wk, an[0].val) : NULL);
	return true;
}

static bool
func_to_upper(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = str_clone_mutable(wk, rcvr);
	const struct str *ss = get_str(wk, *res);

	uint32_t i;
	for (i = 0; i < ss->len; ++i) {
		if ('a' <= ss->s[i] && ss->s[i] <= 'z') {
			((char *)ss->s)[i] -= 32;
		}
	}

	return true;
}

static bool
func_to_lower(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = str_clone_mutable(wk, rcvr);

	const struct str *ss = get_str(wk, *res);

	uint32_t i;
	for (i = 0; i < ss->len; ++i) {
		if ('A' <= ss->s[i] && ss->s[i] <= 'Z') {
			((char *)ss->s)[i] += 32;
		}
	}

	return true;
}

bool
string_format(struct workspace *wk, uint32_t err_node, obj str, obj *res, void *ctx, string_format_cb cb)
{
	struct str key;
	const struct str *ss_in = get_str(wk, str);

	uint32_t i, id_start = 0, id_end = 0;
	bool reading_id = false;

	*res = make_str(wk, "");

	for (i = 0; i < ss_in->len; ++i) {
		if (ss_in->s[i] == '@') {
			if (reading_id) {
				obj elem;
				id_end = i + 1;

				if (i == id_start) {
					str_app(wk, res, "@");
					id_start = i + 1;
					reading_id = true;
					key.len = 0;
					continue;
				}

				key = (struct str){ .s = &ss_in->s[id_start], .len = i - id_start };

				switch (cb(wk, err_node, ctx, &key, &elem)) {
				case format_cb_not_found: {
					vm_error_at(wk, err_node, "key '%.*s' not found", key.len, key.s);
					return false;
				}
				case format_cb_error: return false;
				case format_cb_found: {
					obj coerced;
					if (!coerce_string(wk, err_node, elem, &coerced)) {
						return false;
					}

					const struct str *ss = get_str(wk, coerced);
					str_appn(wk, res, ss->s, ss->len);
					break;
				}
				case format_cb_skip: {
					str_app(wk, res, "@");
					i = id_start - 1;
					id_end = id_start;
					id_start = 0;
					reading_id = false;
					continue;
				}
				}

				reading_id = false;
			} else {
				if (i) {
					str_appn(wk, res, &ss_in->s[id_end], i - id_end);
				}

				id_start = i + 1;
				reading_id = true;
				key.len = 0;
			}
		} else if (ss_in->s[i] == '\\' && ss_in->s[i + 1] == '@') {
			if (i) {
				if (reading_id) {
					vm_warning_at(wk, err_node, "unclosed @ (opened at index %d)", id_start);
					str_app(wk, res, "@");
				}
				str_appn(wk, res, &ss_in->s[id_end], i - id_end);
			}
			id_end = id_start = i + 1;
			++i;
			reading_id = false;
		}
	}

	if (reading_id) {
		vm_warning_at(wk, err_node, "unclosed @ (opened at index %d)", id_start);
		str_app(wk, res, "@");
		str_appn(wk, res, &ss_in->s[id_start], i - id_start);
	} else {
		if (i > id_end) {
			str_appn(wk, res, &ss_in->s[id_end], i - id_end);
		}
	}

	return true;
}

struct func_format_ctx {
	obj arr;
};

static enum format_cb_result
func_format_cb(struct workspace *wk, uint32_t node, void *_ctx, const struct str *key, uint32_t *elem)
{
	struct func_format_ctx *ctx = _ctx;
	int64_t i;

	if (!str_to_i(key, &i, false)) {
		return format_cb_skip;
	}

	if (!boundscheck(wk, node, get_obj_array(wk, ctx->arr)->len, &i)) {
		return format_cb_error;
	}

	obj_array_index(wk, ctx->arr, i, elem);

	return format_cb_found;
}

static bool
func_format(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | tc_number | tc_bool | tc_string | tc_file }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	struct func_format_ctx ctx = {
		.arr = an[0].val,
	};

	obj str;
	if (!string_format(wk, an[0].node, rcvr, &str, &ctx, func_format_cb)) {
		return false;
	}

	*res = str;
	return true;
}

static bool
func_underscorify(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = str_clone_mutable(wk, rcvr);

	const struct str *ss = get_str(wk, *res);

	uint32_t i;
	for (i = 0; i < ss->len; ++i) {
		if (!(('a' <= ss->s[i] && ss->s[i] <= 'z') || ('A' <= ss->s[i] && ss->s[i] <= 'Z')
			    || ('0' <= ss->s[i] && ss->s[i] <= '9'))) {
			((char *)ss->s)[i] = '_';
		}
	}

	return true;
}

static bool
func_split(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string, .optional = true }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	const struct str *split = an[0].set ? get_str(wk, an[0].val) : NULL, *ss = get_str(wk, rcvr);

	*res = str_split(wk, ss, split);
	return true;
}

static bool
func_join(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { TYPE_TAG_GLOB | obj_string }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	return obj_array_join(wk, true, an[0].val, rcvr, res);
}

struct version_compare_ctx {
	bool res;
	uint32_t err_node;
	const struct str *ver1;
};

static enum iteration_result
version_compare_iter(struct workspace *wk, void *_ctx, obj s2)
{
	struct version_compare_ctx *ctx = _ctx;
	struct str ver2 = *get_str(wk, s2);
	enum op_type {
		op_ge,
		op_gt,
		op_eq,
		op_ne,
		op_le,
		op_lt,
	};
	enum op_type op = op_eq;

	struct {
		const struct str name;
		enum op_type op;
	} ops[] = {
		{
			WKSTR(">="),
			op_ge,
		},
		{
			WKSTR(">"),
			op_gt,
		},
		{
			WKSTR("=="),
			op_eq,
		},
		{
			WKSTR("!="),
			op_ne,
		},
		{
			WKSTR("<="),
			op_le,
		},
		{
			WKSTR("<"),
			op_lt,
		},
		{
			WKSTR("="),
			op_eq,
		},
	};

	uint32_t i;
	for (i = 0; i < ARRAY_LEN(ops); ++i) {
		if (str_startswith(&ver2, &ops[i].name)) {
			op = ops[i].op;
			ver2.s += ops[i].name.len;
			ver2.len -= ops[i].name.len;
			break;
		}
	}

	int8_t cmp = rpmvercmp(ctx->ver1, &ver2);

	switch (op) {
	case op_eq: ctx->res = cmp == 0; break;
	case op_ne: ctx->res = cmp != 0; break;
	case op_gt: ctx->res = cmp == 1; break;
	case op_ge: ctx->res = cmp >= 0; break;
	case op_lt: ctx->res = cmp == -1; break;
	case op_le: ctx->res = cmp <= 0; break;
	}

	if (!ctx->res) {
		return ir_done;
	}

	return ir_cont;
}

bool
version_compare(struct workspace *wk, uint32_t err_node, const struct str *ver, obj cmp_arr, bool *res)
{
	struct version_compare_ctx ctx = {
		.err_node = err_node,
		.ver1 = ver,
	};

	if (!obj_array_foreach(wk, cmp_arr, &ctx, version_compare_iter)) {
		return false;
	}

	*res = ctx.res;
	return true;
}

static bool
func_version_compare(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };

	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	struct version_compare_ctx ctx = {
		.err_node = an[0].node,
		.ver1 = get_str(wk, rcvr),
	};

	if (version_compare_iter(wk, &ctx, an[0].val) == ir_err) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, ctx.res);
	return true;
}

static bool
func_string_to_int(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	const struct str *ss = get_str(wk, rcvr);

	int64_t n;
	if (!str_to_i(ss, &n, true)) {
		vm_error_at(wk, args_node, "unable to parse %o", rcvr);
		return false;
	}

	make_obj(wk, res, obj_number);
	set_obj_number(wk, *res, n);
	return true;
}

static bool
func_string_startswith(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, str_startswith(get_str(wk, rcvr), get_str(wk, an[0].val)));
	return true;
}

static bool
func_string_endswith(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, str_endswith(get_str(wk, rcvr), get_str(wk, an[0].val)));
	return true;
}

static bool
func_string_substring(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm ao[] = { { obj_number }, { obj_number }, ARG_TYPE_NULL };
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	const struct str *s = get_str(wk, rcvr);
	int64_t start = 0, end = s->len;

	if (ao[0].set) {
		start = get_obj_number(wk, ao[0].val);
	}

	if (ao[1].set) {
		end = get_obj_number(wk, ao[1].val);
	}

	if (start < 0) {
		start = s->len + start;
	}

	if (end < 0) {
		end = s->len + end;
	}

	if (end < start) {
		end = start;
	}

	*res = make_strn(wk, &s->s[MAX(0, start)], MIN(end - start, s->len));
	return true;
}

static bool
func_string_replace(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, { obj_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	const struct str *s = get_str(wk, rcvr);
	const struct str *find = get_str(wk, an[0].val);
	const struct str *replace = get_str(wk, an[1].val);
	struct str tmp, pre = {
		.s = s->s,
		.len = 0,
	};

	*res = make_str(wk, "");

	uint32_t i;
	for (i = 0; i < s->len; ++i) {
		tmp = (struct str){
			.s = &s->s[i],
			.len = s->len - i,
		};

		if (str_startswith(&tmp, find)) {
			str_appn(wk, res, pre.s, pre.len);
			str_appn(wk, res, replace->s, replace->len);
			i += find->len;
			pre.s = &s->s[i];
			pre.len = 0;

			--i;
		} else {
			++pre.len;
		}
	}

	str_appn(wk, res, pre.s, pre.len);

	return true;
}

static bool
func_string_contains(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	const struct str *s = get_str(wk, rcvr);
	const struct str *find = get_str(wk, an[0].val);
	struct str tmp;

	bool found = false;
	uint32_t i;
	for (i = 0; i < s->len; ++i) {
		tmp = (struct str){
			.s = &s->s[i],
			.len = s->len - i,
		};

		if (str_startswith(&tmp, find)) {
			found = true;
			break;
		}
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, found);
	return true;
}

const struct func_impl impl_tbl_string[] = {
	{ "contains", func_string_contains, tc_bool, true },
	{ "endswith", func_string_endswith, tc_bool, true },
	{ "format", func_format, tc_string, true },
	{ "join", func_join, tc_string, true },
	{ "replace", func_string_replace, tc_string, true },
	{ "split", func_split, tc_array, true },
	{ "startswith", func_string_startswith, tc_bool, true },
	{ "strip", func_strip, tc_string, true },
	{ "substring", func_string_substring, tc_string, true },
	{ "to_int", func_string_to_int, tc_number, true },
	{ "to_lower", func_to_lower, tc_string, true },
	{ "to_upper", func_to_upper, tc_string, true },
	{ "underscorify", func_underscorify, tc_string, true },
	{ "version_compare", func_version_compare, tc_bool, true },
	{ NULL, NULL },
};

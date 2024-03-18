/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: Simon Zeni <simon@bl4ckb0ne.ca>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf_size.h"
#include "coerce.h"
#include "datastructures/hash.h"
#include "error.h"
#include "functions/common.h"
#include "functions/kernel.h"
#include "lang/eval.h"
#include "lang/parser.h"
#include "lang/typecheck.h"
#include "lang/workspace.h"
#include "log.h"
#include "memmem.h"
#include "platform/path.h"

static void
interp_diagnostic(struct workspace *wk, uint32_t n_id, enum log_level lvl, const char *fmt, va_list args)
{
	SBUF(buf);
	obj_vasprintf(wk, &buf, fmt, args);

	if (n_id) {
		struct node *n = get_node(wk->ast, n_id);
		error_message(wk->src, n->location, lvl, buf.buf);
	} else {
		log_print(true, lvl, "%s", buf.buf);
	}
}

void
vm_error_at(struct workspace *wk, uint32_t n_id, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	interp_diagnostic(wk, n_id, log_error, fmt, args);
	va_end(args);
}

void
vm_warning_at(struct workspace *wk, uint32_t n_id, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	interp_diagnostic(wk, n_id, log_warn, fmt, args);
	va_end(args);
}

struct check_scope_ctx {
	const char *name;
	obj res, scope;
	bool found;
};

static enum iteration_result
check_scope(struct workspace *wk, void *_ctx, obj scope)
{
	struct check_scope_ctx *ctx = _ctx;
	if (obj_dict_index_str(wk, scope, ctx->name, &ctx->res)) {
		ctx->scope = scope;
		ctx->found = true;
	}
	return ir_cont;
}

static bool
get_local_variable(struct workspace *wk, const char *name, struct project *proj, obj *res, obj *scope)
{
	struct check_scope_ctx ctx = { .name = name };
	obj_array_foreach(wk, proj->scope_stack, &ctx, check_scope);

	if (ctx.found) {
		*res = ctx.res;
		*scope = ctx.scope;
		return true;
	}

	return false;
}

bool
get_variable(struct workspace *wk, const char *name, obj *res, uint32_t proj_id)
{
	obj o, _scope;

	if (get_local_variable(wk, name, arr_get(&wk->projects, proj_id), &o, &_scope)) {
		*res = o;
		return true;
	} else {
		return false;
	}
}

static enum iteration_result
scope_stack_dup_iter(struct workspace *wk, void *_ctx, obj v)
{
	obj *r = _ctx;
	obj scope;
	obj_dict_dup(wk, v, &scope);
	obj_array_push(wk, *r, scope);
	return ir_cont;
}

obj
scope_stack_dup(struct workspace *wk, obj scope_stack)
{
	obj r;
	make_obj(wk, &r, obj_array);

	obj_array_foreach(wk, scope_stack, &r, scope_stack_dup_iter);
	return r;
}

void
push_local_scope(struct workspace *wk)
{
	obj scope;
	make_obj(wk, &scope, obj_dict);
	obj_array_push(wk, current_project(wk)->scope_stack, scope);
}

void
pop_local_scope(struct workspace *wk)
{
	obj_array_pop(wk, current_project(wk)->scope_stack);
}

void
unassign_variable(struct workspace *wk, const char *name)
{
	obj _, scope;
	if (!get_local_variable(wk, name, current_project(wk), &_, &scope)) {
		return;
	}

	obj_dict_del_str(wk, scope, name);
}

static bool interp_chained(struct workspace *wk, uint32_t node_id, obj l_id, obj *res);

static bool
interp_func(struct workspace *wk, uint32_t n_id, bool chained, obj l_id, obj *res)
{
	obj tmp = 0;
	struct node *n = get_node(wk->ast, n_id);

	bool have_rcvr = true;
	if (!chained) {
		// NOTE: This is to simulate looking up function objects for
		// builtins.
		struct node *l = get_node(wk->ast, n->l);
		if (l->type == node_id && func_lookup(kernel_func_tbl, wk->lang_mode, get_cstr(wk, l->data.str))) {
			have_rcvr = false;
			l_id = 0;
		} else {
			if (!wk->interp_node(wk, n->l, &l_id)) {
				return false;
			}
		}
	}

	if (have_rcvr && !typecheck(wk, n->l, l_id, obj_func)) {
		return false;
	}

	if (!builtin_run(wk, have_rcvr, l_id, n_id, &tmp)) {
		return false;
	}

	if (n->chflg & node_child_d) {
		return interp_chained(wk, n->d, tmp, res);
	} else {
		*res = tmp;
		return true;
	}
}

static bool
interp_method(struct workspace *wk, uint32_t node_id, obj l_id, obj *res)
{
	obj tmp = 0;
	struct node *n = get_node(wk->ast, node_id);

	if (!builtin_run(wk, true, l_id, node_id, &tmp)) {
		return false;
	}

	if (n->chflg & node_child_d) {
		return interp_chained(wk, n->d, tmp, res);
	} else {
		*res = tmp;
		return true;
	}
}

bool
interp_index(struct workspace *wk, struct node *n, obj l_id, bool do_chain, obj *res)
{
	obj r_id;
	obj tmp = 0;

	if (!wk->interp_node(wk, n->r, &r_id)) {
		return false;
	}

	type_tag t = get_obj_type(wk, l_id);

	switch (t) {
	case obj_disabler:
		*res = disabler_id;
		return true;
	case obj_array: {
		if (!typecheck(wk, n->r, r_id, obj_number)) {
			return false;
		}

		int64_t i = get_obj_number(wk, r_id);

		if (!boundscheck(wk, n->r, get_obj_array(wk, l_id)->len, &i)) {
			return false;
		}

		obj_array_index(wk, l_id, i, &tmp);
		break;
	}
	case obj_dict: {
		if (!typecheck(wk, n->r, r_id, obj_string)) {
			return false;
		}

		if (!obj_dict_index(wk, l_id, r_id, &tmp)) {
			vm_error_at(wk, n->r, "key not in dictionary: %o", r_id);
			return false;
		}
		break;
	}
	case obj_custom_target: {
		if (!typecheck(wk, n->r, r_id, obj_number)) {
			return false;
		}

		int64_t i = get_obj_number(wk, r_id);

		struct obj_custom_target *tgt = get_obj_custom_target(wk, l_id);
		struct obj_array *arr = get_obj_array(wk, tgt->output);

		if (!boundscheck(wk, n->r, arr->len, &i)) {
			return false;
		}

		obj_array_index(wk, tgt->output, i, &tmp);
		break;
	}
	case obj_string: {
		if (!typecheck(wk, n->r, r_id, obj_number)) {
			return false;
		}

		int64_t i = get_obj_number(wk, r_id);

		const struct str *s = get_str(wk, l_id);
		if (!boundscheck(wk, n->r, s->len, &i)) {
			return false;
		}

		tmp = make_strn(wk, &s->s[i], 1);
		break;
	}
	default:
		vm_error_at(wk, n->r, "index unsupported for %s", obj_type_to_s(t));
		return false;
	}

	if (do_chain && (n->chflg & node_child_d)) {
		return interp_chained(wk, n->d, tmp, res);
	} else {
		*res = tmp;
		return true;
	}
}

static bool
interp_chained(struct workspace *wk, uint32_t node_id, obj l_id, obj *res)
{
	struct node *n = get_node(wk->ast, node_id);

	switch (n->type) {
	case node_function:
		return interp_func(wk, node_id, true, l_id, res);
	case node_method:
		return interp_method(wk, node_id, l_id, res);
	case node_index:
		return interp_index(wk, n, l_id, true, res);
	default:
		UNREACHABLE;
		break;
	}

	return false;
}

static bool
interp_u_minus(struct workspace *wk, struct node *n, obj *res)
{
	obj l_id;

	if (!wk->interp_node(wk, n->l, &l_id)) {
		return false;
	} else if (l_id == disabler_id) {
		*res = disabler_id;
		return true;
	} else if (!typecheck(wk, n->l, l_id, obj_number)) {
		return false;
	}

	make_obj(wk, res, obj_number);
	set_obj_number(wk, *res, -get_obj_number(wk, l_id));
	return true;
}

bool
interp_arithmetic(struct workspace *wk, uint32_t err_node,
	enum arithmetic_type type, bool plusassign, uint32_t nl, uint32_t nr,
	obj *res)
{
	obj l_id, r_id;

	if (!wk->interp_node(wk, nl, &l_id)
	    || !wk->interp_node(wk, nr, &r_id)) {
		return false;
	}

	if (l_id == disabler_id || r_id == disabler_id) {
		*res = disabler_id;
		return true;
	}

	switch (get_obj_type(wk, l_id)) {
	case obj_string: {
		obj str;

		if (!typecheck_custom(wk, nr, r_id, obj_string, "unsupported operator for %s and %s")) {
			return false;
		}

		switch (type) {
		case arith_add:
			str = str_join(wk, l_id, r_id);
			break;
		case arith_div: {
			const struct str *ss1 = get_str(wk, l_id),
					 *ss2 = get_str(wk, r_id);

			if (str_has_null(ss1)) {
				vm_error_at(wk, nl, "%o is an invalid path", l_id);
				return false;
			}

			if (str_has_null(ss2)) {
				vm_error_at(wk, nr, "%o is an invalid path", r_id);
				return false;
			}

			SBUF(buf);
			path_join(wk, &buf, ss1->s, ss2->s);
			str = sbuf_into_str(wk, &buf);
			break;
		}
		default:
			goto err1;
		}

		*res = str;
		break;
	}
	case obj_number: {
		int64_t num, l, r;

		if (!typecheck_custom(wk, nr, r_id, obj_number, "unsupported operator for %s and %s")) {
			return false;
		}

		l = get_obj_number(wk, l_id);
		r = get_obj_number(wk, r_id);

		switch (type) {
		case arith_add:
			num = l + r;
			break;
		case arith_div:
			if (!r) {
				vm_error_at(wk, nr, "divide by 0");
				return false;
			}
			num = l / r;
			break;
		case arith_sub:
			num = l - r;
			break;
		case arith_mod:
			if (!r) {
				vm_error_at(wk, nr, "divide by 0");
				return false;
			}
			num = l % r;
			break;
		case arith_mul:
			num = l * r;
			break;
		default:
			assert(false);
			return false;
		}

		make_obj(wk, res, obj_number);
		set_obj_number(wk, *res, num);
		break;
	}
	case obj_array: {
		switch (type) {
		case arith_add:
			if (plusassign) {
				*res = l_id;
			} else {
				obj_array_dup(wk, l_id, res);
			}

			if (get_obj_type(wk, r_id) == obj_array) {
				obj_array_extend(wk, *res, r_id);
			} else {
				obj_array_push(wk, *res, r_id);
			}
			return true;
		default:
			goto err1;
		}
	}
	case obj_dict: {
		if (!typecheck_custom(wk, nr, r_id, obj_dict, "unsupported operator for %s and %s")) {
			return false;
		} else if (type != arith_add) {
			goto err1;
		}

		if (plusassign) {
			obj_dict_merge_nodup(wk, l_id, r_id);
			*res = l_id;
		} else {
			obj_dict_merge(wk, l_id, r_id, res);
		}

		break;
	}
	default:
		goto err1;
	}

	return true;
err1:
	assert(type < 5);
	vm_error_at(wk, err_node, "%s does not support %c", obj_type_to_s(get_obj_type(wk, l_id)), "+-%*/"[type]);
	return false;
}

static bool
interp_assign(struct workspace *wk, struct node *n, obj *_)
{
	obj rhs;

	if (!wk->interp_node(wk, n->r, &rhs)) {
		return false;
	}

	switch (get_obj_type(wk, rhs)) {
	case obj_environment:
	case obj_configuration_data: {
		obj cloned;
		if (!obj_clone(wk, wk, rhs, &cloned)) {
			return false;
		}

		rhs = cloned;
		break;
	}
	case obj_dict: {
		obj dup;
		obj_dict_dup(wk, rhs, &dup);
		rhs = dup;
		break;
	}
	case obj_array: {
		obj dup;
		obj_array_dup(wk, rhs, &dup);
		rhs = dup;
	}
	default:
		break;
	}

	if (!rhs) {
		vm_error_at(wk, n->l, "cannot assign variable to void");
		return false;
	}

	wk->assign_variable(wk, get_cstr(wk, get_node(wk->ast, n->l)->data.str), rhs, 0, assign_local);
	return true;
}

static bool
interp_plusassign(struct workspace *wk, uint32_t n_id, obj *_)
{
	struct node *n = get_node(wk->ast, n_id);

	obj rhs;
	if (!interp_arithmetic(wk, n_id, arith_add, true, n->l, n->r, &rhs)) {
		return false;
	}

	wk->assign_variable(wk, get_cstr(wk, get_node(wk->ast, n->l)->data.str), rhs, 0, assign_reassign);
	return true;
}

static bool
interp_array(struct workspace *wk, uint32_t n_id, obj *res)
{
	obj l, r;

	struct node *n = get_node(wk->ast, n_id);
	n->chflg |= node_visited;

	if (n->type == node_empty) {
		make_obj(wk, res, obj_array);
		struct obj_array *arr = get_obj_array(wk, *res);
		arr->len = 0;
		arr->tail = *res;
		return true;
	}

	if (n->subtype == arg_kwarg) {
		vm_error_at(wk, n->l, "kwarg not valid in array constructor");
		return false;
	}

	bool have_c = n->chflg & node_child_c && get_node(wk->ast, n->c)->type != node_empty;

	if (!wk->interp_node(wk, n->l, &l)) {
		return false;
	}

	if (have_c) {
		if (!interp_array(wk, n->c, &r)) {
			return false;
		}
	}

	make_obj(wk, res, obj_array);
	struct obj_array *arr = get_obj_array(wk, *res);
	arr->val = l;

	if ((arr->have_next = have_c)) {
		struct obj_array *arr_r = get_obj_array(wk, r);

		arr->len = arr_r->len + 1;
		arr->tail = arr_r->tail;
		arr->next = r;
	} else {
		arr->len = 1;
		arr->tail = *res;
	}

	return true;
}

static bool
interp_dict(struct workspace *wk, uint32_t n_id, obj *res)
{
	obj key, value;

	struct node *n = get_node(wk->ast, n_id);
	n->chflg |= node_visited;

	if (n->type == node_empty) {
		make_obj(wk, res, obj_dict);
		struct obj_dict *dict = get_obj_dict(wk, *res);
		dict->len = 0;
		dict->tail = *res;
		return true;
	}

	assert(n->type == node_argument);

	if (n->subtype != arg_kwarg) {
		vm_error_at(wk, n->l, "non-kwarg not valid in dict constructor");
		return false;
	}

	bool have_c = n->chflg & node_child_c && get_node(wk->ast, n->c)->type != node_empty;

	if (!wk->interp_node(wk, n->l, &key)) {
		return false;
	}

	if (!typecheck(wk, n->l, key, obj_string)) {
		return false;
	}

	if (!wk->interp_node(wk, n->r, &value)) {
		return false;
	}

	if (get_obj_type(wk, *res) == obj_typeinfo) {
		// do nothing?
	} else if (get_obj_type(wk, key) == obj_typeinfo) {
		make_obj(wk, res, obj_typeinfo);
		struct obj_typeinfo *type = get_obj_typeinfo(wk, *res);
		type->type = tc_dict;
	} else {
		if (obj_dict_in(wk, *res, key)) {
			vm_error_at(wk, n->l, "key %o is duplicated", key);
			return false;
		}

		obj_dict_set(wk, *res, key, value);
	}

	if (have_c) {
		if (!interp_dict(wk, n->c, res)) {
			return false;
		}
	}

	return true;
}

static bool
interp_not(struct workspace *wk, struct node *n, obj *res)
{
	obj obj_l_id;

	if (!wk->interp_node(wk, n->l, &obj_l_id)) {
		return false;
	} else if (obj_l_id == disabler_id) {
		*res = disabler_id;
		return true;
	} else if (!typecheck(wk, n->l, obj_l_id, obj_bool)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, !get_obj_bool(wk, obj_l_id));
	return true;
}

static bool
interp_andor(struct workspace *wk, struct node *n, obj *res)
{
	obj obj_l_id, obj_r_id;

	if (!wk->interp_node(wk, n->l, &obj_l_id)) {
		return false;
	} else if (obj_l_id == disabler_id) {
		*res = disabler_id;
		return true;
	} else if (!typecheck(wk, n->l, obj_l_id, obj_bool)) {
		return false;
	}

	bool cond = get_obj_bool(wk, obj_l_id);
	if (n->type == node_and && !cond) {
		make_obj(wk, res, obj_bool);
		set_obj_bool(wk, *res, false);
		return true;
	} else if (n->type == node_or && cond) {
		make_obj(wk, res, obj_bool);
		set_obj_bool(wk, *res, true);
		return true;
	}

	if (!wk->interp_node(wk, n->r, &obj_r_id)) {
		return false;
	} else if (obj_r_id == disabler_id) {
		*res = disabler_id;
		return true;
	} else if (!typecheck(wk, n->r, obj_r_id, obj_bool)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, get_obj_bool(wk, obj_r_id));
	return true;
}

bool
interp_comparison(struct workspace *wk, struct node *n, obj *res)
{
	bool b;
	obj obj_l_id, obj_r_id;

	if (!wk->interp_node(wk, n->l, &obj_l_id)) {
		return false;
	} else if (!wk->interp_node(wk, n->r, &obj_r_id)) {
		return false;
	}

	if (obj_l_id == disabler_id || obj_r_id == disabler_id) {
		*res = disabler_id;
		return true;
	}

	switch ((enum comparison_type)n->subtype) {
	case comp_equal:
		b = obj_equal(wk, obj_l_id, obj_r_id);
		break;
	case comp_nequal:
		b = !obj_equal(wk, obj_l_id, obj_r_id);
		break;

	case comp_in:
	case comp_not_in:
		switch (get_obj_type(wk, obj_r_id)) {
		case obj_array:
			b = obj_array_in(wk, obj_r_id, obj_l_id);
			break;
		case obj_dict:
			if (!typecheck(wk, n->l, obj_l_id, obj_string)) {
				return false;
			}

			b = obj_dict_in(wk, obj_r_id, obj_l_id);
			break;
		case obj_string:
			if (!typecheck(wk, n->l, obj_l_id, obj_string)) {
				return false;
			}

			const struct str *r = get_str(wk, obj_r_id),
					 *l = get_str(wk, obj_l_id);
			if (memmem(r->s, r->len, l->s, l->len)) {
				b = true;
			} else {
				b = false;
			}
			break;
		default:
			vm_error_at(wk, n->r, "'in' not supported for %s", obj_type_to_s(get_obj_type(wk, obj_r_id)));
			return false;
		}

		if (n->subtype == comp_not_in) {
			b = !b;
		}
		break;

	case comp_lt:
	case comp_le:
	case comp_gt:
	case comp_ge: {
		if (!typecheck(wk, n->l, obj_l_id, obj_number)
		    || !typecheck(wk, n->r, obj_r_id, obj_number)) {
			return false;
		}

		int64_t n_a = get_obj_number(wk, obj_l_id),
			n_b = get_obj_number(wk, obj_r_id);

		switch (n->subtype) {
		case comp_lt:
			b = n_a < n_b;
			break;
		case comp_le:
			b = n_a <= n_b;
			break;
		case comp_gt:
			b = n_a > n_b;
			break;
		case comp_ge:
			b = n_a >= n_b;
			break;
		default:
			UNREACHABLE;
		}
		break;
	}
	default:
		UNREACHABLE;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, b);
	return true;
}

static bool
interp_ternary(struct workspace *wk, struct node *n, obj *res)
{
	obj cond_id;
	if (!wk->interp_node(wk, n->l, &cond_id)) {
		return false;
	} else if (cond_id == disabler_id) {
		*res = disabler_id;
		return true;
	} else if (!typecheck(wk, n->l, cond_id, obj_bool)) {
		return false;
	}

	uint32_t node = get_obj_bool(wk, cond_id) ? n->r : n->c;

	return wk->interp_node(wk, node, res);
}

static bool
interp_if(struct workspace *wk, struct node *n, obj *res)
{
	bool cond;

	switch ((enum if_type)n->subtype) {
	case if_if:
	case if_elseif: {
		obj cond_id;
		if (!wk->interp_node(wk, n->l, &cond_id)) {
			return false;
		} else if (cond_id == disabler_id) {
			*res = disabler_id;
			return true;
		} else if (!typecheck(wk, n->l, cond_id, obj_bool)) {
			return false;
		}

		cond = get_obj_bool(wk, cond_id);
		break;
	}
	case if_else:
		cond = true;
		break;
	default:
		assert(false);
		return false;
	}

	if (cond) {
		if (!wk->interp_node(wk, n->r, res)) {
			return false;
		}
	} else if (n->chflg & node_child_c) {
		if (!wk->interp_node(wk, n->c, res)) {
			return false;
		}
	} else {
		*res = 0;
	}

	return true;
}

struct interp_foreach_ctx {
	const char *id1, *id2;
	uint32_t n_l, n_r;
	uint32_t block_node;
};

static enum iteration_result
interp_foreach_common(struct workspace *wk, struct interp_foreach_ctx *ctx)
{
	obj block_result;

	if (wk->dbg.stepping) {
		wk->dbg.last_line = 0;
	}

	if (get_node(wk->ast, ctx->block_node)->type == node_empty) {
		return ir_done;
	}

	if (!wk->interp_node(wk, ctx->block_node, &block_result)) {
		return ir_err;
	}

	switch (wk->loop_ctl) {
	case loop_continuing:
		wk->loop_ctl = loop_norm;
		break;
	case loop_breaking:
		wk->loop_ctl = loop_norm;
		return ir_done;
	case loop_norm:
		break;
	}

	return ir_cont;
}

static enum iteration_result
interp_foreach_dict_iter(struct workspace *wk, void *_ctx, obj k_id, obj v_id)
{
	struct interp_foreach_ctx *ctx = _ctx;

	wk->assign_variable(wk, ctx->id1, k_id, ctx->n_l, assign_local);
	wk->assign_variable(wk, ctx->id2, v_id, ctx->n_r, assign_local);

	return interp_foreach_common(wk, ctx);
}

static enum iteration_result
interp_foreach_arr_iter(struct workspace *wk, void *_ctx, obj v_id)
{
	struct interp_foreach_ctx *ctx = _ctx;

	wk->assign_variable(wk, ctx->id1, v_id, ctx->n_l, assign_local);

	return interp_foreach_common(wk, ctx);
}

static bool
interp_foreach(struct workspace *wk, struct node *n, obj *res)
{
	obj iterable;
	bool ret;

	struct node *args = get_node(wk->ast, n->l);

	struct node *stmt_node = get_node(wk->ast, n->r);
	if (!(args->chflg & node_child_r) && stmt_node->type == node_function) {
		if (str_eql(get_str(wk, get_node(wk->ast, stmt_node->l)->data.str), &WKSTR("range"))
		    && !(stmt_node->chflg & node_child_d)) {
			struct range_params range_params;
			if (!func_range_common(wk, stmt_node->r, &range_params)) {
				return false;
			}

			struct interp_foreach_ctx ctx = {
				.id1 = get_cstr(wk, get_node(wk->ast, args->l)->data.str),
				.n_l = args->l,
				.block_node = n->c,
			};


			++wk->loop_depth;
			wk->loop_ctl = loop_norm;
			uint32_t i;
			bool break_out_of_loop = false;
			ret = true;
			for (i = range_params.start; i < range_params.stop; i += range_params.step) {
				obj num;
				make_obj(wk, &num, obj_number);
				set_obj_number(wk, num, i);

				switch (interp_foreach_arr_iter(wk, &ctx, num)) {
				case ir_err:
					ret = false;
					break_out_of_loop = true;
					break;
				case ir_cont:
					break;
				case ir_done:
					break_out_of_loop = true;
					break;
				}

				if (break_out_of_loop) {
					break;
				}
			}
			--wk->loop_depth;

			return ret;
		}
	}

	if (!wk->interp_node(wk, n->r, &iterable)) {
		return false;
	}


	switch (get_obj_type(wk, iterable)) {
	case obj_array: {
		if (args->chflg & node_child_r) {
			vm_error_at(wk, n->l, "array foreach needs exactly one variable to set");
			return false;
		}

		struct interp_foreach_ctx ctx = {
			.id1 = get_cstr(wk, get_node(wk->ast, args->l)->data.str),
			.n_l = args->l,
			.block_node = n->c,
		};

		++wk->loop_depth;
		wk->loop_ctl = loop_norm;
		ret = obj_array_foreach(wk, iterable, &ctx, interp_foreach_arr_iter);
		--wk->loop_depth;

		break;
	}
	case obj_dict: {
		if (!(args->chflg & node_child_r)) {
			vm_error_at(wk, n->l, "dict foreach needs exactly two variables to set");
			return false;
		}

		assert(get_node(wk->ast, get_node(wk->ast, args->r)->type == node_foreach_args));

		struct interp_foreach_ctx ctx = {
			.id1 = get_cstr(wk, get_node(wk->ast, args->l)->data.str),
			.id2 = get_cstr(wk, get_node(wk->ast, get_node(wk->ast, args->r)->l)->data.str),
			.n_l = args->l,
			.n_r = get_node(wk->ast, args->r)->l,
			.block_node = n->c,
		};

		++wk->loop_depth;
		wk->loop_ctl = loop_norm;
		ret = obj_dict_foreach(wk, iterable, &ctx, interp_foreach_dict_iter);
		--wk->loop_depth;

		break;
	}
	default:
		vm_error_at(wk, n->r, "%s is not iterable", obj_type_to_s(get_obj_type(wk, iterable)));
		return false;
	}

	return ret;
}

static bool
interp_func_def(struct workspace *wk, struct node *n, obj *res)
{
	make_obj(wk, res, obj_func);
	struct obj_func *f = get_obj_func(wk, *res);
	f->src = make_str(wk, wk->src->label);
	f->args_id = n->r;
	f->block_id = n->c;
	f->ast = wk->ast;
	f->lang_mode = wk->lang_mode;
	f->scope_stack = wk->scope_stack_dup(wk, current_project(wk)->scope_stack);
	f->return_type = n->data.type;

	struct node *arg;
	uint32_t arg_id = n->r;
	while (true) {
		arg = get_node(wk->ast, arg_id);
		if (arg->type == node_empty) {
			break;
		}

		assert(arg->type == node_argument); // TODO: delete
		if (arg->subtype == arg_normal) {
			if (f->nkwargs) {
				vm_error_at(wk, arg_id, "non-kwarg after kwargs");
			}
			++f->nargs;
		} else if (arg->subtype == arg_kwarg) {
			if (!f->nkwargs) {
				make_obj(wk, &f->kwarg_defaults, obj_array);
			}
			++f->nkwargs;

			obj r;
			if (!wk->interp_node(wk, arg->r, &r)) {
				return false;
			}

			obj_array_push(wk, f->kwarg_defaults, r);
		}

		if (!(arg->chflg & node_child_c)) {
			break;
		}
		arg_id = arg->c;
	}

	f->name = get_cstr(wk, get_node(wk->ast, n->l)->data.str);
	wk->assign_variable(wk, f->name, *res, n->l, assign_local);
	return true;
}

bool
interp_stringify(struct workspace *wk, struct node *n, obj *res)
{
	obj l_id;

	if (!wk->interp_node(wk, n->l, &l_id)) {
		return false;
	}

	if (!coerce_string(wk, n->l, l_id, res)) {
		return false;
	}

	return true;
}

bool
interp_node(struct workspace *wk, uint32_t n_id, obj *res)
{
	bool ret = false;
	*res = 0;

	struct node *n = get_node(wk->ast, n_id);
	n->chflg |= node_visited; // for analyzer

	/* L("%d:%s", n_id, node_to_s(n)); */
	if (wk->subdir_done || wk->loop_ctl || wk->returning) {
		return true;
	}

	switch (n->type) {
	/* literals */
	case node_bool:
	case node_string:
	case node_number:
		*res = n->l;
		ret = true;
		break;
	case node_array:
		ret = interp_array(wk, n->l, res);
		break;
	case node_dict:
		make_obj(wk, res, obj_dict);
		ret = interp_dict(wk, n->l, res);
		break;
	case node_id:
		if (!wk->get_variable(wk, get_cstr(wk,n->data.str), res, wk->cur_project)) {
			vm_error_at(wk, n_id, "undefined object");
			ret = false;
			break;
		}
		ret = true;
		break;

	/* control flow */
	case node_block: {
		bool have_r;
interp_block:
		n->chflg |= node_visited; // for analyzer
		have_r = n->chflg & node_child_r
			 && get_node(wk->ast, n->r)->type != node_empty;

		assert(n->type == node_block);

		obj obj_l; // this return value is disregarded

		// XXX: this is a hacky way to avoid messing up the debug node
		// for eval'd strings
		bool is_internal = strcmp(wk->src->label, "<internal>") == 0;
		bool was_stepping = wk->dbg.stepping;
		if (!is_internal && !wk->dbg.stepping) {
			wk->dbg.node = n->l;
		}

		if (!wk->interp_node(wk, n->l, &obj_l)) {
			if (wk->dbg.break_on_err) {
				repl(wk, true);
			} else {
				return false;
			}
		}

		if (!is_internal
		    && was_stepping
		    && wk->dbg.stepping
		    && wk->dbg.last_line != get_node(wk->ast, n->l)->location.line) {
			wk->dbg.node = n->l;
			wk->dbg.last_line = get_node(wk->ast, n->l)->location.line;
			repl(wk, true);
		}

		if (have_r) {
			struct node *r = get_node(wk->ast, n->r);
			switch (r->type) {
			case node_empty:
				*res = obj_l;
				break;
			case node_block:
				n_id = n->r;
				n = r;
				goto interp_block;
			default:
				UNREACHABLE;
			}
		} else {
			*res = obj_l;
		}

		return true;
	}
	case node_if:
		ret = interp_if(wk, n, res);
		break;
	case node_foreach:
		ret = interp_foreach(wk, n, res);
		break;
	case node_continue:
		wk->loop_ctl = loop_continuing;
		ret = true;
		break;
	case node_break:
		wk->loop_ctl = loop_breaking;
		ret = true;
		break;
	case node_return:
		ret = wk->interp_node(wk, n->l, &wk->returned);
		wk->return_node = n_id;
		wk->returning = true;
		break;

	/* functions */
	case node_function:
		ret = interp_func(wk, n_id, false, 0, res);
		break;
	case node_method:
	case node_index: {
		obj l_id;
		assert(n->chflg & node_child_l);

		if (!wk->interp_node(wk, n->l, &l_id)) {
			ret = false;
			break;
		}

		ret = interp_chained(wk, n_id, l_id, res);
		break;
	}

	/* assignment */
	case node_assignment:
		ret = interp_assign(wk, n, res);
		break;

	/* comparison stuff */
	case node_not:
		ret = interp_not(wk, n, res);
		break;
	case node_and:
	case node_or:
		ret = interp_andor(wk, n, res);
		break;
	case node_comparison:
		ret = interp_comparison(wk, n, res);
		break;
	case node_ternary:
		ret = interp_ternary(wk, n, res);
		break;

	/* math */
	case node_u_minus:
		ret = interp_u_minus(wk, n, res);
		break;
	case node_arithmetic:
		ret = interp_arithmetic(wk, n_id, n->subtype, false, n->l, n->r, res);
		break;
	case node_plusassign:
		ret = interp_plusassign(wk, n_id, res);
		break;

	/* special */
	case node_stringify:
		ret = interp_stringify(wk, n, res);
		break;
	case node_func_def:
		ret = interp_func_def(wk, n, res);
		break;

	/* handled in other places */
	case node_foreach_args:
	case node_argument:
		assert(false && "unreachable");
		break;

	case node_empty:
		ret = true;
		break;

	/* never valid */
	case node_paren:
	case node_empty_line:
	case node_null:
		assert(false && "invalid node");
		break;
	}

	if (!ret) {
		*res = 0;
	}

	return ret;
}

void
interpreter_init(void)
{
	static bool init = false;

	if (init) {
		return;
	}

	build_func_impl_tables();
	init = true;
}

#include "posix.h"

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf_size.h"
#include "coerce.h"
#include "data/hash.h"
#include "error.h"
#include "functions/common.h"
#include "lang/analyze.h"
#include "lang/eval.h"
#include "lang/interpreter.h"
#include "lang/parser.h"
#include "lang/workspace.h"
#include "log.h"
#include "platform/path.h"

static bool analyze_error;

struct assignment {
	const char *name;
	obj o;
	bool accessed, default_var;
	uint32_t n_id;
};

struct scope_group {
	struct darr scopes;
};

struct {
	struct darr groups;
	struct darr base;
} assignment_scopes;

static const char *
inspect_typeinfo(struct workspace *wk, obj t)
{
	if (get_obj_type(wk, t) == obj_typeinfo) {
		struct obj_typeinfo *ti = get_obj_typeinfo(wk, t);

		return typechecking_type_to_s(wk, ti->type);
	} else {
		return obj_type_to_s(get_obj_type(wk, t));
	}
}

static obj
make_typeinfo(struct workspace *wk, uint32_t t, uint32_t sub_t)
{
	assert(t & obj_typechecking_type_tag);
	if (sub_t) {
		assert(sub_t & obj_typechecking_type_tag);
	}

	obj res;
	make_obj(wk, &res, obj_typeinfo);
	struct obj_typeinfo *type = get_obj_typeinfo(wk, res);
	type->type = t;
	type->subtype = sub_t;
	return res;
}

static uint32_t
obj_type_to_tc_type(enum obj_type t)
{
	uint32_t i;
	for (i = 0; i < ARRAY_LEN(typemap); ++i) {
		if (t == typemap[i].type) {
			return typemap[i].tc;
		}
	}

	UNREACHABLE_RETURN;
}

static void
merge_types(struct workspace *wk, struct obj_typeinfo *a, obj r)
{
	enum obj_type t = get_obj_type(wk, r);
	if (t == obj_typeinfo) {
		a->type |= get_obj_typeinfo(wk, r)->type;
	} else {
		a->type |= obj_type_to_tc_type(t);
	}
}

struct analyze_ctx {
	uint32_t expected;
	uint32_t found;
	func_impl func;
	enum comparison_type comparison_type;
};

typedef bool ((analyze_for_each_type_cb)(struct workspace *wk,
	struct analyze_ctx *ctx, uint32_t n_id, enum obj_type t, obj *res));

static bool
analyze_for_each_type(struct workspace *wk, struct analyze_ctx *ctx, uint32_t n_id,
	obj o, uint32_t typemask, analyze_for_each_type_cb cb, obj *res)
{
	bool ok = false;
	uint32_t i;
	obj r = 0;

	uint32_t t;
	if ((t = get_obj_type(wk, o)) == obj_typeinfo) {
		t = get_obj_typeinfo(wk, o)->type;
	}

	if (t & obj_typechecking_type_tag) {
		struct obj_typeinfo res_t = { 0 };

		if (typemask) {
			t &= typemask;
		}

		for (i = 0; i < ARRAY_LEN(typemap); ++i) {
			if ((t & typemap[i].tc) == typemap[i].tc) {
				if (cb(wk, ctx, n_id, typemap[i].type, &r)) {
					if (r) {
						merge_types(wk, &res_t, r);
					}
					ok = true;
				}
			}
		}

		make_obj(wk, res, obj_typeinfo);
		*get_obj_typeinfo(wk, *res) = res_t;
	} else {
		ok = cb(wk, ctx, n_id, t, res);
	}

	return ok;
}

/*
 * Variable assignment and retrieval is handled with the following functions.
 * The additional complexity is required due to variables that are
 * conditionally assigned, e.g. assigned in an if-block, or foreach loop.
 *
 * When a conditional block is started, a "scope group" is created, and then
 * every branch of the if statement gets its own sub-scope.  At the end of the
 * if statement, all the sub scopes are merged (conflicting object types get
 * merged) into the parent scope, and the scope group is popped.
 */

static struct assignment *
assign_lookup_scope(const char *name, struct darr *scope)
{
	uint32_t i;
	for (i = 0; i < scope->len; ++i) {
		struct assignment *a = darr_get(scope, i);
		if (strcmp(a->name, name) == 0) {
			return a;
		}
	}

	return NULL;
}

static struct assignment *
assign_lookup(struct workspace *wk, const char *name)
{
	uint32_t i;
	uint32_t id;
	struct assignment *found = NULL;

	for (i = 0; i < assignment_scopes.groups.len; ++i) {
		struct scope_group *g = darr_get(&assignment_scopes.groups, i);
		if (!(g->scopes.len)) {
			continue;
		}

		struct darr *scope = darr_get(&g->scopes, g->scopes.len - 1);
		if ((found = assign_lookup_scope(name, scope))) {
			break;
		}
	}

	if (!found && wk->projects.len && get_obj_id(wk, name, &id, wk->cur_project)) {
		found = darr_get(&assignment_scopes.base, id);
	}

	return found;
}

static struct assignment *
scope_assign(struct workspace *wk, const char *name, obj o, uint32_t n_id)
{
	struct assignment *a;
	if ((a = assign_lookup(wk, name))) {
		a->o = o;
		return a;
	}

	struct darr *s;
	if (assignment_scopes.groups.len) {
		struct scope_group *g = darr_get(&assignment_scopes.groups, assignment_scopes.groups.len - 1);

		assert(g->scopes.len);
		s = darr_get(&g->scopes, g->scopes.len - 1);
	} else {
		s = &assignment_scopes.base;
	}

	darr_push(s, &(struct assignment) {
		.name = name,
		.o = o,
		.n_id = n_id,
	});

	if (!assignment_scopes.groups.len) {
		struct hash *scope;
		if (wk->projects.len) {
			scope = &current_project(wk)->scope;
		} else {
			scope = &wk->scope;
		}

		hash_set_str(scope, name, s->len - 1);
	}

	return darr_get(s, s->len - 1);
}

static void
push_scope_group_scope(void)
{
	assert(assignment_scopes.groups.len);
	struct scope_group *g = darr_get(&assignment_scopes.groups, assignment_scopes.groups.len - 1);
	darr_push(&g->scopes, &(struct darr) { 0 });
	struct darr *scope = darr_get(&g->scopes, g->scopes.len - 1);
	darr_init(scope, 256, sizeof(struct assignment));
}

static void
push_scope_group(void)
{
	struct scope_group g = { 0 };
	darr_init(&g.scopes, 4, sizeof(struct darr));
	darr_push(&assignment_scopes.groups, &g);
}

static void
pop_scope_group(struct workspace *wk)
{
	assert(assignment_scopes.groups.len);
	size_t idx = assignment_scopes.groups.len - 1;
	struct scope_group *g = darr_get(&assignment_scopes.groups, idx);
	darr_del(&assignment_scopes.groups, idx);

	struct darr *base = darr_get(&g->scopes, 0);

	uint32_t i, j;
	for (i = 1; i < g->scopes.len; ++i) {
		struct darr *scope = darr_get(&g->scopes, i);
		for (j = 0; j < scope->len; ++j) {
			struct assignment *old, *a = darr_get(scope, j);
			if ((old = assign_lookup_scope(a->name, base))) {
				if (get_obj_type(wk, old->o) != obj_typeinfo) {
					obj old_tc = make_typeinfo(wk, obj_type_to_tc_type(get_obj_type(wk, old->o)), 0);
					old->o = old_tc;
				}

				merge_types(wk, get_obj_typeinfo(wk, old->o), a->o);
			} else {
				darr_push(base, a);
			}
		}
	}

	for (i = 0; i < base->len; ++i) {
		struct assignment *a = darr_get(base, i),
				  *b = scope_assign(wk, a->name, a->o, a->n_id);

		b->accessed = a->accessed;
	}

	for (i = 0; i < g->scopes.len; ++i) {
		struct darr *scope = darr_get(&g->scopes, i);
		darr_destroy(scope);
	}
	darr_destroy(&g->scopes);
}

/*
 *-----------------------------------------------------------------------------
 */

static bool analyze_chained(struct workspace *wk, uint32_t n_id, obj l_id, obj *res);

static bool
analyze_method(struct workspace *wk, struct analyze_ctx *ctx, uint32_t n_id, enum obj_type rcvr_type, obj *res)
{
	obj tmp = 0;
	struct node *n = get_node(wk->ast, n_id);

	const char *name = get_node(wk->ast, n->r)->dat.s;

	/* if (rcvr_type == obj_module) { */
	/* 	struct obj_module *m = get_obj_module(wk, rcvr_id); */
	/* 	enum module mod = m->module; */
	/* } */

	const struct func_impl_name *impl_tbl = func_tbl[rcvr_type][wk->lang_mode];

	if (!impl_tbl) {
		return true;
	}

	const struct func_impl_name *fi;
	if (!(fi = func_lookup(impl_tbl, name))) {
		return true;
	}

	++ctx->found;

	analyze_function_args(wk, fi->func, n->c);

	if (fi->return_type) {
		tmp = make_typeinfo(wk, fi->return_type, 0);
	}

	if (n->chflg & node_child_d) {
		return analyze_chained(wk, n->d, tmp, res);
	} else {
		*res = tmp;
		return true;
	}
}

static bool
analyze_index(struct workspace *wk, struct analyze_ctx *ctx, uint32_t n_id, enum obj_type lhs, obj *res)
{
	struct node *n = get_node(wk->ast, n_id);

	obj tmp = 0;

	switch (lhs) {
	case obj_disabler:
		*res = disabler_id;
		return true;
	case obj_array: {
		ctx->expected |= tc_number;
		tmp = make_typeinfo(wk, tc_any, 0);
		break;
	}
	case obj_dict: {
		ctx->expected |= tc_string;
		tmp = make_typeinfo(wk, tc_any, 0);
		break;
	}
	case obj_custom_target: {
		ctx->expected |= tc_number;
		tmp = make_typeinfo(wk, tc_file, 0);
		break;
	}
	case obj_string: {
		ctx->expected |= tc_number;
		tmp = make_typeinfo(wk, tc_string, 0);
		break;
	}
	default:
		UNREACHABLE_RETURN;
	}

	if (n->chflg & node_child_d) {
		return analyze_chained(wk, n->d, tmp, res);
	} else {
		*res = tmp;
		return true;
	}
}

static bool
analyze_chained(struct workspace *wk, uint32_t n_id, obj l_id, obj *res)
{
	struct node *n = get_node(wk->ast, n_id);

	switch (n->type) {
	case node_method: {
		struct analyze_ctx ctx = { 0 };

		if (!analyze_for_each_type(wk, &ctx, n_id, l_id, 0, analyze_method, res)) {
			return false;
		}

		if (!ctx.found) {
			interp_error(wk, n_id, "method %s not found on %s", get_node(wk->ast, n->r)->dat.s, inspect_typeinfo(wk, l_id));
			return false;
		}
		return true;
	}
	case node_index: {
		obj r;
		if (!wk->interp_node(wk, n->r, &r)) {
			return false;
		}

		struct analyze_ctx ctx = { 0 };

		const uint32_t tc_lhs = tc_string | tc_custom_target | tc_dict | tc_array;

		if (!typecheck(wk, n->l, l_id, tc_lhs)) {
			return false;
		}

		if (!analyze_for_each_type(wk, &ctx, n_id, l_id, tc_lhs, analyze_index, res)) {
			return false;
		}

		if (!typecheck(wk, n->r, r, ctx.expected)) {
			return false;
		}
		return true;
	}
	default:
		UNREACHABLE_RETURN;
	}

	return false;
}

static bool
analyze_func(struct workspace *wk, uint32_t n_id, obj *res)
{
	bool ret = true;

	obj tmp = 0;
	struct node *n = get_node(wk->ast, n_id);

	const char *name = get_node(wk->ast, n->l)->dat.s;

	const struct func_impl_name *impl_tbl = func_tbl[obj_default][wk->lang_mode];
	const struct func_impl_name *fi;

	if (!(fi = func_lookup(impl_tbl, name))) {
		interp_error(wk, n_id, "function %s not found", name);
		ret = false;

		tmp = make_typeinfo(wk, tc_any, 0);
	} else {
		analyze_function_args(wk, fi->func, n->r);
		if (fi->return_type) {
			tmp = make_typeinfo(wk, fi->return_type, 0);
		}
	}

	if (n->chflg & node_child_d) {
		return analyze_chained(wk, n->d, tmp, res);
	} else {
		*res = tmp;
		return ret;
	}
}

static bool
analyze_u_minus(struct workspace *wk, struct node *n, obj *res)
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

	if (get_obj_type(wk, l_id) == obj_number) {
		make_obj(wk, res, obj_number);
		set_obj_number(wk, *res, -get_obj_number(wk, l_id));
	} else {
		make_typeinfo(wk, tc_number, 0);
	}
	return true;
}

static bool
analyze_arithmetic_cb(struct workspace *wk, struct analyze_ctx *ctx, uint32_t n_id, enum obj_type lhs, obj *res)
{
	switch (lhs) {
	case obj_string: {
		ctx->expected |= tc_string;
		*res = make_typeinfo(wk, tc_string, 0);
		break;
	}
	case obj_number: {
		ctx->expected |= tc_number;
		*res = make_typeinfo(wk, tc_number, 0);
		break;
	}
	case obj_array: {
		ctx->expected |= tc_any;
		*res = make_typeinfo(wk, tc_array, 0);
		break;
	}
	case obj_dict: {
		ctx->expected |= tc_dict;
		*res = make_typeinfo(wk, tc_dict, 0);
		break;
	}
	default:
		break;
	}

	return true;
}


static bool
analyze_arithmetic(struct workspace *wk, uint32_t err_node,
	enum arithmetic_type type, uint32_t nl, uint32_t nr,
	obj *res)
{
	obj l, r;
	if (!wk->interp_node(wk, nl, &l)) {
		return false;
	}
	if (!wk->interp_node(wk, nr, &r)) {
		return false;
	}

	struct analyze_ctx ctx = { 0 };
	uint32_t tc_lhs;

	switch (type) {
	case arith_add:
		tc_lhs = tc_string | tc_number | tc_dict | tc_array;
		break;
	case arith_div:
		tc_lhs = tc_string | tc_number;
		break;
	case arith_sub:
	case arith_mod:
	case arith_mul:
		tc_lhs = tc_number;
		break;
	default:
		assert(false);
		return false;
	}

	if (!typecheck(wk, nl, l, tc_lhs)) {
		return false;
	}

	if (!analyze_for_each_type(wk, &ctx, nl, l, tc_lhs, analyze_arithmetic_cb, res)) {
		return false;
	}

	if (!typecheck(wk, nr, r, ctx.expected)) {
		return false;
	}

	return true;
}

static bool
analyze_not(struct workspace *wk, struct node *n, obj *res)
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

	*res = make_typeinfo(wk, tc_bool, 0);
	return true;
}

static bool
analyze_andor(struct workspace *wk, struct node *n, obj *res)
{
	obj obj_l_id, obj_r_id;

	if (!wk->interp_node(wk, n->l, &obj_l_id)) {
		return false;
	} else if (!typecheck(wk, n->l, obj_l_id, obj_bool)) {
		return false;
	}

	if (!wk->interp_node(wk, n->r, &obj_r_id)) {
		return false;
	} else if (!typecheck(wk, n->r, obj_r_id, obj_bool)) {
		return false;
	}

	*res = make_typeinfo(wk, tc_bool, 0);
	return true;
}

static bool
analyze_comparison_cb(struct workspace *wk, struct analyze_ctx *ctx, uint32_t n_id, enum obj_type lhs, obj *res)
{
	switch (ctx->comparison_type) {
	case comp_equal:
	case comp_nequal:
		ctx->expected |= obj_type_to_tc_type(lhs);
		break;
	case comp_in:
	case comp_not_in:
		ctx->expected |= tc_array | tc_dict;
		break;
	case comp_lt:
	case comp_le:
	case comp_gt:
	case comp_ge:
		ctx->expected |= tc_number;
		break;
	default:
		UNREACHABLE_RETURN;
	}

	*res = make_typeinfo(wk, tc_bool, 0);
	return true;
}

static bool
analyze_comparison(struct workspace *wk, struct node *n, obj *res)
{
	obj l, r;

	if (!wk->interp_node(wk, n->l, &l)) {
		return false;
	} else if (!wk->interp_node(wk, n->r, &r)) {
		return false;
	}

	struct analyze_ctx ctx = { .comparison_type = n->subtype };
	uint32_t tc_lhs;

	switch ((enum comparison_type)n->subtype) {
	case comp_equal:
	case comp_nequal:
		tc_lhs = tc_any;
		break;
	case comp_in:
	case comp_not_in:
		tc_lhs = tc_any;
		break;
	case comp_lt:
	case comp_le:
	case comp_gt:
	case comp_ge:
		tc_lhs = tc_number;
		break;
	default:
		UNREACHABLE_RETURN;
	}

	if (!typecheck(wk, n->l, l, tc_lhs)) {
		return false;
	}

	if (!analyze_for_each_type(wk, &ctx, n->l, l, tc_lhs, analyze_comparison_cb, res)) {
		return false;
	}

	if (!typecheck(wk, n->r, r, ctx.expected)) {
		return false;
	}

	return true;
}

static bool
analyze_ternary(struct workspace *wk, struct node *n, obj *res)
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

	struct obj_typeinfo res_t = { 0 };
	obj a, b;
	wk->interp_node(wk, n->l, &a);
	wk->interp_node(wk, n->r, &b);

	merge_types(wk, &res_t, a);
	merge_types(wk, &res_t, b);

	*res = make_typeinfo(wk, res_t.type, res_t.subtype);
	return true;
}

static bool
analyze_if(struct workspace *wk, struct node *n, obj *res)
{
	bool ret = true;

	switch ((enum if_type)n->subtype) {
	case if_if:
	case if_elseif: {
		obj cond_id;
		if (!wk->interp_node(wk, n->l, &cond_id)) {
			ret = false;
		} else if (cond_id == disabler_id) {
			*res = disabler_id;
			return true;
		} else if (!typecheck(wk, n->l, cond_id, obj_bool)) {
			ret = false;
		}

		break;
	}
	case if_else:
		break;
	default:
		UNREACHABLE_RETURN;
	}

	push_scope_group_scope();
	// if block
	if (!wk->interp_node(wk, n->r, res)) {
		ret = false;
	}

	if (n->chflg & node_child_c) {
		if (!wk->interp_node(wk, n->c, res)) {
			ret = false;
		}
	}

	*res = 0;
	return ret;
}

#if 0
struct interp_foreach_ctx {
	const char *id1, *id2;
	uint32_t block_node;
};

static enum iteration_result
interp_foreach_common(struct workspace *wk, struct interp_foreach_ctx *ctx)
{
	obj block_result;

	if (get_node(wk->ast, ctx->block_node)->type == node_empty) {
		return ir_done;
	}

	if (!interp_block(wk, get_node(wk->ast, ctx->block_node), &block_result)) {
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

	hash_set_str(&current_project(wk)->scope, ctx->id1, k_id);
	hash_set_str(&current_project(wk)->scope, ctx->id2, v_id);

	return interp_foreach_common(wk, ctx);
}

static enum iteration_result
interp_foreach_arr_iter(struct workspace *wk, void *_ctx, obj v_id)
{
	struct interp_foreach_ctx *ctx = _ctx;

	hash_set_str(&current_project(wk)->scope, ctx->id1, v_id);

	return interp_foreach_common(wk, ctx);
}

static bool
interp_foreach(struct workspace *wk, struct node *n, obj *res)
{
	obj iterable;
	bool ret;

	if (!interp_node(wk, n->r, &iterable)) {
		return false;
	}

	struct node *args = get_node(wk->ast, n->l);

	switch (get_obj_type(wk, iterable)) {
	case obj_array: {
		if (args->chflg & node_child_r) {
			interp_error(wk, n->l, "array foreach needs exactly one variable to set");
			return false;
		}

		struct interp_foreach_ctx ctx = {
			.id1 = get_node(wk->ast, args->l)->dat.s,
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
			interp_error(wk, n->l, "dict foreach needs exactly two variables to set");
			return false;
		}

		assert(get_node(wk->ast, get_node(wk->ast, args->r)->type == node_foreach_args));

		struct interp_foreach_ctx ctx = {
			.id1 = get_node(wk->ast, args->l)->dat.s,
			.id2 = get_node(wk->ast, get_node(wk->ast, args->r)->l)->dat.s,
			.block_node = n->c,
		};

		++wk->loop_depth;
		wk->loop_ctl = loop_norm;
		ret = obj_dict_foreach(wk, iterable, &ctx, interp_foreach_dict_iter);
		--wk->loop_depth;

		break;
	}
	default:
		interp_error(wk, n->r, "%s is not iterable", obj_type_to_s(get_obj_type(wk, iterable)));
		return false;
	}

	return ret;
}

static bool
interp_stringify(struct workspace *wk, struct node *n, obj *res)
{
	obj l_id;

	if (!interp_node(wk, n->l, &l_id)) {
		return false;
	}

	if (!coerce_string(wk, n->l, l_id, res)) {
		return false;
	}

	return true;
}

static bool
analyze_block(struct workspace *wk, struct node *n, obj *res)
{
	bool have_r = n->chflg & node_child_r
		      && get_node(wk->ast, n->r)->type != node_empty;

	assert(n->type == node_block);

	obj obj_l, obj_r;

	struct node *nl = get_node(wk->ast, n->l);
	switch (nl->type) {
	case node_if: {
		obj _;
		wk->interp_node(wk, n->l, &_);
		if (!wk->interp_node(wk, n->r, &obj_r)) {
			return false;
		}
		break;
	}
	default:
		if (!wk->interp_node(wk, n->l, &obj_l)) {
			return false;
		}
	}

	if (have_r) {
		if (!wk->interp_node(wk, n->r, &obj_r)) {
			return false;
		}

		*res = obj_r;
	} else {
		*res = obj_l;
	}

	return true;
}
#endif

static bool
analyze_assign(struct workspace *wk, struct node *n)
{
	obj rhs;
	if (!wk->interp_node(wk, n->r, &rhs)) {
		return false;
	}

	scope_assign(wk, get_node(wk->ast, n->l)->dat.s, rhs, n->l);
	return true;
}

static bool
analyze_plusassign(struct workspace *wk, uint32_t n_id, obj *_)
{
	struct node *n = get_node(wk->ast, n_id);

	obj rhs;
	if (!analyze_arithmetic(wk, n_id, arith_add, n->l, n->r, &rhs)) {
		return false;
	}

	scope_assign(wk, get_node(wk->ast, n->l)->dat.s, rhs, n->l);
	return true;
}

bool
analyze_node(struct workspace *wk, uint32_t n_id, obj *res)
{
	bool ret = true;
	*res = 0;

	struct node *n = get_node(wk->ast, n_id);

	++wk->stack_depth;
	if (wk->loop_ctl) {
		--wk->stack_depth;
		return true;
	}

	switch (n->type) {
	/* literals */
	case node_dict:
	case node_array:
	case node_string:
	case node_number:
	case node_bool:
		ret = interp_node(wk, n_id, res);
		break;

	/* control flow */
	case node_block:
		ret = interp_node(wk, n_id, res);
		break;
	case node_if:
		if (n->subtype == if_if) {
			push_scope_group();
		}

		ret = analyze_if(wk, n, res);

		if (n->subtype == if_if) {
			pop_scope_group(wk);
		}
		break;
	case node_foreach:
		/* ret = interp_foreach(wk, n, res); */
		break;
	case node_continue:
		ret = true;
		break;
	case node_break:
		ret = true;
		break;

	/* functions */
	case node_function:
		ret = analyze_func(wk, n_id, res);
		break;
	case node_method:
	case node_index: {
		obj l_id;
		assert(n->chflg & node_child_l);

		if (!analyze_node(wk, n->l, &l_id)) {
			ret = false;
			break;
		}

		ret = analyze_chained(wk, n_id, l_id, res);
		break;
	}

	/* assignment */
	case node_assignment:
		ret = analyze_assign(wk, n);
		break;
	case node_id: {
		struct assignment *a;
		if (!(a = assign_lookup(wk, n->dat.s))) {
			interp_error(wk, n_id, "undefined object");
			ret = false;
		} else {
			*res = a->o;
			a->accessed = true;
		}
		break;
	}

	/* comparison stuff */
	case node_not:
		ret = analyze_not(wk, n, res);
		break;
	case node_and:
	case node_or:
		ret = analyze_andor(wk, n, res);
		break;
	case node_comparison:
		ret = analyze_comparison(wk, n, res);
		break;
	case node_ternary:
		ret = analyze_ternary(wk, n, res);
		break;

	/* math */
	case node_u_minus:
		ret = analyze_u_minus(wk, n, res);
		break;
	case node_arithmetic:
		ret = analyze_arithmetic(wk, n_id, n->subtype, n->l, n->r, res);
		break;
	case node_plusassign:
		ret = analyze_plusassign(wk, n_id, res);
		break;

	/* special */
	case node_stringify:
		/* ret = interp_stringify(wk, n, res); */
		break;
	case node_empty:
		ret = true;
		break;

	/* never valid */
	case node_foreach_args:
	case node_argument:
	case node_paren:
	case node_empty_line:
	case node_null:
		UNREACHABLE_RETURN;
	}

	--wk->stack_depth;
	if (!ret) {
		analyze_error = true;
	}
	return true;
}

bool
do_analyze(void)
{
	bool res = false;;
	struct workspace wk;
	workspace_init(&wk);

	darr_init(&assignment_scopes.groups, 16, sizeof(struct scope_group));
	darr_init(&assignment_scopes.base, 256, sizeof(struct assignment));

	{
		/*
		 * default variables have to be re-assigned with scope_assign
		 * for them to be used in the analyzer
		 */
		const char *default_vars[] = {
			"meson",
			"host_machine",
			"build_machine",
			"target_machine",
		};

		uint32_t i;
		uint64_t obj;
		for (i = 0; i < ARRAY_LEN(default_vars); ++i) {
			obj = *hash_get_str(&wk.scope, default_vars[i]);
			hash_unset_str(&wk.scope, default_vars[i]);
			struct assignment *a = scope_assign(&wk, default_vars[i], obj, 0);
			a->default_var = true;
		}
	}

	wk.interp_node = analyze_node;

	if (!workspace_setup_dirs(&wk, "dummy", "argv0", false)) {
		goto err;
	}

	uint32_t project_id;
	res = eval_project(&wk, NULL, wk.source_root, wk.build_root, &project_id);

	{
		assert(assignment_scopes.groups.len == 0);
		uint32_t i;
		for (i = 0; i < assignment_scopes.base.len; ++i) {
			struct assignment *a = darr_get(&assignment_scopes.base, i);
			if (!a->default_var && !a->accessed) {
				// TODO: this requires ast information to be preserved after eval_project
				/* interp_error(&wk, a->n_id, "unused variable %s", a->name); */
				LOG_W("unused variable %s", a->name);
				res = false;
			}
		}
	}

	if (analyze_error) {
		res = false;
	}
err:
	darr_destroy(&assignment_scopes.groups);
	darr_destroy(&assignment_scopes.base);
	workspace_destroy(&wk);
	return res;
}
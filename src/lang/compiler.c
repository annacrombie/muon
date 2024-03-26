/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 */

#include "compat.h"

#include <inttypes.h>
#include <string.h>

#include "buf_size.h"
#include "functions/common.h"
#include "lang/compiler.h"
#include "lang/parser.h"
#include "lang/typecheck.h"
#include "lang/vm.h"
#include "lang/workspace.h"
#include "platform/path.h"

/******************************************************************************
 * compiler
 ******************************************************************************/

static void
push_location(struct workspace *wk, struct node *n)
{
	arr_push(&wk->vm.locations,
		&(struct source_location_mapping){
			.ip = wk->vm.code.len,
			.loc = n->location,
			.src_idx = wk->vm.src.len - 1,
		});
}

static void
push_code(struct workspace *wk, uint8_t b)
{
	arr_push(&wk->vm.code, &b);
}

static void
push_constant_at(obj v, uint8_t *code)
{
	code[0] = (v >> 16) & 0xff;
	code[1] = (v >> 8) & 0xff;
	code[2] = v & 0xff;
}

static void
push_constant(struct workspace *wk, obj v)
{
	push_code(wk, (v >> 16) & 0xff);
	push_code(wk, (v >> 8) & 0xff);
	push_code(wk, v & 0xff);
}

/* static void */
/* push_constant64(struct workspace *wk, uint64_t v) */
/* { */
/* 	push_code(wk, (v >> 56) & 0xff); */
/* 	push_code(wk, (v >> 48) & 0xff); */
/* 	push_code(wk, (v >> 40) & 0xff); */
/* 	push_code(wk, (v >> 32) & 0xff); */
/* 	push_code(wk, (v >> 24) & 0xff); */
/* 	push_code(wk, (v >> 16) & 0xff); */
/* 	push_code(wk, (v >> 8) & 0xff); */
/* 	push_code(wk, (v >> 0) & 0xff); */
/* } */

static void compile_block(struct workspace *wk, struct node *n);
static void compile_expr(struct workspace *wk, struct node *n);

static void
comp_node(struct workspace *wk, struct node *n)
{
	assert(n->type != node_type_stmt);

	/* L("compiling %s", node_to_s(wk, n)); */

	switch (n->type) {
	case node_type_stmt: UNREACHABLE;

	case node_type_id_lit:
	case node_type_args:
	case node_type_def_args:
	case node_type_kw:
	case node_type_list:
	case node_type_foreach_args:
		// Skipped
		break;

	case node_type_stringify: push_code(wk, op_stringify); break;
	case node_type_index: push_code(wk, op_index); break;
	case node_type_negate: push_code(wk, op_negate); break;
	case node_type_add: push_code(wk, op_add); break;
	case node_type_sub: push_code(wk, op_sub); break;
	case node_type_mul: push_code(wk, op_mul); break;
	case node_type_div: push_code(wk, op_div); break;
	case node_type_mod: push_code(wk, op_mod); break;
	case node_type_not: push_code(wk, op_not); break;
	case node_type_eq: push_code(wk, op_eq); break;
	case node_type_neq:
		push_code(wk, op_eq);
		push_code(wk, op_not);
		break;
	case node_type_in: push_code(wk, op_in); break;
	case node_type_not_in:
		push_code(wk, op_in);
		push_code(wk, op_not);
		break;
	case node_type_lt: push_code(wk, op_lt); break;
	case node_type_gt: push_code(wk, op_gt); break;
	case node_type_leq:
		push_code(wk, op_gt);
		push_code(wk, op_not);
		break;
	case node_type_geq:
		push_code(wk, op_lt);
		push_code(wk, op_not);
		break;
	case node_type_id:
		push_code(wk, op_constant);
		push_constant(wk, n->data.str);
		push_code(wk, op_load);
		break;
	case node_type_number:
		push_code(wk, op_constant);
		obj o;
		make_obj(wk, &o, obj_number);
		set_obj_number(wk, o, n->data.num);
		push_constant(wk, o);
		break;
	case node_type_bool:
		push_code(wk, op_constant);
		push_constant(wk, n->data.num ? obj_bool_true : obj_bool_false);
		break;
	case node_type_string:
		push_code(wk, op_constant);
		push_constant(wk, n->data.str);
		break;
	case node_type_array:
		push_code(wk, op_constant_list);
		push_constant(wk, n->data.len.args);
		break;
	case node_type_dict:
		push_code(wk, op_constant_dict);
		push_constant(wk, n->data.len.kwargs);
		break;
	case node_type_assign:
		push_code(wk, op_constant);
		push_constant(wk, n->l->data.str);
		push_code(wk, op_store);
		break;
	case node_type_plusassign:
		push_code(wk, op_add_store);
		push_constant(wk, n->l->data.str);
		break;
	case node_type_method: {
		push_code(wk, op_call_method);
		push_constant(wk, n->l->data.len.args);
		push_constant(wk, n->l->data.len.kwargs);
		push_constant(wk, n->r->data.str);
		break;
	}
	case node_type_call: {
		bool known = false;
		uint32_t idx;

		if (n->r->type == node_type_id_lit) {
			const struct str *name = get_str(wk, n->r->data.str);
			if (str_eql(name, &WKSTR("subdir_done"))) {
				push_location(wk, n);

				if (!(n->l->data.len.args == 0 && n->l->data.len.kwargs == 0)) {
				}

				push_code(wk, op_return);
				break;
			} else if (str_eql(name, &WKSTR("set_variable"))) {
				push_location(wk, n);

				if (!(n->l->data.len.args == 2 && n->l->data.len.kwargs == 0)) {
				}

				push_code(wk, op_swap);
				push_code(wk, op_store);
				break;
			} else if (str_eql(name, &WKSTR("get_variable"))) {
				uint32_t jmp1, jmp2;

				push_location(wk, n);

				if (!(1 <= n->l->data.len.args && n->l->data.len.args <= 2
					    && n->l->data.len.kwargs == 0)) {
				}

				if (n->l->data.len.args == 2) {
					push_code(wk, op_swap);
				}

				push_code(wk, op_jmp_if_disabler);
				jmp1 = wk->vm.code.len;
				push_constant(wk, 0);

				push_code(wk, op_jmp);
				jmp2 = wk->vm.code.len;
				push_constant(wk, 0);

				push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, jmp1));
				push_code(wk, op_constant);
				push_constant(wk, disabler_id);
				push_code(wk, op_jmp);
				jmp1 = wk->vm.code.len;
				push_constant(wk, 0);

				push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, jmp2));

				if (n->l->data.len.args == 2) {
					push_code(wk, op_try_load);
					push_code(wk, op_jmp_if_null);
					jmp2 = wk->vm.code.len;
					push_constant(wk, 0);

					push_code(wk, op_swap);
					push_code(wk, op_pop);

					push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, jmp1));
					push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, jmp2));
				} else {
					push_code(wk, op_load);

					push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, jmp1));
				}
				break;
			} else if (str_eql(name, &WKSTR("is_disabler"))) {
				uint32_t true_jmp_tgt, false_jmp_tgt;

				push_location(wk, n);

				if (!(n->l->data.len.args == 1 && n->l->data.len.kwargs == 0)) {
				}

				push_code(wk, op_jmp_if_disabler);
				true_jmp_tgt = wk->vm.code.len;
				push_constant(wk, 0);

				push_code(wk, op_pop);
				push_code(wk, op_constant);
				push_constant(wk, obj_bool_false);

				push_code(wk, op_jmp);
				false_jmp_tgt = wk->vm.code.len;
				push_constant(wk, 0);

				push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, true_jmp_tgt));

				push_code(wk, op_constant);
				push_constant(wk, obj_bool_true);

				push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, false_jmp_tgt));
				break;
			}

			known = func_lookup(wk, 0, name->s, &idx, 0);
			if (!known) {
				n->r->type = node_type_id;
				push_location(wk, n->r);
				comp_node(wk, n->r);
			}
		}

		push_location(wk, n);

		if (known) {
			push_code(wk, op_call_native);
			push_constant(wk, n->l->data.len.args);
			push_constant(wk, n->l->data.len.kwargs);
			push_constant(wk, idx);
		} else {
			push_code(wk, op_call);
			push_constant(wk, n->l->data.len.args);
			push_constant(wk, n->l->data.len.kwargs);
		}
		break;
	}
	case node_type_return: {
		if (!n->l) {
			push_code(wk, op_constant);
			push_constant(wk, 0);
		}
		push_code(wk, op_return);
		break;
	}
	case node_type_foreach: {
		uint32_t break_jmp_patch_tgt, loop_body_start;
		struct node *ida = n->l->l->l, *idb = n->l->l->r;

		compile_expr(wk, n->l->r);

		push_location(wk, n);

		if (idb) {
			push_code(wk, op_typecheck);
			push_constant(wk, obj_dict);
		}
		push_code(wk, op_iterator);

		loop_body_start = wk->vm.code.len;

		push_code(wk, op_iterator_next);
		push_code(wk, op_jmp_if_null);
		break_jmp_patch_tgt = wk->vm.code.len;
		push_constant(wk, 0);

		push_code(wk, op_constant);
		push_constant(wk, ida->data.str);
		push_code(wk, op_store);
		push_code(wk, op_pop);

		if (idb) {
			push_code(wk, op_constant);
			push_constant(wk, idb->data.str);
			push_code(wk, op_store);
			push_code(wk, op_pop);
		}

		uint32_t loop_jmp_stack_base = wk->vm.compiler_state.loop_jmp_stack.len;
		arr_push(&wk->vm.compiler_state.loop_jmp_stack, &loop_body_start);

		compile_block(wk, n->r);

		push_code(wk, op_jmp);
		push_constant(wk, loop_body_start);
		push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, break_jmp_patch_tgt));

		arr_pop(&wk->vm.compiler_state.loop_jmp_stack);

		while (wk->vm.compiler_state.loop_jmp_stack.len > loop_jmp_stack_base) {
			break_jmp_patch_tgt = *(uint32_t *)arr_pop(&wk->vm.compiler_state.loop_jmp_stack);
			push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, break_jmp_patch_tgt));
		}
		break;
	}
	case node_type_continue: {
		push_code(wk, op_jmp);
		push_constant(wk, *(uint32_t *)arr_peek(&wk->vm.compiler_state.loop_jmp_stack, 1));
		break;
	}
	case node_type_break: {
		push_code(wk, op_jmp);

		uint32_t top = *(uint32_t *)arr_pop(&wk->vm.compiler_state.loop_jmp_stack);
		arr_push(&wk->vm.compiler_state.loop_jmp_stack, &wk->vm.code.len);
		arr_push(&wk->vm.compiler_state.loop_jmp_stack, &top);

		push_constant(wk, 0);
		push_constant(wk, *(uint32_t *)arr_peek(&wk->vm.compiler_state.loop_jmp_stack, 1));
		break;
	}
	case node_type_if: {
		uint32_t else_jmp = 0, end_jmp;
		uint32_t patch_tgts = 0;

		while (n) {
			if (n->l->l) {
				compile_expr(wk, n->l->l);
				push_code(wk, op_jmp_if_disabler);
				arr_push(&wk->vm.compiler_state.if_jmp_stack, &wk->vm.code.len);
				++patch_tgts;
				push_constant(wk, 0);
				push_code(wk, op_jmp_if_false);
				else_jmp = wk->vm.code.len;
				push_constant(wk, 0);
			}

			compile_block(wk, n->l->r);

			push_code(wk, op_jmp);
			arr_push(&wk->vm.compiler_state.if_jmp_stack, &wk->vm.code.len);
			++patch_tgts;
			push_constant(wk, 0);

			if (n->l->l) {
				push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, else_jmp));
			}

			n = n->r;
		}

		for (uint32_t i = 0; i < patch_tgts; ++i) {
			end_jmp = *(uint32_t *)arr_pop(&wk->vm.compiler_state.if_jmp_stack);
			push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, end_jmp));
		}
		break;
	}
	case node_type_ternary: {
		uint32_t else_jmp, end_jmp;

		L("comping ternary l");
		compile_expr(wk, n->l);
		push_code(wk, op_jmp_if_false);
		else_jmp = wk->vm.code.len;
		push_constant(wk, 0);
		L("comping ternary r->l");
		compile_expr(wk, n->r->l);
		push_code(wk, op_jmp);
		end_jmp = wk->vm.code.len;
		push_constant(wk, 0);
		push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, else_jmp));
		L("comping ternary r->r");
		compile_expr(wk, n->r->r);
		push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, end_jmp));

		break;
	}
	case node_type_or: {
		uint32_t jmp1, jmp2, jmp3;
		compile_expr(wk, n->l);

		push_code(wk, op_jmp_if_disabler);
		jmp1 = wk->vm.code.len;
		push_constant(wk, 0);

		push_code(wk, op_dup);
		push_code(wk, op_jmp_if_true);
		jmp2 = wk->vm.code.len;
		push_constant(wk, 0);

		push_code(wk, op_pop);

		compile_expr(wk, n->r);
		push_code(wk, op_typecheck);
		push_constant(wk, obj_bool);

		push_code(wk, op_jmp);
		jmp3 = wk->vm.code.len;
		push_constant(wk, 0);

		push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, jmp1));
		push_code(wk, op_constant);
		push_constant(wk, disabler_id);

		push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, jmp2));
		push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, jmp3));
		break;
	}
	case node_type_and: {
		uint32_t short_circuit_jmp, disabler_jmp;
		compile_expr(wk, n->l);
		push_code(wk, op_jmp_if_disabler);
		disabler_jmp = wk->vm.code.len;
		push_constant(wk, 0);
		push_code(wk, op_dup);
		push_code(wk, op_jmp_if_false);
		short_circuit_jmp = wk->vm.code.len;
		push_constant(wk, 0);
		push_code(wk, op_pop);
		compile_expr(wk, n->r);
		push_code(wk, op_typecheck);
		push_constant(wk, obj_bool);
		push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, short_circuit_jmp));
		push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, disabler_jmp));
		break;
	}
	case node_type_func_def: {
		obj f;
		uint32_t func_jump_over_patch_tgt;
		struct obj_func *func;
		struct node *arg;

		make_obj(wk, &f, obj_func);
		func = get_obj_func(wk, f);

		push_code(wk, op_jmp);
		func_jump_over_patch_tgt = wk->vm.code.len;
		push_constant(wk, 0);

		/* function body start */

		func->entry = wk->vm.code.len;

		for (arg = n->l->r; arg; arg = arg->r) {
			/* if (arg->subtype == arg_normal) { */
			/* 	if (f->nkwargs) { */
			/* 		vm_error_at(wk, arg_id, "non-kwarg after kwargs"); */
			/* 	} */
			if (arg->l) {
				func->an[func->nargs] = (struct args_norm){
					.name = get_cstr(wk, arg->l->data.str),
					.type = tc_any,
				};
				++func->nargs;
			}
			/* } else if (arg->subtype == arg_kwarg) { */
			/* 	if (!f->nkwargs) { */
			/* 		make_obj(wk, &f->kwarg_defaults, obj_array); */
			/* 	} */
			/* 	++f->nkwargs; */

			/* 	obj r; */
			/* 	if (!wk->interp_node(wk, arg->r, &r)) { */
			/* 		return false; */
			/* 	} */

			/* 	obj_array_push(wk, f->kwarg_defaults, r); */
			/* } */

			/* if (!(arg->chflg & node_child_c)) { */
			/* 	break; */
			/* } */
			/* arg_id = arg->c; */
		}
		func->an[func->nargs].type = ARG_TYPE_NULL;

		compile_block(wk, n->r);
		if (wk->vm.code.e[wk->vm.code.len - 1] != op_return) {
			push_code(wk, op_constant);
			push_constant(wk, 0);
			push_code(wk, op_return);
		}

		/* function body end */

		push_constant_at(wk->vm.code.len, arr_get(&wk->vm.code, func_jump_over_patch_tgt));

		push_code(wk, op_constant_func);
		push_constant(wk, f);

		push_code(wk, op_store);
		push_constant(wk, n->l->l->data.str);

		break;
	}
	}
}

static void
compile_expr(struct workspace *wk, struct node *n)
{
	struct node *peek, *prev = 0;

	uint32_t stack_base = wk->vm.compiler_state.node_stack.len;

	while (wk->vm.compiler_state.node_stack.len > stack_base || n) {
		if (n) {
			if (n->type == node_type_foreach || n->type == node_type_if || n->type == node_type_func_def
				|| n->type == node_type_ternary || n->type == node_type_and
				|| n->type == node_type_or) {
				comp_node(wk, n);
				prev = n;
				n = 0;
			} else {
				arr_push(&wk->vm.compiler_state.node_stack, &n);
				n = n->l;
			}
		} else {
			peek = *(struct node **)arr_peek(&wk->vm.compiler_state.node_stack, 1);
			if (peek->r && prev != peek->r) {
				n = peek->r;
			} else {
				push_location(wk, peek);
				comp_node(wk, peek);
				prev = *(struct node **)arr_pop(&wk->vm.compiler_state.node_stack);
			}
		}
	}
}

static void
compile_block(struct workspace *wk, struct node *n)
{
	while (n && n->l) {
		assert(n->type == node_type_stmt);
		compile_expr(wk, n->l);
		if (n->l->type != node_type_if) {
			push_code(wk, op_pop);
		}
		n = n->r;
	}
}

void
compiler_write_initial_code_segment(struct workspace *wk)
{
	arr_push(&wk->vm.locations,
		&(struct source_location_mapping){
			.ip = 0,
			.loc = { 0 },
			.src_idx = UINT32_MAX,
		});

	push_code(wk, op_return);
}

bool
compile(struct workspace *wk, struct source *src, uint32_t flags, uint32_t *entry)
{
	struct node *n;

	bucket_arr_clear(&wk->vm.compiler_state.nodes);
	wk->vm.compiler_state.err = false;

	if (!(n = parse(wk, src, &wk->vm.compiler_state.nodes))) {
		wk->vm.compiler_state.err = true;
	}

	*entry = wk->vm.code.len;
	compile_block(wk, n);
	push_code(wk, op_return);

	assert(wk->vm.compiler_state.node_stack.len == 0);
	assert(wk->vm.compiler_state.loop_jmp_stack.len == 0);
	assert(wk->vm.compiler_state.if_jmp_stack.len == 0);
	return !wk->vm.compiler_state.err;
}

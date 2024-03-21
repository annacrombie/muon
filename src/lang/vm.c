/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <inttypes.h>
#include <string.h>

#include "coerce.h"
#include "error.h"
#include "functions/common.h"
#include "lang/compiler.h"
#include "lang/object_iterators.h"
#include "lang/parser.h"
#include "lang/typecheck.h"
#include "lang/vm.h"
#include "lang/workspace.h"
#include "log.h"
#include "platform/mem.h"
#include "platform/path.h"

/******************************************************************************
 * object stack
 ******************************************************************************/

struct obj_stack_entry {
	obj o;
	uint32_t ip;
};

enum { object_stack_page_size = 1024 / sizeof(struct obj_stack_entry) };

static void
object_stack_alloc_page(struct object_stack *s)
{
	bucket_arr_pushn(&s->ba, 0, 0, s->ba.bucket_size);
	++s->bucket;
	s->page = (struct obj_stack_entry *)((struct bucket *)s->ba.buckets.e)[s->bucket].mem;
	((struct bucket *)s->ba.buckets.e)[s->bucket].len = object_stack_page_size;
	s->i = 0;
}

static void
object_stack_init(struct object_stack *s)
{
	bucket_arr_init(&s->ba, object_stack_page_size, sizeof(struct obj_stack_entry));
	s->page = (struct obj_stack_entry *)((struct bucket *)s->ba.buckets.e)[0].mem;
	((struct bucket *)s->ba.buckets.e)[0].len = object_stack_page_size;
}

static void
object_stack_push(struct workspace *wk, obj o)
{
	if (wk->vm.stack.i >= object_stack_page_size) {
		object_stack_alloc_page(&wk->vm.stack);
	}

	wk->vm.stack.page[wk->vm.stack.i] = (struct obj_stack_entry){ .o = o, .ip = wk->vm.ip - 1 };
	++wk->vm.stack.i;
	++wk->vm.stack.ba.len;
}

static struct obj_stack_entry *
object_stack_pop_entry(struct object_stack *s)
{
	if (!s->i) {
		assert(s->bucket);
		--s->bucket;
		s->page = (struct obj_stack_entry *)((struct bucket *)s->ba.buckets.e)[s->bucket].mem;
		s->i = object_stack_page_size;
	}

	--s->i;
	--s->ba.len;
	return &s->page[s->i];
}

static obj
object_stack_pop(struct object_stack *s)
{
	return object_stack_pop_entry(s)->o;
}

static struct obj_stack_entry *
object_stack_peek_entry(struct object_stack *s, uint32_t off)
{
	return bucket_arr_get(&s->ba, s->ba.len - off);
}

static obj
object_stack_peek(struct object_stack *s, uint32_t off)
{
	return ((struct obj_stack_entry *)bucket_arr_get(&s->ba, s->ba.len - off))->o;
}

static void
object_stack_discard(struct object_stack *s, uint32_t n)
{
	assert(s->ba.len >= n);
	s->ba.len -= n;
	s->bucket = s->ba.len / s->ba.bucket_size;
	s->page = (struct obj_stack_entry *)((struct bucket *)s->ba.buckets.e)[s->bucket].mem;
	s->i = s->ba.len % s->ba.bucket_size;
}

void
object_stack_print(struct workspace *wk, struct object_stack *s)
{
	for (int32_t i = s->ba.len - 1; i >= 0; --i) {
		struct obj_stack_entry *e = bucket_arr_get(&s->ba, i);
		obj_fprintf(wk, log_file(), "%o%s", e->o, i > 0 ? ", " : "");
	}
	log_plain("\n");
}

/******************************************************************************
 * vm errors
 ******************************************************************************/

static void
vm_lookup_inst_location(struct vm *vm, uint32_t ip, struct source_location *loc, struct source **src)
{
	struct source_location_mapping *locations = (struct source_location_mapping *)vm->locations.e;

	uint32_t i;
	for (i = 0; i < vm->locations.len; ++i) {
		if (locations[i].ip >= ip) {
			break;
		}
	}

	if (i == vm->locations.len) {
		--i;
	}

	*loc = locations[i].loc;

	if (locations[i].src_idx == UINT32_MAX) {
		static struct source null_src = { 0 };
		*src = &null_src;
	} else {
		*src = arr_get(&vm->src, locations[i].src_idx);
	}
}

void
vm_diagnostic_v(struct workspace *wk, uint32_t ip, enum log_level lvl, const char *fmt, va_list args)
{
	static char buf[1024];
	vsnprintf(buf, ARRAY_LEN(buf), fmt, args);

	struct source_location loc;
	struct source *src;
	vm_lookup_inst_location(&wk->vm, wk->vm.ip, &loc, &src);

	error_message(src, loc, lvl, buf);

	if (lvl == log_error) {
		wk->vm.ip = wk->vm.code.len;
		wk->vm.error = true;
	}
}

void
vm_diagnostic(struct workspace *wk, uint32_t ip, enum log_level lvl, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vm_diagnostic_v(wk, ip, lvl, fmt, args);
	va_end(args);
}

void
vm_error_at(struct workspace *wk, uint32_t ip, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vm_diagnostic_v(wk, ip, log_error, fmt, args);
	va_end(args);
}

void
vm_warning_at(struct workspace *wk, uint32_t ip, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vm_diagnostic_v(wk, ip, log_warn, fmt, args);
	va_end(args);
}

void
vm_error(struct workspace *wk, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vm_diagnostic_v(wk, wk->vm.ip, log_error, fmt, args);
	va_end(args);
}

/******************************************************************************
 * pop_args
 ******************************************************************************/

static bool
typecheck_function_arg(struct workspace *wk, uint32_t ip, obj *val, type_tag type)
{
	bool listify = (type & TYPE_TAG_LISTIFY) == TYPE_TAG_LISTIFY;
	type &= ~TYPE_TAG_LISTIFY;

	enum obj_type t = get_obj_type(wk, *val);

	// If obj_file or tc_file is requested, and the argument is an array of
	// length 1, try to unpack it.
	if (!listify && (type == obj_file || (type & tc_file) == tc_file)) {
		if (t == obj_array && get_obj_array(wk, *val)->len == 1) {
			obj i0;
			obj_array_index(wk, *val, 0, &i0);
			if (get_obj_type(wk, i0) == obj_file) {
				*val = i0;
			}
		} else if (t == obj_typeinfo && (get_obj_typeinfo(wk, *val)->type & tc_array) == tc_array) {
			return true;
		}
	}

	if (listify && t == obj_array) {
		obj v, arr;
		make_obj(wk, &arr, obj_array);

		LO("foreach flat on %o\n", *val);
		obj_array_flat_for_begin(wk, *val, v) {
			LO(">> %o\n", v);
			if (!typecheck(wk, ip, v, type)) {
				return false;
			}

			obj_array_push(wk, arr, v);
			obj_array_flat_for_end;
		}

		*val = arr;
		return true;
	}

	return typecheck(wk, ip, *val, type);
}

bool
pop_args(struct workspace *wk, struct args_norm an[], struct args_kw akw[])
{
	const char *kw;
	struct obj_stack_entry *entry;
	uint32_t i, j, argi;

	for (i = 0; i < wk->vm.nkwargs; ++i) {
		entry = object_stack_pop_entry(&wk->vm.stack);
		kw = get_str(wk, entry->o)->s;

		if (!akw) {
			vm_error_at(wk, entry->ip, "this function does not accept kwargs");
			return false;
		}

		for (j = 0; akw[j].key; ++j) {
			if (strcmp(kw, akw[j].key) == 0) {
				break;
			}
		}

		if (!akw[j].key) {
			vm_diagnostic(wk, entry->ip, log_error, "unknown kwarg %s", kw);
			return false;
		}

		entry = object_stack_pop_entry(&wk->vm.stack);

		if (!typecheck_function_arg(wk, entry->ip, &entry->o, akw[j].type)) {
			return false;
		} else if (akw[j].set) {
			vm_error_at(wk, entry->ip, "keyword argument '%s' set twice", akw[j].key);
			return false;
		}

		akw[j].val = entry->o;
		akw[j].node = entry->ip;
		akw[j].set = true;
	}

	object_stack_discard(&wk->vm.stack, wk->vm.nkwargs);

	argi = 0;

	for (i = 0; an && an[i].type != ARG_TYPE_NULL; ++i) {
		if (an[i].type & TYPE_TAG_GLOB) {
			an[i].type &= ~TYPE_TAG_GLOB;
			an[i].type |= TYPE_TAG_LISTIFY;
			make_obj(wk, &an[i].val, obj_array);
			for (j = i; j < wk->vm.nargs; ++j) {
				entry = object_stack_peek_entry(&wk->vm.stack, wk->vm.nargs - argi);
				obj_array_push(wk, an[i].val, entry->o);
				an[i].node = entry->ip; // TODO
				++argi;
			}
		} else {
			if (argi >= wk->vm.nargs) {
				if (!an[i].optional) {
					vm_error(wk, "not enough args");
					return false;
				} else {
					break;
				}
			}

			entry = object_stack_peek_entry(&wk->vm.stack, wk->vm.nargs - argi);
			an[i].val = entry->o;
			an[i].node = entry->ip;
			an[i].set = true;
			++argi;
		}

		if (!typecheck_function_arg(wk, an[i].node, &an[i].val, an[i].type)) {
			return false;
		}
	}

	if (argi > wk->vm.nargs) {
		vm_error(wk, "too many args, got %d, expected %d", argi, wk->vm.nargs);
		return false;
	}

	L("discarding %d", argi);
	object_stack_discard(&wk->vm.stack, argi);
	return true;
}

/******************************************************************************
 * utility functions
 ******************************************************************************/

static obj
vm_get_constant(uint8_t *code, uint32_t *ip)
{
	obj r = (code[*ip + 0] << 16) | (code[*ip + 1] << 8) | code[*ip + 2];
	*ip += 3;
	return r;
}

/******************************************************************************
 * disassembler
 ******************************************************************************/

struct vm_dis_result {
	const char *text;
	uint32_t inst_len;
};

struct vm_dis_result
vm_dis_inst(struct workspace *wk, uint8_t *code, uint32_t base_ip)
{
	uint32_t i = 0;
	static char buf[2048];
#define buf_push(...) i += obj_snprintf(wk, &buf[i], sizeof(buf) - i, __VA_ARGS__);
#define op_case(__op) \
	case __op: buf_push(#__op);

	uint32_t ip = base_ip;
	buf_push("%04x ", ip);

	// clang-format off
	switch ((enum op)code[ip++]) {
	op_case(op_pop) break;
	op_case(op_stringify) break;
	op_case(op_index) break;
	op_case(op_add) break;
	op_case(op_sub) break;
	op_case(op_mul) break;
	op_case(op_div) break;
	op_case(op_mod) break;
	op_case(op_and) break;
	op_case(op_or) break;
	op_case(op_eq) break;
	op_case(op_in) break;
	op_case(op_gt) break;
	op_case(op_lt) break;
	op_case(op_not) break;
	op_case(op_negate) break;
	op_case(op_return) break;
	op_case(op_iterator) break;
	op_case(op_iterator_next) break;
	op_case(op_eval_file) break;

	op_case(op_store)
		buf_push(":%s", get_str(wk, vm_get_constant(code, &ip))->s);
		break;
	op_case(op_add_store)
		buf_push(":%s", get_str(wk, vm_get_constant(code, &ip))->s);
		break;
	op_case(op_load)
		buf_push(":%s", get_str(wk, vm_get_constant(code, &ip))->s);
		break;
	op_case(op_constant)
		buf_push(":%o", vm_get_constant(code, &ip));
		break;
	op_case(op_constant_list)
		buf_push(":len:%d", vm_get_constant(code, &ip));
		break;
	op_case(op_constant_dict)
		buf_push(":len:%d", vm_get_constant(code, &ip));
		break;
	op_case(op_constant_func)
		buf_push(":%d", vm_get_constant(code, &ip));
		break;
	op_case(op_call)
		buf_push(":%d,%d", vm_get_constant(code, &ip), vm_get_constant(code, &ip));
		break;
	op_case(op_call_method) {
		uint32_t a, b, c;
		a = vm_get_constant(code, &ip);
		b = vm_get_constant(code, &ip);
		c = vm_get_constant(code, &ip);
		buf_push(":%d,%d,%o", a, b, c);
		break;
	}
	op_case(op_call_native)
		buf_push(":");
		buf_push("%d,%d,", vm_get_constant(code, &ip), vm_get_constant(code, &ip));
		uint32_t id = vm_get_constant(code, &ip);
		buf_push("%s", native_funcs[id].name);
		break;
	op_case(op_jmp_if_null)
		buf_push(":%04x", vm_get_constant(code, &ip));
		break;
	op_case(op_jmp_if_false)
		buf_push(":%04x", vm_get_constant(code, &ip));
		break;
	op_case(op_jmp)
		buf_push(":%04x", vm_get_constant(code, &ip));
		break;
	op_case(op_typecheck)
		buf_push(":%s", obj_type_to_s(vm_get_constant(code, &ip)));
		break;
	}
	// clang-format on

#undef buf_push

	return (struct vm_dis_result){ buf, ip - base_ip };
}

void
vm_dis(struct workspace *wk)
{
	uint32_t w = 60;

	char loc_buf[256];
	for (uint32_t i = 0; i < wk->vm.code.len;) {
		struct vm_dis_result dis = vm_dis_inst(wk, wk->vm.code.e, i);
		struct source_location loc;
		struct source *src;
		vm_lookup_inst_location(&wk->vm, i, &loc, &src);
		snprintf(loc_buf, sizeof(loc_buf), "%s:%3d:%02d", src ? src->label : 0, loc.line, loc.col);
		printf("%-*s%s\n", w, dis.text, loc_buf);
		i += dis.inst_len;
	}
}

/******************************************************************************
 * vm_execute
 ******************************************************************************/

#define binop_disabler_check(a, b)                  \
	if (a == disabler_id || b == disabler_id) { \
		object_stack_push(wk, a);           \
		break;                              \
	}

#define vm_pop(__it) entry = object_stack_pop_entry(&wk->vm.stack), __it = entry->o, __it##_ip = entry->ip

void
vm_execute(struct workspace *wk)
{
	struct obj_stack_entry *entry;
	uint32_t b_ip;
	obj a, b;

	while (!wk->vm.error) {
		if (log_should_print(log_debug)) {
			LL("%-50s", vm_dis_inst(wk, wk->vm.code.e, wk->vm.ip).text);
			object_stack_print(wk, &wk->vm.stack);
		}

		switch ((enum op)wk->vm.code.e[wk->vm.ip++]) {
		case op_constant:
			a = vm_get_constant(wk->vm.code.e, &wk->vm.ip);
			object_stack_push(wk, a);
			break;
		case op_constant_list: {
			uint32_t i, len = vm_get_constant(wk->vm.code.e, &wk->vm.ip);
			make_obj(wk, &b, obj_array);
			for (i = 0; i < len; ++i) {
				obj_array_push(wk, b, object_stack_peek(&wk->vm.stack, len - i));
			}

			object_stack_discard(&wk->vm.stack, len);
			object_stack_push(wk, b);
			break;
		}
		case op_constant_dict: {
			uint32_t i, len = vm_get_constant(wk->vm.code.e, &wk->vm.ip);
			make_obj(wk, &b, obj_dict);
			for (i = 0; i < len; ++i) {
				obj_dict_set(wk,
					b,
					object_stack_peek(&wk->vm.stack, (len - i) * 2 - 1),
					object_stack_peek(&wk->vm.stack, (len - i) * 2));
			}

			object_stack_discard(&wk->vm.stack, len * 2);
			object_stack_push(wk, b);
			break;
		}
		case op_constant_func: {
			obj c;
			struct obj_capture *capture;

			a = vm_get_constant(wk->vm.code.e, &wk->vm.ip);

			make_obj(wk, &c, obj_capture);
			capture = get_obj_capture(wk, c);

			capture->func = get_obj_func(wk, a);
			capture->scope_stack = wk->vm.behavior.scope_stack_dup(wk, wk->vm.scope_stack);

			object_stack_push(wk, c);
			break;
		}
		case op_add: {
			b = object_stack_pop(&wk->vm.stack);
			a = object_stack_pop(&wk->vm.stack);
			binop_disabler_check(a, b);

			enum obj_type a_t = get_obj_type(wk, a), b_t = get_obj_type(wk, b);
			if (!(a_t == obj_array || a_t == b_t)) {
				goto op_add_type_err;
			}

			obj res = 0;

			switch (a_t) {
			case obj_number:
				make_obj(wk, &res, obj_number);
				set_obj_number(wk, res, get_obj_number(wk, a) + get_obj_number(wk, b));
				break;
			case obj_string: res = str_join(wk, a, b); break;
			case obj_array:
				obj_array_dup(wk, a, &res);
				if (b_t == obj_array) {
					obj_array_extend(wk, res, b);
				} else {
					obj_array_push(wk, res, b);
				}
				break;
			case obj_dict: obj_dict_merge(wk, a, b, &res); break;
			default:
op_add_type_err:
				vm_error(wk, "+ not defined for %s and %s", obj_type_to_s(a_t), obj_type_to_s(b_t));
				break;
			}

			object_stack_push(wk, res);
			break;
		}
		case op_sub: {
			b = object_stack_pop(&wk->vm.stack);
			a = object_stack_pop(&wk->vm.stack);
			binop_disabler_check(a, b);

			enum obj_type a_t = get_obj_type(wk, a), b_t = get_obj_type(wk, b);
			if (!(a_t == obj_number && a_t == b_t)) {
				vm_error(wk, "- not defined for %s and %s", obj_type_to_s(a_t), obj_type_to_s(b_t));
				break;
			}

			obj res;
			make_obj(wk, &res, obj_number);
			set_obj_number(wk, res, get_obj_number(wk, a) - get_obj_number(wk, b));

			object_stack_push(wk, res);
			break;
		}
		case op_mul: {
			b = object_stack_pop(&wk->vm.stack);
			a = object_stack_pop(&wk->vm.stack);
			binop_disabler_check(a, b);

			enum obj_type a_t = get_obj_type(wk, a), b_t = get_obj_type(wk, b);
			if (!(a_t == obj_number && a_t == b_t)) {
				vm_error(wk, "* not defined for %s and %s", obj_type_to_s(a_t), obj_type_to_s(b_t));
				break;
			}

			obj res;
			make_obj(wk, &res, obj_number);
			set_obj_number(wk, res, get_obj_number(wk, a) * get_obj_number(wk, b));

			object_stack_push(wk, res);
			break;
		}
		case op_div: {
			b = object_stack_pop(&wk->vm.stack);
			a = object_stack_pop(&wk->vm.stack);
			binop_disabler_check(a, b);

			enum obj_type a_t = get_obj_type(wk, a), b_t = get_obj_type(wk, b);
			if (!(a_t == b_t)) {
				goto op_div_type_err;
			}

			obj res = 0;

			switch (a_t) {
			case obj_number:
				make_obj(wk, &res, obj_number);
				set_obj_number(wk, res, get_obj_number(wk, a) / get_obj_number(wk, b));
				break;
			case obj_string: {
				const struct str *ss1 = get_str(wk, a), *ss2 = get_str(wk, b);

				if (str_has_null(ss1)) {
					vm_error(wk, "%o is an invalid path", a);
					break;
				} else if (str_has_null(ss2)) {
					vm_error(wk, "%o is an invalid path", b);
					break;
				}

				SBUF(buf);
				path_join(wk, &buf, ss1->s, ss2->s);
				res = sbuf_into_str(wk, &buf);
				break;
			}
			default:
op_div_type_err:
				vm_error(wk, "/ not defined for %s and %s", obj_type_to_s(a_t), obj_type_to_s(b_t));
				break;
			}

			object_stack_push(wk, res);
			break;
		}
		case op_mod: {
			b = object_stack_pop(&wk->vm.stack);
			a = object_stack_pop(&wk->vm.stack);
			binop_disabler_check(a, b);

			enum obj_type a_t = get_obj_type(wk, a), b_t = get_obj_type(wk, b);
			if (!(a_t == obj_number && a_t == b_t)) {
				vm_error(wk, "%% not defined for %s and %s", obj_type_to_s(a_t), obj_type_to_s(b_t));
				break;
			}

			obj res;
			make_obj(wk, &res, obj_number);
			set_obj_number(wk, res, get_obj_number(wk, a) % get_obj_number(wk, b));

			object_stack_push(wk, res);
			break;
		}
		case op_and:
			b = object_stack_pop(&wk->vm.stack);
			a = object_stack_pop(&wk->vm.stack);
			binop_disabler_check(a, b);

			break;
		case op_or:
			b = object_stack_pop(&wk->vm.stack);
			a = object_stack_pop(&wk->vm.stack);
			binop_disabler_check(a, b);

			break;
		case op_lt:
			b = object_stack_pop(&wk->vm.stack);
			a = object_stack_pop(&wk->vm.stack);
			binop_disabler_check(a, b);

			break;
		case op_gt:
			b = object_stack_pop(&wk->vm.stack);
			a = object_stack_pop(&wk->vm.stack);
			binop_disabler_check(a, b);

			break;
		case op_in:
			b = object_stack_pop(&wk->vm.stack);
			a = object_stack_pop(&wk->vm.stack);
			binop_disabler_check(a, b);

			bool res = false;

			switch (get_obj_type(wk, b)) {
			case obj_array: res = obj_array_in(wk, b, a); break;
			case obj_dict:
				if (!typecheck(wk, wk->vm.ip - 1, a, obj_string)) {
					break;
				}

				res = obj_dict_in(wk, b, a);
				break;
			case obj_string:
				if (!typecheck(wk, wk->vm.ip - 1, a, obj_string)) {
					break;
				}

				const struct str *r = get_str(wk, b), *l = get_str(wk, a);
				if (memmem(r->s, r->len, l->s, l->len)) {
					res = true;
				} else {
					res = false;
				}
				break;
			default: vm_error(wk, "'in' not supported for %s", obj_type_to_s(get_obj_type(wk, a))); break;
			}

			a = res ? obj_bool_true : obj_bool_false;
			object_stack_push(wk, a);
			break;
		case op_eq:
			b = object_stack_pop(&wk->vm.stack);
			a = object_stack_pop(&wk->vm.stack);
			binop_disabler_check(a, b);

			a = obj_equal(wk, a, b) ? obj_bool_true : obj_bool_false;
			object_stack_push(wk, a);
			break;
		case op_not:
			a = object_stack_pop(&wk->vm.stack);

			if (!typecheck(wk, wk->vm.ip, a, obj_bool)) {
				break;
			}

			a = get_obj_bool(wk, a) ? obj_bool_false : obj_bool_true;
			object_stack_push(wk, a);
			break;
		case op_negate:
			a = object_stack_pop(&wk->vm.stack);

			if (!typecheck(wk, wk->vm.ip, a, obj_number)) {
				break;
			}

			make_obj(wk, &b, obj_number);
			set_obj_number(wk, b, get_obj_number(wk, a) * -1);
			object_stack_push(wk, b);
			break;
		case op_stringify:
			a = object_stack_pop(&wk->vm.stack);

			if (!coerce_string(wk, wk->vm.ip - 1, a, &b)) {
				break;
			}

			object_stack_push(wk, b);
			break;
		case op_store: {
			b = object_stack_peek(&wk->vm.stack, 1);

			switch (get_obj_type(wk, b)) {
			case obj_environment:
			case obj_configuration_data: {
				obj cloned;
				if (!obj_clone(wk, wk, b, &cloned)) {
					UNREACHABLE;
				}

				b = cloned;
				break;
			}
			case obj_dict: {
				obj dup;
				obj_dict_dup(wk, b, &dup);
				b = dup;
				break;
			}
			case obj_array: {
				obj dup;
				obj_array_dup(wk, b, &dup);
				b = dup;
			}
			default: break;
			}

			a = vm_get_constant(wk->vm.code.e, &wk->vm.ip);
			wk->vm.behavior.assign_variable(wk, get_str(wk, a)->s, b, 0, assign_local);
			/* LO("%o <= %o\n", a, b); */
			break;
		}
		case op_add_store: {
			b = object_stack_pop(&wk->vm.stack);
			obj a_id = vm_get_constant(wk->vm.code.e, &wk->vm.ip);
			const struct str *id = get_str(wk, a_id);
			if (!wk->vm.behavior.get_variable(wk, id->s, &a)) {
				vm_error(wk, "undefined object %s", get_cstr(wk, a_id));
				break;
			}

			enum obj_type a_t = get_obj_type(wk, a), b_t = get_obj_type(wk, b);
			if (!(a_t == obj_array || a_t == b_t)) {
				goto op_add_store_type_err;
			}

			switch (a_t) {
			case obj_number: {
				obj res;
				make_obj(wk, &res, obj_number);
				set_obj_number(wk, res, get_obj_number(wk, a) + get_obj_number(wk, b));
				a = res;
				wk->vm.behavior.assign_variable(wk, id->s, a, 0, assign_reassign);
				break;
			}
			case obj_string:
				// TODO: could use str_appn, but would have to dup on store
				a = str_join(wk, a, b);
				wk->vm.behavior.assign_variable(wk, id->s, a, 0, assign_reassign);
				break;
			case obj_array:
				if (b_t == obj_array) {
					obj_array_extend(wk, a, b);
				} else {
					obj_array_push(wk, a, b);
				}
				break;
			case obj_dict: obj_dict_merge_nodup(wk, a, b); break;
			default:
op_add_store_type_err:
				vm_error(wk, "+= not defined for %s and %s", obj_type_to_s(a_t), obj_type_to_s(b_t));
				break;
			}

			object_stack_push(wk, a);
			break;
		}
		case op_load: {
			a = vm_get_constant(wk->vm.code.e, &wk->vm.ip);

			if (!wk->vm.behavior.get_variable(wk, get_str(wk, a)->s, &b)) {
				vm_error(wk, "undefined object %s", get_cstr(wk, a));
				break;
			}

			/* LO("%o <= %o\n", b, a); */
			object_stack_push(wk, b);
			break;
		}
		case op_index: {
			int64_t i;
			obj res = 0;
			vm_pop(b);
			a = object_stack_pop(&wk->vm.stack);
			binop_disabler_check(a, b);

			switch (get_obj_type(wk, a)) {
			case obj_array: {
				if (!typecheck(wk, b_ip, b, obj_number)) {
					break;
				}
				i = get_obj_number(wk, b);

				if (!boundscheck(wk, b_ip, get_obj_array(wk, a)->len, &i)) {
					break;
				}

				obj_array_index(wk, a, i, &res);
				break;
			}
			case obj_dict: {
				if (!typecheck(wk, b_ip, b, obj_string)) {
					break;
				}

				if (!obj_dict_index(wk, a, b, &res)) {
					vm_error_at(wk, b_ip, "key not in dictionary: %o", b);
					break;
				}
				break;
			}
			case obj_custom_target: {
				if (!typecheck(wk, b_ip, b, obj_number)) {
					break;
				}
				i = get_obj_number(wk, b);

				struct obj_custom_target *tgt = get_obj_custom_target(wk, a);
				struct obj_array *arr = get_obj_array(wk, tgt->output);

				if (!boundscheck(wk, b_ip, arr->len, &i)) {
					break;
				}

				obj_array_index(wk, tgt->output, i, &res);
				break;
			}
			case obj_string: {
				if (!typecheck(wk, b_ip, b, obj_number)) {
					break;
				}
				i = get_obj_number(wk, b);

				const struct str *s = get_str(wk, a);
				if (!boundscheck(wk, b_ip, s->len, &i)) {
					break;
				}

				res = make_strn(wk, &s->s[i], 1);
				break;
			}
			default:
				vm_error_at(wk, b_ip, "index unsupported for %s", obj_type_to_s(get_obj_type(wk, a)));
				break;
			}

			object_stack_push(wk, res);
			break;
		}
		case op_call: {
			uint32_t i;
			struct obj_capture *capture;

			a = object_stack_pop(&wk->vm.stack);
			wk->vm.nargs = vm_get_constant(wk->vm.code.e, &wk->vm.ip);
			wk->vm.nkwargs = vm_get_constant(wk->vm.code.e, &wk->vm.ip);

			capture = get_obj_capture(wk, a);

			if (!pop_args(wk, capture->func->an, capture->func->akw)) {
				break;
			}

			arr_push(&wk->vm.call_stack,
				&(struct call_frame){
					.type = call_frame_type_func,
					.return_ip = wk->vm.ip,
					.scope_stack = wk->vm.scope_stack,
				});

			wk->vm.scope_stack = capture->scope_stack;

			wk->vm.behavior.push_local_scope(wk);

			for (i = 0; capture->func->an[i].type != ARG_TYPE_NULL; ++i) {
				wk->vm.behavior.assign_variable(
					wk, capture->func->an[i].name, capture->func->an[i].val, 0, assign_local);
			}

			for (i = 0; capture->func->akw[i].key; ++i) {
				wk->vm.behavior.assign_variable(
					wk, capture->func->akw[i].key, capture->func->akw[i].val, 0, assign_local);
			}

			wk->vm.ip = capture->func->entry;
			break;
		}
		case op_call_method: {
			obj f = 0;
			uint32_t idx;

			b = object_stack_pop(&wk->vm.stack);
			wk->vm.nargs = vm_get_constant(wk->vm.code.e, &wk->vm.ip);
			wk->vm.nkwargs = vm_get_constant(wk->vm.code.e, &wk->vm.ip);
			a = vm_get_constant(wk->vm.code.e, &wk->vm.ip);

			if (!func_lookup(wk, b, get_str(wk, a)->s, &idx, &f)) {
				break;
			}

			native_funcs[idx].func(wk, b, 0, &a);
			object_stack_push(wk, a);
			break;
		}
		case op_call_native: {
			wk->vm.nargs = vm_get_constant(wk->vm.code.e, &wk->vm.ip);
			wk->vm.nkwargs = vm_get_constant(wk->vm.code.e, &wk->vm.ip);

			b = vm_get_constant(wk->vm.code.e, &wk->vm.ip);
			native_funcs[b].func(wk, 0, 0, &a);
			object_stack_push(wk, a);
			break;
		}
		case op_iterator: {
			obj iter;
			struct obj_iterator *iterator;

			a = object_stack_pop(&wk->vm.stack);
			enum obj_type a_type = get_obj_type(wk, a);

			if (a_type == obj_iterator) {
				// already an iterator!
				object_stack_push(wk, a);
				break;
			}

			make_obj(wk, &iter, obj_iterator);
			object_stack_push(wk, iter);
			iterator = get_obj_iterator(wk, iter);

			switch (get_obj_type(wk, a)) {
			case obj_array:
				iterator->type = obj_iterator_type_array;
				iterator->data.array = get_obj_array(wk, a);
				if (!iterator->data.array->len) {
					// TODO: update this when we implement array_elem
					iterator->data.array = 0;
				}
				break;
			case obj_dict: {
				struct obj_dict *d = get_obj_dict(wk, a);
				if (d->flags & obj_dict_flag_big) {
					iterator->type = obj_iterator_type_dict_big;
					iterator->data.dict_big.h
						= bucket_arr_get(&wk->vm.objects.dict_hashes, d->data);
				} else {
					iterator->type = obj_iterator_type_dict_small;
					iterator->data.dict_small = bucket_arr_get(&wk->vm.objects.dict_elems, d->data);
				}
				break;
			}
			default: {
				UNREACHABLE;
			}
			}
			break;
		}
		case op_iterator_next: {
			obj key = 0, val;
			struct obj_iterator *iterator;
			iterator = get_obj_iterator(wk, object_stack_peek(&wk->vm.stack, 1));

			switch (iterator->type) {
			case obj_iterator_type_array:
				if (!iterator->data.array) {
					val = 0;
				} else {
					val = iterator->data.array->val;
					iterator->data.array = iterator->data.array->have_next ?
								       get_obj_array(wk, iterator->data.array->next) :
								       0;
				}
				break;
			case obj_iterator_type_range:
				if (iterator->data.range.start >= iterator->data.range.stop) {
					val = 0;
				} else {
					make_obj(wk, &val, obj_number);
					set_obj_number(wk, val, iterator->data.range.start);
					iterator->data.range.start += iterator->data.range.step;
				}
				break;
			case obj_iterator_type_dict_small:
				if (!iterator->data.dict_small) {
					val = 0;
				} else {
					key = iterator->data.dict_small->key;
					val = iterator->data.dict_small->val;
					if (iterator->data.dict_small->next) {
						iterator->data.dict_small = bucket_arr_get(
							&wk->vm.objects.dict_elems, iterator->data.dict_small->next);
					} else {
						iterator->data.dict_small = 0;
					}
				}
				break;
			case obj_iterator_type_dict_big:
				if (iterator->data.dict_big.i >= iterator->data.dict_big.h->keys.len) {
					val = 0;
				} else {
					void *k = arr_get(&iterator->data.dict_big.h->keys, iterator->data.dict_big.i);
					uint64_t *v = hash_get(iterator->data.dict_big.h, k);
					key = *v >> 32;
					val = *v & 0xffffffff;
				}
				break;
			default: UNREACHABLE;
			}

			if (key) {
				object_stack_push(wk, key);
			}
			object_stack_push(wk, val);
			break;
		}
		case op_pop: a = object_stack_pop(&wk->vm.stack); break;
		case op_jmp_if_null:
			a = object_stack_peek(&wk->vm.stack, 1);
			b = vm_get_constant(wk->vm.code.e, &wk->vm.ip);
			if (!a) {
				object_stack_discard(&wk->vm.stack, 1);
				wk->vm.ip = b;
			}
			break;
		case op_jmp_if_false:
			a = object_stack_pop(&wk->vm.stack);
			b = vm_get_constant(wk->vm.code.e, &wk->vm.ip);
			if (!get_obj_bool(wk, a)) {
				wk->vm.ip = b;
			}
			break;
		case op_jmp:
			a = vm_get_constant(wk->vm.code.e, &wk->vm.ip);
			wk->vm.ip = a;
			break;
		case op_return: {
			struct call_frame frame;
			arr_pop(&wk->vm.call_stack, &frame);
			wk->vm.ip = frame.return_ip;

			wk->vm.behavior.pop_local_scope(wk);
			wk->vm.scope_stack = frame.scope_stack;

			if (frame.type == call_frame_type_eval) {
				return;
			}
			break;
		}
		case op_eval_file: {
			a = object_stack_pop(&wk->vm.stack);

			if (!typecheck(wk, wk->vm.ip, a, obj_string)) {
				break;
			}

			uint32_t src_idx = arr_push(&wk->vm.src, &(struct source){ 0 });
			struct source *src = arr_get(&wk->vm.src, src_idx);

			if (!fs_read_entire_file(get_cstr(wk, a), src)) {
				break;
			}

			uint32_t entry;
			if (!compile(wk, src, 0, &entry)) {
				break;
			}

			arr_push(&wk->vm.call_stack,
				&(struct call_frame){
					.type = call_frame_type_eval,
					.return_ip = wk->vm.ip,
					.scope_stack = wk->vm.scope_stack,
				});

			wk->vm.ip = entry;
			break;
		}
		case op_typecheck: {
			entry = object_stack_peek_entry(&wk->vm.stack, 1);
			typecheck(wk, entry->ip, entry->o, vm_get_constant(wk->vm.code.e, &wk->vm.ip));
			break;
		}
		}
	}
	/* stack_pop(&stack, a); */
	/* LO("%o\n", a); */
	/* printf("%04x %02x\n", i, code[i]); */
}

/******************************************************************************
 * vm behavior functions
 ******************************************************************************/

struct vm_check_scope_ctx {
	const char *name;
	obj res, scope;
	bool found;
};

static enum iteration_result
vm_check_scope(struct workspace *wk, void *_ctx, obj scope)
{
	struct vm_check_scope_ctx *ctx = _ctx;
	if (obj_dict_index_str(wk, scope, ctx->name, &ctx->res)) {
		ctx->scope = scope;
		ctx->found = true;
	}
	return ir_cont;
}

static bool
vm_get_local_variable(struct workspace *wk, const char *name, obj *res, obj *scope)
{
	struct vm_check_scope_ctx ctx = { .name = name };
	obj_array_foreach(wk, wk->vm.scope_stack, &ctx, vm_check_scope);

	if (ctx.found) {
		*res = ctx.res;
		*scope = ctx.scope;
		return true;
	}

	return false;
}

static bool
vm_get_variable(struct workspace *wk, const char *name, obj *res)
{
	obj o, _scope;

	if (vm_get_local_variable(wk, name, &o, &_scope)) {
		*res = o;
		return true;
	} else {
		return false;
	}
}

static enum iteration_result
vm_scope_stack_dup_iter(struct workspace *wk, void *_ctx, obj v)
{
	obj *r = _ctx;
	obj scope;
	obj_dict_dup(wk, v, &scope);
	obj_array_push(wk, *r, scope);
	return ir_cont;
}

static obj
vm_scope_stack_dup(struct workspace *wk, obj scope_stack)
{
	obj r;
	make_obj(wk, &r, obj_array);

	obj_array_foreach(wk, scope_stack, &r, vm_scope_stack_dup_iter);
	return r;
}

static void
vm_push_local_scope(struct workspace *wk)
{
	obj scope;
	make_obj(wk, &scope, obj_dict);
	obj_array_push(wk, wk->vm.scope_stack, scope);
}

static void
vm_pop_local_scope(struct workspace *wk)
{
	obj_array_pop(wk, wk->vm.scope_stack);
}

static void
vm_unassign_variable(struct workspace *wk, const char *name)
{
	obj _, scope;
	if (!vm_get_local_variable(wk, name, &_, &scope)) {
		return;
	}

	obj_dict_del_str(wk, scope, name);
}

static void
vm_assign_variable(struct workspace *wk, const char *name, obj o, uint32_t ip, enum variable_assignment_mode mode)
{
	obj scope = 0;
	if (mode == assign_reassign) {
		obj _;
		if (!vm_get_local_variable(wk, name, &_, &scope)) {
			UNREACHABLE;
		}
	} else {
		uint32_t len = get_obj_array(wk, wk->vm.scope_stack)->len;
		obj_array_index(wk, wk->vm.scope_stack, len - 1, &scope);
	}

	obj_dict_set(wk, scope, make_str(wk, name), o);

	if (wk->vm.dbg_state.watched && obj_array_in(wk, wk->vm.dbg_state.watched, make_str(wk, name))) {
		LOG_I("watched variable \"%s\" changed", name);
		repl(wk, true);
	}
}

/******************************************************************************
 * init / destroy
 ******************************************************************************/

void
vm_init_objects(struct workspace *wk)
{
	bucket_arr_init(&wk->vm.objects.chrs, 4096, 1);
	bucket_arr_init(&wk->vm.objects.objs, 1024, sizeof(struct obj_internal));
	bucket_arr_init(&wk->vm.objects.dict_elems, 1024, sizeof(struct obj_dict_elem));
	bucket_arr_init(&wk->vm.objects.dict_hashes, 16, sizeof(struct hash));

	const struct {
		uint32_t item_size;
		uint32_t bucket_size;
	} sizes[] = {
		[obj_number] = { sizeof(int64_t), 1024 },
		[obj_string] = { sizeof(struct str), 1024 },
		[obj_compiler] = { sizeof(struct obj_compiler), 4 },
		[obj_array] = { sizeof(struct obj_array), 2048 },
		[obj_dict] = { sizeof(struct obj_dict), 512 },
		[obj_build_target] = { sizeof(struct obj_build_target), 16 },
		[obj_custom_target] = { sizeof(struct obj_custom_target), 16 },
		[obj_subproject] = { sizeof(struct obj_subproject), 16 },
		[obj_dependency] = { sizeof(struct obj_dependency), 16 },
		[obj_external_program] = { sizeof(struct obj_external_program), 32 },
		[obj_python_installation] = { sizeof(struct obj_python_installation), 32 },
		[obj_run_result] = { sizeof(struct obj_run_result), 32 },
		[obj_configuration_data] = { sizeof(struct obj_configuration_data), 16 },
		[obj_test] = { sizeof(struct obj_test), 64 },
		[obj_module] = { sizeof(struct obj_module), 16 },
		[obj_install_target] = { sizeof(struct obj_install_target), 128 },
		[obj_environment] = { sizeof(struct obj_environment), 4 },
		[obj_include_directory] = { sizeof(struct obj_include_directory), 16 },
		[obj_option] = { sizeof(struct obj_option), 32 },
		[obj_generator] = { sizeof(struct obj_generator), 16 },
		[obj_generated_list] = { sizeof(struct obj_generated_list), 16 },
		[obj_alias_target] = { sizeof(struct obj_alias_target), 4 },
		[obj_both_libs] = { sizeof(struct obj_both_libs), 4 },
		[obj_typeinfo] = { sizeof(struct obj_typeinfo), 32 },
		[obj_func] = { sizeof(struct obj_func), 32 },
		[obj_capture] = { sizeof(struct obj_func), 64 },
		[obj_source_set] = { sizeof(struct obj_source_set), 4 },
		[obj_source_configuration] = { sizeof(struct obj_source_configuration), 4 },
		[obj_iterator] = { sizeof(struct obj_iterator), 32 },
	};

	uint32_t i;
	for (i = _obj_aos_start; i < obj_type_count; ++i) {
		bucket_arr_init(&wk->vm.objects.obj_aos[i - _obj_aos_start], sizes[i].bucket_size, sizes[i].item_size);
	}

	bucket_arr_pushn(&wk->vm.objects.dict_elems, 0, 0, 1); // reserve dict_elem 0 as a null element

	hash_init(&wk->vm.objects.obj_hash, 128, sizeof(obj));
	hash_init_str(&wk->vm.objects.str_hash, 128);

	/* default objects */

	obj id;
	make_obj(wk, &id, obj_null);
	assert(id == 0);

	make_obj(wk, &id, obj_disabler);
	assert(id == disabler_id);

	make_obj(wk, &id, obj_bool);
	assert(id == obj_bool_true);
	set_obj_bool(wk, id, true);

	make_obj(wk, &id, obj_bool);
	assert(id == obj_bool_false);
	set_obj_bool(wk, id, false);
}

void
vm_init(struct workspace *wk)
{
	wk->vm = (struct vm){ 0 };

	/* core vm runtime */
	object_stack_init(&wk->vm.stack);
	arr_init(&wk->vm.call_stack, 64, sizeof(struct call_frame));
	arr_init(&wk->vm.code, 4 * 1024, 1);
	arr_init(&wk->vm.src, 64, sizeof(struct source));
	arr_init(&wk->vm.locations, 1024, sizeof(struct source_location_mapping));

	/* compiler state */
	arr_init(&wk->vm.compiler_state.node_stack, 4096, sizeof(struct node *));
	arr_init(&wk->vm.compiler_state.jmp_stack, 1024, sizeof(uint32_t));
	bucket_arr_init(&wk->vm.compiler_state.nodes, 2048, sizeof(struct node));

	/* behavior pointers */
	wk->vm.behavior = (struct vm_behavior){
		.assign_variable = vm_assign_variable,
		.unassign_variable = vm_unassign_variable,
		.push_local_scope = vm_push_local_scope,
		.pop_local_scope = vm_pop_local_scope,
		.scope_stack_dup = vm_scope_stack_dup,
		.get_variable = vm_get_variable,
		.eval_project_file = eval_project_file,
	};

	/* objects */
	vm_init_objects(wk);

	/* func impl tables */
	build_func_impl_tables();

	/* default scope */
	make_obj(wk, &wk->vm.default_scope_stack, obj_array);
	obj scope;
	make_obj(wk, &scope, obj_dict);
	obj_array_push(wk, wk->vm.default_scope_stack, scope);

	obj id;
	make_obj(wk, &id, obj_meson);
	obj_dict_set(wk, scope, make_str(wk, "meson"), id);

	make_obj(wk, &id, obj_machine);
	obj_dict_set(wk, scope, make_str(wk, "host_machine"), id);
	obj_dict_set(wk, scope, make_str(wk, "build_machine"), id);
	obj_dict_set(wk, scope, make_str(wk, "target_machine"), id);

	wk->vm.scope_stack = wk->vm.behavior.scope_stack_dup(wk, wk->vm.default_scope_stack);

	/* initial code segment */
	compiler_write_initial_code_segment(wk);
}

void
vm_destroy_objects(struct workspace *wk)
{
	uint32_t i;
	struct bucket_arr *ba = &wk->vm.objects.obj_aos[obj_string - _obj_aos_start];
	for (i = 0; i < ba->len; ++i) {
		struct str *s = bucket_arr_get(ba, i);
		if (s->flags & str_flag_big) {
			z_free((void *)s->s);
		}
	}

	for (i = _obj_aos_start; i < obj_type_count; ++i) {
		bucket_arr_destroy(&wk->vm.objects.obj_aos[i - _obj_aos_start]);
	}

	for (i = 0; i < wk->vm.objects.dict_hashes.len; ++i) {
		struct hash *h = bucket_arr_get(&wk->vm.objects.dict_hashes, i);
		hash_destroy(h);
	}

	bucket_arr_destroy(&wk->vm.objects.chrs);
	bucket_arr_destroy(&wk->vm.objects.objs);
	bucket_arr_destroy(&wk->vm.objects.dict_elems);
	bucket_arr_destroy(&wk->vm.objects.dict_hashes);

	hash_destroy(&wk->vm.objects.obj_hash);
	hash_destroy(&wk->vm.objects.str_hash);
}

void
vm_destroy(struct workspace *wk)
{
	vm_destroy_objects(wk);
}

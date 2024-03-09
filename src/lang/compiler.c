/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 */

#include "compat.h"

#include <inttypes.h>
#include <string.h>

#include "buf_size.h"
#include "functions/common.h"
#include "lang/compiler.h"
#include "lang/lexer.h"
#include "lang/typecheck.h"
#include "lang/workspace.h"
#include "platform/mem.h"

/******************************************************************************
 * parser
 ******************************************************************************/

enum node_type {
	node_type_stmt,
	node_type_bool,
	node_type_id,
	node_type_id_lit,
	node_type_number,
	node_type_string,
	node_type_continue,
	node_type_break,
	node_type_args,
	node_type_dict,
	node_type_list,
	node_type_kw,
	node_type_or,
	node_type_and,
	node_type_comparison,
	node_type_add,
	node_type_sub,
	node_type_div,
	node_type_mul,
	node_type_not,
	node_type_index,
	node_type_method,
	node_type_call,
	node_type_assign,
	node_type_plusassign,
	node_type_foreach,
	node_type_foreach_args,
	node_type_if,
	node_type_u_minus,
	node_type_ternary,
	node_type_stringify,
	node_type_func_def,
	node_type_return,
};

struct node {
	union literal_data data;
	struct node *l, *r;
	struct source_location location;
	enum node_type type;
};

struct parser {
	struct token previous, current, next;
	struct lexer lexer;
	struct workspace *wk;
	struct source *src;
	struct bucket_arr *nodes;
	uint32_t mode;

	struct {
		char msg[2048];
		uint32_t len;
		uint32_t count;
	} err;
};

enum parse_precedence {
	parse_precedence_none,
	parse_precedence_assignment,  // =
	parse_precedence_or,          // OR
	parse_precedence_and,         // AND
	parse_precedence_equality,    // == !=
	parse_precedence_comparison,  // < > <= >=
	parse_precedence_term,        // + -
	parse_precedence_factor,      // * /
	parse_precedence_unary,       // ! -
	parse_precedence_call,        // . ()
	parse_precedence_primary,
};

typedef struct node *((*parse_prefix_fn)(struct parser *p));
typedef struct node *((*parse_infix_fn)(struct parser *p, struct node *l));

struct parse_rule {
	parse_prefix_fn prefix;
	parse_infix_fn infix;
	enum parse_precedence precedence;
};

static const struct parse_rule *parse_rules;

static struct node *parse_prec(struct parser *p, enum parse_precedence prec);
static struct node *parse_expr(struct parser *p);

/*******************************************************************************
 * misc api functions
 ******************************************************************************/

static const char *
node_type_to_s(enum node_type t)
{
#define nt(__n) case node_type_ ##__n: return #__n;

	switch (t) {
	nt(bool);
	nt(id);
	nt(id_lit);
	nt(number);
	nt(string);
	nt(continue);
	nt(break);
	nt(args);
	nt(list);
	nt(dict);
	nt(kw);
	nt(or);
	nt(and);
	nt(comparison);
	nt(add);
	nt(sub);
	nt(div);
	nt(mul);
	nt(not);
	nt(index);
	nt(method);
	nt(call);
	nt(assign);
	nt(plusassign);
	nt(foreach_args);
	nt(foreach);
	nt(if);
	nt(u_minus);
	nt(ternary);
	nt(stmt);
	nt(stringify);
	nt(func_def);
	nt(return);
	}

	UNREACHABLE_RETURN;

#undef nt
}

static const char *
node_to_s(struct workspace *wk, const void *_n)
{
	const struct node *n = _n;

	static char buf[BUF_SIZE_S + 1];
	uint32_t i = 0;

	i += snprintf(&buf[i], BUF_SIZE_S - i, "%s", node_type_to_s(n->type));

	switch (n->type) {
	case node_type_id:
		i += snprintf(&buf[i], BUF_SIZE_S - i, ":%s", get_cstr(wk, n->data.str));
		break;
	case node_type_string:
		i += obj_snprintf(wk, &buf[i], BUF_SIZE_S - i, ":%o", n->data.str);
		break;
	case node_type_number:
		i += snprintf(&buf[i], BUF_SIZE_S - i, ":%" PRId64, n->data.num);
		break;
	default:
		break;
	}

	return buf;
}

static void
print_ast_at(struct workspace *wk, struct node *n, uint32_t d, char label)
{
	uint32_t i;

	for (i = 0; i < d; ++i) {
		printf("  ");
	}

	printf("%c:%s\n", label, node_to_s(wk, n));

	if (n->l) { print_ast_at(wk, n->l, d + 1, 'l'); }
	if (n->r) { print_ast_at(wk, n->r, d + 1, 'r'); }
}

static void
print_ast(struct workspace *wk, struct node *root)
{
	print_ast_at(wk, root, 0, 'l');
}

/******************************************************************************
 * error handling
 ******************************************************************************/

static void
parse_diagnostic(struct parser *p, struct source_location *l, enum log_level lvl)
{
	/* if (p->mode & pm_quiet) { */
	/* 	return; */
	/* } */

	if (!l) {
		l = &p->current.location;
	}

	error_message(p->src, *l, lvl, p->err.msg);
}

#if 0
static void
parse_error_begin(struct parser *p)
{
	p->err.len = 0;
}

MUON_ATTR_FORMAT(printf, 2, 3)
static void
parse_error_push(struct parser *p, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	p->err.len += vsnprintf(&p->err.msg[p->err.len], ARRAY_LEN(p->err.msg) - p->err.len, fmt, args);
	va_end(args);
}

static void
parse_error_end(struct parser *p, struct source_location *l)
{
	parse_diagnostic(p, l, log_error);
}
#endif

MUON_ATTR_FORMAT(printf, 3, 4)
static void
parse_error(struct parser *p, struct source_location *l, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(p->err.msg, ARRAY_LEN(p->err.msg), fmt, args);
	va_end(args);

	parse_diagnostic(p, l, log_error);
}

static void
parse_advance(struct parser *p)
{
	p->previous = p->current;
	lexer_next(&p->lexer, &p->current);

	LL("previous: %s, current: ", token_to_s(p->wk, &p->previous));
	printf("%s\n", token_to_s(p->wk, &p->current));
}

static bool
parse_accept(struct parser *p, enum token_type type)
{
	if (p->current.type == type) {
		parse_advance(p);
		return true;
	}

	return false;
}

static bool
parse_match(struct parser *p, enum token_type types[], uint32_t types_len)
{
	if (!types_len) {
		return false;
	} else if (types_len == 1) {
		return parse_accept(p, types[0]);
	}

	if (p->current.type != types[0]) {
		return false;
	}

	struct lexer lexer_peek = p->lexer;
	struct token next;

	uint32_t i;
	for (i = 1; i < types_len; ++i) {
		lexer_next(&lexer_peek, &next);
		if (next.type != types[i]) {
			return false;
		}
	}

	return true;
}


#if 0
static bool
parse_expect_many(struct parser *p, enum token_type types[], uint32_t types_len)
{
	uint32_t i;
	for (i = 0; i < types_len; ++i) {
		if (p->current.type == types[i]) {
			parse_advance(p);
			return true;
		}
	}

	parse_error_begin(p);
	parse_error_push(p, "expected <");
	for (i = 0; i < types_len; ++i) {
		parse_error_push(p, "%s", token_type_to_s(types[i]));
		if (i < types_len - 1) {
			parse_error_push(p, " or ");
		}
	}
	parse_error_push(p, "> got <%s>\n", token_type_to_s(p->current.type));
	parse_error_end(p, 0);

	return false;
}
#endif

static bool
parse_expect(struct parser *p, enum token_type type)
{
	if (p->current.type != type) {
		parse_error(p, 0, "expected <%s> got <%s>", token_type_to_s(type), token_type_to_s(p->current.type));
		return false;
	}

	parse_advance(p);
	return true;
}

static struct node *
make_node(struct parser *p, struct node *n)
{
	n = bucket_arr_push(p->nodes, n);
	if (p->previous.type) {
		n->location = p->previous.location;
		n->data = p->previous.data;
	}
	return n;
}

static struct node *
make_node_t(struct parser *p, enum node_type t)
{
	return make_node(p, &(struct node) { .type = t });
}

/******************************************************************************
 * parsing functions
 ******************************************************************************/

static struct node *
parse_number(struct parser *p)
{
	return make_node_t(p, node_type_number);
}

static struct node *
parse_id(struct parser *p)
{
	return make_node_t(p, node_type_id);
}

static struct node *
parse_string(struct parser *p)
{
	return make_node_t(p, node_type_string);
}

static struct node *
parse_fstring(struct parser *p)
{
	uint32_t i, j;
	const struct str *fstr = get_str(p->wk, p->previous.data.str);
	struct str str = { fstr->s }, identifier;

	struct node *n, *res;

	res = n = make_node_t(p, node_type_add);
	n->l = make_node_t(p, node_type_string);
	n->l->data.str = make_str(p->wk, "");

	for (i = 0; i < fstr->len;) {
		if (fstr->s[i] == '@' && is_valid_start_of_identifier(fstr->s[i + 1])) {
			for (j = i + 1; j < fstr->len && is_valid_inside_of_identifier(fstr->s[j]); ++j) {
			}

			if (fstr->s[j] == '@' && (identifier.len = j - i - 1) > 0) {
				identifier.s = &fstr->s[i + 1];

				n = n->r = make_node_t(p, node_type_add);

				n->l = make_node_t(p, node_type_add);
				n->l->data.str = make_strn(p->wk, str.s, str.len);

				n = n->r = make_node_t(p, node_type_add);

				n->l = make_node_t(p, node_type_stringify);
				n->l->l = make_node_t(p, node_type_id);
				n->l->l->data.str = make_strn(p->wk, identifier.s, identifier.len);

				i = j + 1;

				str = (struct str) { &fstr->s[i] };
				continue;
			}
		}

		++str.len;
		++i;
	}

	n = n->r = make_node_t(p, node_type_string);
	n->r->data.str = make_strn(p->wk, str.s, str.len);

	return res;
}

static struct node *
parse_binary(struct parser *p, struct node *l)
{
	enum node_type t;
	enum token_type prev = p->previous.type;
	struct node *n, *r;

	r = parse_prec(p, parse_rules[prev].precedence + 1);

	switch (prev) {
	case '+':
		t = node_type_add;
		break;
	default:
		UNREACHABLE;
	}

	n = make_node_t(p, t);
	n->l = l;
	n->r = r;
	return n;
}

static struct node *
parse_grouping(struct parser *p)
{
	struct node *n = parse_prec(p, parse_precedence_assignment);
	parse_expect(p, ')');
	return n;
}

static struct node *
parse_list(struct parser *p, enum node_type t, enum token_type end)
{
	uint32_t len = 0;
	struct node *n, *res;

	res = n = make_node_t(p, t);
	while (p->current.type != end) {
		n->l = parse_expr(p);
		++len;

		if (!parse_accept(p, ',') || p->current.type == end) {
			break;
		}

		n = n->r = make_node_t(p, node_type_list);
	}

	parse_expect(p, end);

	res->data.num = len;
	return res;
}

static struct node *
parse_call(struct parser *p, struct node *l)
{
	struct node *n;
	n = make_node_t(p, node_type_call);
	n->r = l;
	n->l = parse_list(p, node_type_args, ')');

	if (n->r->type == node_type_id) {
		n->r->type = node_type_id_lit;
	}
	return n;
}

static struct node *
parse_expr(struct parser *p)
{
	return parse_prec(p, parse_precedence_assignment);
}

static struct node *
parse_prec(struct parser *p, enum parse_precedence prec)
{
	parse_advance(p);

	if (!parse_rules[p->previous.type].prefix) {
		parse_error(p, 0, "expected expression");
		return 0;
	}

	struct node *l = parse_rules[p->previous.type].prefix(p);

	while (prec <= parse_rules[p->current.type].precedence) {
		parse_advance(p);
		l = parse_rules[p->previous.type].infix(p, l);
	}

	return l;
}

static struct node *
parse_stmt(struct parser *p)
{
	struct node *n;

	enum token_type assign_sequences[][2] = {
		{ token_type_identifier, '=' },
		{ token_type_identifier, token_type_plus_assign },
	};

	if (parse_match(p, assign_sequences[0], ARRAY_LEN(assign_sequences[0]))
		|| parse_match(p, assign_sequences[1], ARRAY_LEN(assign_sequences[1]))) {
		parse_advance(p);

		n = make_node_t(p, p->current.type == '=' ? node_type_assign : node_type_plusassign);
		n->l = parse_id(p);
		n->l->type = node_type_id_lit;

		parse_advance(p);
		n->r = parse_expr(p);
	} else {
		n = parse_expr(p);
	}

	parse_expect(p, token_type_eol);
	return n;
}

static const struct parse_rule _parse_rules[] = {
	[token_type_eol]        = { 0,              0,            0 },
	[token_type_eof]        = { 0,              0,            0 },
	[token_type_if]         = { 0,              0,            0 },
	[token_type_else]       = { 0,              0,            0 },
	[token_type_elif]       = { 0,              0,            0 },
	[token_type_number]     = { parse_number,   0,            0 },
	[token_type_identifier] = { parse_id,       0,            0 },
	[token_type_string]     = { parse_string,   0,            0 },
	[token_type_fstring]    = { parse_fstring,  0,            0 },
	['(']                   = { parse_grouping, parse_call,   parse_precedence_call },
	[')']                   = { 0,              0,            0 },
	['[']                   = { 0,              0,            0 },
	[']']                   = { 0,              0,            0 },
	[',']                   = { 0,              0,            0 },
	['+']                   = { 0,              parse_binary, parse_precedence_term  },
	['=']                   = { 0,              0,            0 },
};

static struct node *
parse(struct workspace *wk, struct source *src, struct bucket_arr *nodes)
{
	struct node *n, *res;
	struct parser _p = {
		.wk = wk,
		.nodes = nodes,
		.src = src,
		/* .current.type = token_type_error, */
	}, *p = &_p;

	// populate the global parse_rules
	parse_rules = _parse_rules;

	lexer_init(&p->lexer, wk, src, lexer_mode_functions);

	parse_advance(p);

	res = n = make_node_t(p, node_type_stmt);

	while (!parse_accept(p, token_type_eof)) {
		n->l = parse_stmt(p);
		n = n->r = make_node_t(p, node_type_stmt);
	}

	return res;
}

/******************************************************************************
 * stack
 ******************************************************************************/

struct stack_tag;
typedef void (*stack_print_cb)(void *ctx, void *mem, struct stack_tag *tag);

struct stack {
	char *mem;
	uint32_t len, cap;

	const char *name;
	bool log;
	stack_print_cb cb;
	void *ctx;
};

static struct stack
stack_init(uint32_t cap, const char *name, bool log, stack_print_cb cb, void *ctx)
{
	return (struct stack) {
		.mem = z_malloc(cap),
		.cap = cap,

		.name = name,
		.log = log,
		.cb = cb,
		.ctx = ctx,
	};
}

struct stack_tag {
	const char *name;
	uint32_t size;
};

static void
stack_push_raw(struct stack *stack, const void *mem, uint32_t size)
{
	assert(stack->len + size < stack->cap);
	memcpy(stack->mem + stack->len, mem, size);
	stack->len += size;
}

static void
stack_pop_raw(struct stack *stack, void *mem, uint32_t size)
{
	assert(stack->len >= size);
	stack->len -= size;
	memcpy(mem, stack->mem + stack->len, size);
}

static void
stack_peek_raw(struct stack *stack, void *mem, uint32_t size, uint32_t *off)
{
	assert(*off >= size);
	*off -= size;
	memcpy(mem, stack->mem + *off, size);
}

static void
stack_print(struct stack *_stack)
{
	struct stack_tag tag;
	struct stack stack = *_stack;
	while (stack.len) {
		stack_pop_raw(&stack, &tag, sizeof(tag));
		printf("  - %04d - %s", tag.size, tag.name);

		assert(stack.len >= tag.size);
		stack.len -= tag.size;
		void *mem = stack.mem + stack.len;

		stack.cb(stack.ctx, mem, &tag);

		printf("\n");
	}
}

static void
stack_push_sized(struct stack *stack, const void *mem, uint32_t size, const char *name)
{
	stack_push_raw(stack, mem, size);
	stack_push_raw(stack, &(struct stack_tag) { name, size }, sizeof(struct stack_tag));

	if (stack->log) {
		L("\033[33mstack\033[0m %s pushed:", stack->name);
		stack_print(stack);
	}
}

static void
stack_pop_sized(struct stack *stack, void *mem, uint32_t size)
{
	struct stack_tag tag;
	stack_pop_raw(stack, &tag, sizeof(tag));

	assert(size == tag.size);

	stack_pop_raw(stack, mem, size);

	if (stack->log) {
		L("\033[33mstack\033[0m %s popped:", stack->name);
		stack_print(stack);
	}
}

static void
stack_peek_sized(struct stack *stack, void *mem, uint32_t size)
{
	uint32_t off = stack->len;
	struct stack_tag tag;
	stack_peek_raw(stack, &tag, sizeof(tag), &off);

	assert(size == tag.size);

	stack_peek_raw(stack, mem, size, &off);
}

#define STRINGIZE(x) STRINGIZE2(x)
#define STRINGIZE2(x) #x
#define LINE_STRING STRINGIZE(__LINE__)

#define stack_push(__stack, __it) stack_push_sized((__stack), &(__it), (sizeof(__it)),  __FILE__":"LINE_STRING" "#__it)
#define stack_pop(__stack, __it) stack_pop_sized((__stack), &(__it), (sizeof(__it)))
#define stack_peek(__stack, __it) stack_peek_sized((__stack), &(__it), (sizeof(__it)))

static void
stack_print_node_cb(void *ctx, void *mem, struct stack_tag *tag)
{
	struct workspace *wk = ctx;
	printf(" %s", node_to_s(wk, *(struct node **)mem));
}

static void
arr_stack_print(struct workspace *wk) {
	uint32_t i;
	for (i = 0; i < wk->vm.stack.len; ++i) {
		printf("%04x %8d\n", i, *(obj *)arr_get(&wk->vm.stack, i));
	}
}

/******************************************************************************
 * vm errors
 ******************************************************************************/

static void
vm_error(struct workspace *wk, const char *fmt, ...) {
	static char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, ARRAY_LEN(buf), fmt, args);
	va_end(args);

	error_message(wk->src, wk->vm.locations[wk->vm.ip - 1], log_error, buf);
}


/******************************************************************************
 * fake native function api
 ******************************************************************************/

static bool
pop_args(struct workspace *wk,
	struct args_norm positional_args[],
	struct args_norm optional_positional_args[],
	struct args_kw keyword_args[])
{
	arr_stack_print(wk);


	uint32_t i;
	for (i = 0; positional_args[i].type != ARG_TYPE_NULL; ++i) {
		if (i >= wk->vm.nargs) {
			vm_error(wk, "not enough args");
			return false;
		}

		positional_args[i].val = *(obj *)arr_get(&wk->vm.stack, wk->vm.stack.len - wk->vm.nargs + i);
	}

	return true;
}

static bool
func_p2(struct workspace *wk, obj rcvr, obj *res)
{
	struct args_norm an[] = { { tc_any | TYPE_TAG_ALLOW_VOID }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, 0, 0)) {
		return false;
	}

	LO("p: %o\n", an[0].val);
	return true;
}

typedef bool (*func_impl2)(struct workspace *wk, obj rcvr, obj *res);

struct func_impl2 {
	const char *name;
	func_impl2 func;
};

const struct func_impl2 kernel_func_tbl2[][language_mode_count] = { [language_internal] = {
	{ "p", func_p2 },
}};

static bool
func_lookup2(const struct func_impl2 *impl_tbl, const struct str *s, uint32_t *idx)
{
	if (!impl_tbl) {
		return false;
	}

	uint32_t i;
	for (i = 0; impl_tbl[i].name; ++i) {
		if (strcmp(impl_tbl[i].name, s->s) == 0) {
			*idx = i;
			return true;
		}
	}

	return false;
}

/******************************************************************************
 * compiler
 ******************************************************************************/

struct compiler_state {
	struct bucket_arr nodes;
	struct arr locations;
	struct arr code;
	struct workspace *wk;
};

enum ops {
	op_constant = 1,
	op_add,
	op_sub,
	op_mul,
	op_div,
	op_store,
	op_load,
	op_return,
	op_call,
	op_call_native,
};

static obj
vm_get_constant(struct workspace *wk, uint8_t *code, uint32_t *ip)
{
	obj r = (code[*ip + 0] << 16) | (code[*ip + 1] << 8) | code[*ip + 2];
	*ip += 3;
	return r;
}

static const char*
vm_dis(struct workspace *wk, uint8_t *code, uint32_t ip)
{
	uint32_t i = 0;
	static char buf[2048];
#define buf_push(...) i += snprintf(&buf[i], sizeof(buf) - i, __VA_ARGS__);
#define op_case(__op) case __op: buf_push(#__op ":");

	buf_push("%04x ", ip);

	switch (code[ip++]) {
	op_case(op_add)
		break;
	op_case(op_return)
		break;
	op_case(op_store)
		break;
	op_case(op_constant)
		buf_push("%d", vm_get_constant(wk, code, &ip));
		break;
	op_case(op_call)
		buf_push("%d", vm_get_constant(wk, code, &ip));
		break;
	op_case(op_call_native)
		buf_push("%d", vm_get_constant(wk, code, &ip));
		uint32_t id = vm_get_constant(wk, code, &ip);
		buf_push(",%s", kernel_func_tbl2[language_internal][id].name);
		break;
	default:
		buf_push("unknown: %d", code[ip - 1]);
	}

#undef buf_push

	return buf;
}

static void
vm_execute(struct workspace *wk, struct arr *_code, struct arr *locations)
{
	wk->vm.code = _code->e;
	wk->vm.locations = (struct source_location *)locations->e;
	obj a, b;

	arr_init(&wk->vm.stack, 4096, sizeof(obj));

	while (wk->vm.ip < _code->len) {
		L("%s", vm_dis(wk, wk->vm.code, wk->vm.ip));

		switch (wk->vm.code[wk->vm.ip++]) {
		case op_constant:
			a = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);
			arr_push(&wk->vm.stack, &a);
			break;
		case op_add:
			arr_pop(&wk->vm.stack, &b);
			arr_pop(&wk->vm.stack, &a);
			uint32_t res = get_obj_number(wk, b) + get_obj_number(wk, a);
			make_obj(wk, &a, obj_number);
			set_obj_number(wk, a, res);
			arr_push(&wk->vm.stack, &a);
			break;
		case op_store:
			arr_pop(&wk->vm.stack, &b);
			arr_pop(&wk->vm.stack, &a);
			wk->assign_variable(wk, get_str(wk, a)->s, b, 0, assign_local);
			/* LO("%o, %o\n", a, b); */
			break;
		case op_load:
			a = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);

			if (!wk->get_variable(wk, get_str(wk, a)->s, &b, wk->cur_project)) {
				L("undefined object %s", get_str(wk, a)->s);
				/* interp_error(wk, 0, "undefined object"); */
				/* ret = false; */
				wk->vm.ip = _code->len;
			}

			arr_push(&wk->vm.stack, &b);
			break;
		case op_call:
			wk->vm.nargs = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);
			L("%d", a);
			break;
		case op_call_native:
			wk->vm.nargs = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);
			b = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);
			L("%d, %d", a, b);
			obj id;
			kernel_func_tbl2[language_internal][b].func(wk, 0, &id);
			if (id) {
				arr_push(&wk->vm.stack, &id);
			}
			break;
		case op_return:
			wk->vm.ip = _code->len;
			break;
		default:
			UNREACHABLE;
		}
	}
	/* stack_pop(&stack, a); */
	/* LO("%o\n", a); */
	/* printf("%04x %02x\n", i, code[i]); */
}

static void
push_code(struct compiler_state *c, struct node *n, uint8_t b)
{
	arr_push(&c->code, &b);
	arr_push(&c->locations, n ? &n->location : &(struct source_location) { 0 });
}

static void
push_constant(struct compiler_state *c, struct node *n, obj v)
{
	push_code(c, n, (v >> 16) & 0xff);
	push_code(c, n, (v >> 8) & 0xff);
	push_code(c, n, v & 0xff);
}

static void
comp_node(struct compiler_state *c, struct node *n)
{
	/* L("%s", node_to_s(c->wk, n)); */
	switch (n->type) {
	case node_type_id:
		push_code(c, n, op_load);
		push_constant(c, n, n->data.str);
		break;
	case node_type_number:
		push_code(c, n, op_constant);
		obj o;
		make_obj(c->wk, &o, obj_number);
		set_obj_number(c->wk, o, n->data.num);
		push_constant(c, n, o);
		break;
	case node_type_add:
		push_code(c, n, op_add);
		break;
	case node_type_sub:
		push_code(c, n, op_sub);
		break;
	case node_type_mul:
		push_code(c, n, op_mul);
		break;
	case node_type_div:
		push_code(c, n, op_div);
		break;
	case node_type_assign:
		push_code(c, n, op_store);
		break;
	/* case node_type_args: */
	/* 	break; */
	case node_type_call: {
		bool native = false;
		uint32_t idx;
					       //
		if (n->r->type == node_type_id_lit) {
			native = func_lookup2(kernel_func_tbl2[language_internal], get_str(c->wk, n->r->data.str), &idx);
			if (!native) {
				n->r->type = node_type_id;
				comp_node(c, n->r);
			}
		}

		if (native) {
			push_code(c, n, op_call_native);
			push_constant(c, n, n->l->data.num); // nargs
			push_constant(c, n, idx);
		} else {
			push_code(c, n, op_call);
			push_constant(c, n, n->l->data.num); // nargs
		}
		break;
	}
	default:
		L("skipping %s", node_to_s(c->wk, n));
	}
}

bool
compile(struct workspace *wk, struct source *src, uint32_t flags)
{
	struct node *n, *prev = 0, *peek;
	struct compiler_state _c = { .wk = wk, }, *c = &_c;
	struct stack stack;

	stack = stack_init(4096, "", 0, stack_print_node_cb, wk);
	bucket_arr_init(&c->nodes, 2048, sizeof(struct node));
	arr_init(&c->code, 8 + 1024, 1);
	arr_init(&c->locations, 8 + 1024, sizeof(struct source_location));

	if (!(n = parse(wk, src, &c->nodes))) {
		return false;
	}

	print_ast(wk, n);

	while (stack.len || n) {
		if (n) {
			stack_push(&stack, n);
			n = n->l;
		} else {
			stack_peek(&stack, peek);
			if (peek->r && prev != peek->r) {
				n = peek->r;
			} else {
				comp_node(c, peek);
				stack_pop(&stack, prev);
			}
		}
	}
	push_code(c, 0, op_return);

	wk->src = src;
	vm_execute(wk, &c->code, &c->locations);
	return true;
}

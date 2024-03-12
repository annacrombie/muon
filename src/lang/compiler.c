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
	node_type_array,
	node_type_list,
	node_type_kw,
	node_type_or,
	node_type_and,
	node_type_eq,
	node_type_neq,
	node_type_lt,
	node_type_leq,
	node_type_gt,
	node_type_geq,
	node_type_in,
	node_type_not_in,
	node_type_add,
	node_type_sub,
	node_type_div,
	node_type_mul,
	node_type_mod,
	node_type_not,
	node_type_index,
	node_type_method,
	node_type_call,
	node_type_assign,
	node_type_plusassign,
	node_type_foreach,
	node_type_foreach_args,
	node_type_if,
	node_type_negate,
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
static struct node *parse_block(struct parser *p, enum token_type types[], uint32_t types_len);

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
	nt(array);
	nt(kw);
	nt(or);
	nt(and);
	nt(in);
	nt(not_in);
	nt(eq);
	nt(neq);
	nt(lt);
	nt(leq);
	nt(gt);
	nt(geq);
	nt(add);
	nt(sub);
	nt(div);
	nt(mul);
	nt(mod);
	nt(not);
	nt(index);
	nt(method);
	nt(call);
	nt(assign);
	nt(plusassign);
	nt(foreach_args);
	nt(foreach);
	nt(if);
	nt(negate);
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
	case node_type_id_lit:
	case node_type_string:
		i += obj_snprintf(wk, &buf[i], BUF_SIZE_S - i, ":%o", n->data.str);
		break;
	case node_type_number:
	case node_type_bool:
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

	if (p->current.type == token_type_not) {
		struct lexer lexer_peek = p->lexer;
		struct token next;
		lexer_next(&lexer_peek, &next);
		if (next.type == token_type_in) {
			p->current.type = token_type_not_in;
			p->lexer = lexer_peek;
		}
	}

	/* LL("previous: %s, current: ", token_to_s(p->wk, &p->previous)); */
	/* printf("%s\n", token_to_s(p->wk, &p->current)); */
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
parse_bool(struct parser *p)
{
	struct node *n = make_node_t(p, node_type_bool);
	n->data.num = p->previous.type == token_type_true;
	return n;
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
	case '+': t = node_type_add; break;
	case '-': t = node_type_sub; break;
	case '/': t = node_type_div; break;
	case '*': t = node_type_mul; break;
	case '%': t = node_type_mod; break;
	case '<': t = node_type_lt; break;
	case '>': t = node_type_gt; break;
	case token_type_eq: t = node_type_eq; break;
	case token_type_neq: t = node_type_neq; break;
	case token_type_leq: t = node_type_leq; break;
	case token_type_geq: t = node_type_geq; break;
	case token_type_or: t = node_type_or; break;
	case token_type_and: t = node_type_and; break;
	case token_type_in: t = node_type_in; break;
	case token_type_not_in: t = node_type_not_in; break;
	default:
		UNREACHABLE;
	}

	n = make_node_t(p, t);
	n->l = l;
	n->r = r;
	return n;
}

static struct node *
parse_unary(struct parser *p)
{
	struct node *n;
	n = make_node_t(p, node_type_negate);
	n->l = parse_prec(p, parse_precedence_unary);
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
parse_array(struct parser *p)
{
	return parse_list(p, node_type_array, ']');
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
parse_method(struct parser *p, struct node *l)
{
	struct node *n;
	n = make_node_t(p, node_type_method);
	n->l = l;

	parse_expect(p, token_type_identifier);
	struct node *id = parse_id(p);

	parse_expect(p, '(');
	n->r = parse_call(p, id);
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

	if (parse_accept(p, token_type_if)) {
		struct node *parent;
		parent = n = make_node_t(p, node_type_if);
		while (true) {
			n->l = make_node_t(p, node_type_list);
			n->l->l = p->previous.type == token_type_else ? 0 : parse_expr(p);
			parse_expect(p, token_type_eol);
			n->l->r = parse_block(p, (enum token_type[]){token_type_elif, token_type_else, token_type_endif}, 3);

			if (!(parse_accept(p, token_type_elif) || parse_accept(p, token_type_else))) {
				break;
			}

			n = n->r = make_node_t(p, node_type_if);
		}

		parse_expect(p, token_type_endif);
		n = parent;
	} else if (parse_accept(p, token_type_foreach)) {
		n = make_node_t(p, node_type_foreach);
		n->l = make_node_t(p, node_type_foreach_args);

		parse_expect(p, token_type_identifier);
		n->l->l = make_node_t(p, node_type_list);
		n->l->l->l = parse_id(p);

		if (parse_accept(p, ',')) {
			parse_expect(p, token_type_identifier);
			n->l->l->r = parse_id(p);
		}

		parse_expect(p, ':');
		n->l->r = parse_expr(p);

		parse_expect(p, token_type_eol);

		n->r = parse_block(p, (enum token_type[]){token_type_endforeach}, 1);
		parse_expect(p, token_type_endforeach);
	} else if (parse_match(p, assign_sequences[0], ARRAY_LEN(assign_sequences[0]))
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

static bool
parse_block_skip_eol_and_check_terminator(struct parser *p, enum token_type types[], uint32_t types_len)
{
	while (parse_accept(p, token_type_eol)) {
	}

	uint32_t i;
	for (i = 0; i < types_len; ++i) {
		if (p->current.type == types[i]) {
			return true;
		}
	}

	return  parse_accept(p, token_type_eof);
}

static struct node *
parse_block(struct parser *p, enum token_type types[], uint32_t types_len)
{
	struct node *res, *n;

	res = n = make_node_t(p, node_type_stmt);

	while (true) {
		if (parse_block_skip_eol_and_check_terminator(p, types, types_len)) {
			break;
		}

		n->l = parse_stmt(p);

		if (parse_block_skip_eol_and_check_terminator(p, types, types_len)) {
			break;
		}

		n = n->r = make_node_t(p, node_type_stmt);
	}

	return res;
}

static const struct parse_rule _parse_rules[] = {
	[token_type_number]     = { parse_number,   0,            0                           },
	[token_type_identifier] = { parse_id,       0,            0                           },
	[token_type_string]     = { parse_string,   0,            0                           },
	[token_type_fstring]    = { parse_fstring,  0,            0                           },
	[token_type_true]       = { parse_bool,     0,            0                           },
	[token_type_false]      = { parse_bool,     0,            0                           },
	['(']                   = { parse_grouping, parse_call,   parse_precedence_call       },
	['[']                   = { parse_array,    0,            0                           },
	['+']                   = { 0,              parse_binary, parse_precedence_term       },
	['-']                   = { parse_unary,    parse_binary, parse_precedence_term       },
	['*']                   = { 0,              parse_binary, parse_precedence_factor     },
	['/']                   = { 0,              parse_binary, parse_precedence_factor     },
	['<']                   = { 0,              parse_binary, parse_precedence_comparison },
	['>']                   = { 0,              parse_binary, parse_precedence_comparison },
	[token_type_leq]        = { 0,              parse_binary, parse_precedence_comparison },
	[token_type_geq]        = { 0,              parse_binary, parse_precedence_comparison },
	[token_type_or]         = { 0,              parse_binary, parse_precedence_or         },
	[token_type_and]        = { 0,              parse_binary, parse_precedence_and        },
	[token_type_eq]         = { 0,              parse_binary, parse_precedence_equality   },
	[token_type_neq]        = { 0,              parse_binary, parse_precedence_equality   },
	[token_type_in]         = { 0,              parse_binary, parse_precedence_equality   },
	['.']                   = { 0,              parse_method, parse_precedence_call       },
};

static struct node *
parse(struct workspace *wk, struct source *src, struct bucket_arr *nodes)
{
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

	return parse_block(p, (enum token_type[]){token_type_eof}, 1);
}

/******************************************************************************
 * obj stack
 ******************************************************************************/

enum {
	object_stack_page_size = 1024 / sizeof(obj)
};

static void
object_stack_alloc_page(struct object_stack *s)
{
	bucket_arr_pushn(&s->ba, 0, 0, s->ba.bucket_size);
	++s->bucket;
	s->page = (obj *)((struct bucket *)s->ba.buckets.e)[s->bucket].mem;
	((struct bucket *)s->ba.buckets.e)[s->bucket].len = object_stack_page_size;
	s->i = 0;
}

static void
object_stack_init(struct object_stack *s)
{
	bucket_arr_init(&s->ba, object_stack_page_size, sizeof(obj));
	s->page = (obj *)((struct bucket *)s->ba.buckets.e)[0].mem;
	((struct bucket *)s->ba.buckets.e)[0].len = object_stack_page_size;
}

static void
object_stack_push(struct object_stack *s, obj o)
{
	if (s->i >= object_stack_page_size) {
		object_stack_alloc_page(s);
	}

	s->page[s->i] = o;
	++s->i;
	++s->ba.len;
}

static obj
object_stack_pop(struct object_stack *s)
{
	if (!s->i) {
		assert(s->bucket);
		--s->bucket;
		s->page = (obj *)((struct bucket *)s->ba.buckets.e)[s->bucket].mem;
		s->i = object_stack_page_size;
	}

	--s->i;
	--s->ba.len;
	return s->page[s->i];
}

static obj
object_stack_peek(struct object_stack *s, uint32_t off)
{
	return *(obj *)bucket_arr_get(&s->ba, s->ba.len - off);
}

static void
object_stack_discard(struct object_stack *s, uint32_t n)
{
	s->ba.len -= n;
	s->bucket = s->ba.len / s->ba.bucket_size;
	s->page = (obj *)((struct bucket *)s->ba.buckets.e)[s->bucket].mem;
	s->i = s->ba.len % s->ba.bucket_size;
}

static void
object_stack_print(struct object_stack *s)
{
	for (int32_t i = s->ba.len - 1; i >= 0; --i) {
		log_plain("%d|", *(obj *)bucket_arr_get(&s->ba, i));
	}
	log_plain("\n");
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
	uint32_t i;
	for (i = 0; positional_args[i].type != ARG_TYPE_NULL; ++i) {
		if (i >= wk->vm.nargs) {
			vm_error(wk, "not enough args");
			return false;
		}

		positional_args[i].val = object_stack_peek(&wk->vm.stack, wk->vm.nargs - i);
	}

	object_stack_discard(&wk->vm.stack, wk->vm.nargs);
	return true;
}

static bool
func_p2(struct workspace *wk, obj rcvr, obj *res)
{
	struct args_norm an[] = { { tc_any | TYPE_TAG_ALLOW_VOID }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, 0, 0)) {
		return false;
	}

	obj_fprintf(wk, log_file(), "%o\n", an[0].val);
	*res = an[0].val;
	return true;
}

bool
vm_rangecheck(struct workspace *wk, uint32_t n_id, int64_t min, int64_t max, int64_t n)
{
	if (n < min || n > max) {
		vm_error(wk, "number %" PRId64 " out of bounds (%" PRId64 ", %" PRId64 ")", n, min, max);
		return false;
	}

	return true;
}


static bool
func_range(struct workspace *wk, obj _, obj *res)
{
	struct range_params params;
	struct args_norm an[] = { { obj_number }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { obj_number }, { obj_number }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, ao, 0)) {
		return false;
	}

	int64_t n = get_obj_number(wk, an[0].val);
	if (!vm_rangecheck(wk, an[0].node, 0, UINT32_MAX, n)) {
		return false;
	}
	params.start = n;

	if (ao[0].set) {
		int64_t n = get_obj_number(wk, ao[0].val);
		if (!vm_rangecheck(wk, ao[0].node, params.start, UINT32_MAX, n)) {
			return false;
		}

		params.stop = n;
	} else {
		params.stop = params.start;
		params.start = 0;
	}

	if (ao[1].set) {
		int64_t n = get_obj_number(wk, ao[1].val);
		if (!vm_rangecheck(wk, ao[1].node, 1, UINT32_MAX, n)) {
			return false;
		}
		params.step = n;
	} else {
		params.step = 1;
	}

	make_obj(wk, res, obj_iterator);
	struct obj_iterator *iter = get_obj_iterator(wk, *res);
	iter->type = obj_iterator_type_range;
	iter->data.range = params;

	return true;
}


typedef bool (*func_impl2)(struct workspace *wk, obj rcvr, obj *res);

struct func_impl2 {
	const char *name;
	func_impl2 func;
};

const struct func_impl2 kernel_func_tbl2[][language_mode_count] = { [language_internal] = {
	{ "p", func_p2 },
	{ "range", func_range },
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
	struct arr node_stack;
	struct arr jmp_stack;
	struct arr locations;
	struct arr code;
	struct workspace *wk;
};

enum op {
	op_constant = 1,
	op_constant_list,
	op_add,
	op_sub,
	op_mul,
	op_div,
	op_eq,
	op_neq,
	op_in,
	op_not_in,
	op_gt,
	op_lt,
	op_leq,
	op_geq,
	op_negate,
	op_store,
	op_load,
	op_return,
	op_call,
	op_call_native,
	op_iterator,
	op_iterator_next,
	op_jmp_if_null,
	op_jmp_if_false,
	op_jmp,
	op_pop,
};

static obj
vm_get_constant(struct workspace *wk, uint8_t *code, uint32_t *ip)
{
	obj r = (code[*ip + 0] << 16) | (code[*ip + 1] << 8) | code[*ip + 2];
	*ip += 3;
	return r;
}

struct vm_dis_result
{
	const char *text;
	uint32_t inst_len;
};

static struct vm_dis_result
vm_dis(struct workspace *wk, uint8_t *code, uint32_t base_ip)
{
	uint32_t i = 0;
	static char buf[2048];
#define buf_push(...) i += snprintf(&buf[i], sizeof(buf) - i, __VA_ARGS__);
#define op_case(__op) case __op: buf_push(#__op);

	uint32_t ip = base_ip;
	buf_push("%04x ", ip);

	switch (code[ip++]) {
	op_case(op_add)
		break;
	op_case(op_return)
		break;
	op_case(op_store)
		buf_push(":%s", get_str(wk, vm_get_constant(wk, code, &ip))->s);
		break;
	op_case(op_load)
		buf_push(":%s", get_str(wk, vm_get_constant(wk, code, &ip))->s);
		break;
	op_case(op_constant)
		buf_push(":%d", vm_get_constant(wk, code, &ip));
		break;
	op_case(op_constant_list)
		buf_push(":%d", vm_get_constant(wk, code, &ip));
		break;
	op_case(op_call)
		buf_push(":%d", vm_get_constant(wk, code, &ip));
		break;
	op_case(op_call_native)
		buf_push(":%d", vm_get_constant(wk, code, &ip));
		uint32_t id = vm_get_constant(wk, code, &ip);
		buf_push(",%s", kernel_func_tbl2[language_internal][id].name);
		break;
	op_case(op_iterator)
		break;
	op_case(op_iterator_next)
		break;
	op_case(op_jmp_if_null)
		buf_push(":%04x", vm_get_constant(wk, code, &ip));
		break;
	op_case(op_jmp_if_false)
		buf_push(":%04x", vm_get_constant(wk, code, &ip));
		break;
	op_case(op_jmp)
		buf_push(":%04x", vm_get_constant(wk, code, &ip));
		break;
	op_case(op_pop)
		break;
	op_case(op_eq)
		break;
	default:
		buf_push("unknown: %d", code[ip - 1]);
	}

#undef buf_push

	return (struct vm_dis_result){ buf, ip - base_ip };
}

static void
vm_execute(struct workspace *wk, struct arr *_code, struct arr *locations)
{
	wk->vm.code = _code->e;
	wk->vm.locations = (struct source_location *)locations->e;
	obj a, b;

	object_stack_init(&wk->vm.stack);

	L("---");
	for (uint32_t i = 0; i < _code->len;) {
		struct vm_dis_result dis = vm_dis(wk, wk->vm.code, i);
		L("%s", dis.text);
		i += dis.inst_len;
	}
	L("---");

	while (wk->vm.ip < _code->len) {
		LL("%-50s", vm_dis(wk, wk->vm.code, wk->vm.ip).text);
		object_stack_print(&wk->vm.stack);

		switch (wk->vm.code[wk->vm.ip++]) {
		case op_constant:
			a = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);
			object_stack_push(&wk->vm.stack, a);
			break;
		case op_constant_list: {
			uint32_t len = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);
			make_obj(wk, &b, obj_array);
			for (uint32_t i = 0; i < len; ++i) {
				obj_array_push(wk, b, object_stack_peek(&wk->vm.stack, len - i));
			}

			object_stack_discard(&wk->vm.stack, len);
			object_stack_push(&wk->vm.stack, b);
			break;
		}
		case op_add:
			b = object_stack_pop(&wk->vm.stack);
			a = object_stack_pop(&wk->vm.stack);
			struct obj_internal *oa = bucket_arr_get(&wk->objs, a),
					    *ob = bucket_arr_get(&wk->objs, b);

			if (!(oa->t == obj_array || oa->t == ob->t)) {
				goto op_add_type_err;
			}

			obj res;

			switch (oa->t) {
			case obj_number:
				make_obj(wk, &res, obj_number);
				int64_t *ia = bucket_arr_get(&wk->obj_aos[obj_number - _obj_aos_start], oa->val),
					*ib = bucket_arr_get(&wk->obj_aos[obj_number - _obj_aos_start], ob->val);
				set_obj_number(wk, res, *ia + *ib);
				break;
			default:
op_add_type_err:
				vm_error(wk, "unable to add!");
				res = 0;
				break;
			}

			object_stack_push(&wk->vm.stack, res);
			break;
		case op_sub:
			break;
		case op_mul:
			break;
		case op_div:
			break;
		case op_eq:
			b = object_stack_pop(&wk->vm.stack);
			a = object_stack_pop(&wk->vm.stack);
			a = obj_equal(wk, a, b) ? obj_bool_true : obj_bool_false;
			object_stack_push(&wk->vm.stack, a);
			break;
		case op_store:
			b = object_stack_peek(&wk->vm.stack, 1);
			a = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);
			wk->assign_variable(wk, get_str(wk, a)->s, b, 0, assign_local);
			LO("%o <= %o\n", a, b);
			break;
		case op_load:
			a = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);

			if (!wk->get_variable(wk, get_str(wk, a)->s, &b, wk->cur_project)) {
				L("undefined object %s", get_str(wk, a)->s);
				/* interp_error(wk, 0, "undefined object"); */
				/* ret = false; */
				wk->vm.ip = _code->len;
			}

			LO("%o <= %o\n", b, a);
			object_stack_push(&wk->vm.stack, b);
			break;
		case op_call:
			wk->vm.nargs = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);
			break;
		case op_call_native:
			wk->vm.nargs = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);
			b = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);
			kernel_func_tbl2[language_internal][b].func(wk, 0, &a);
			object_stack_push(&wk->vm.stack, a);
			break;
		case op_iterator: {
			obj iter;
			struct obj_iterator *iterator;

			a = object_stack_pop(&wk->vm.stack);
			enum obj_type a_type = get_obj_type(wk, a);

			if (a_type == obj_iterator) {
				// already an iterator!
				object_stack_push(&wk->vm.stack, a);
				break;
			}

			make_obj(wk, &iter, obj_iterator);
			object_stack_push(&wk->vm.stack, iter);
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
			default:
				vm_error(wk, "bad iterator type");
				return;
			}
			break;
		}
		case op_iterator_next: {
			struct obj_iterator *iterator;
			iterator = get_obj_iterator(wk, object_stack_peek(&wk->vm.stack, 1));

			switch (iterator->type) {
			case obj_iterator_type_array:
				if (!iterator->data.array) {
					b = 0;
				} else {
					b = iterator->data.array->val;
					iterator->data.array = iterator->data.array->have_next ? get_obj_array(wk, iterator->data.array->next) : 0;
				}
				break;
			case obj_iterator_type_range:
				if (iterator->data.range.start >= iterator->data.range.stop) {
					b = 0;
				} else {
					make_obj(wk, &b, obj_number);
					set_obj_number(wk, b, iterator->data.range.start);
					iterator->data.range.start += iterator->data.range.step;
				}
				break;
			default:
				UNREACHABLE;
			}

			object_stack_push(&wk->vm.stack, b);
			break;
		}
		case op_pop:
			a = object_stack_pop(&wk->vm.stack);
			break;
		case op_jmp_if_null:
			a = object_stack_peek(&wk->vm.stack, 1);
			b = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);
			if (!a) {
				object_stack_discard(&wk->vm.stack, 1);
				wk->vm.ip = b;
			}
			break;
		case op_jmp_if_false:
			a = object_stack_pop(&wk->vm.stack);
			b = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);
			if (!get_obj_bool(wk, a)) {
				wk->vm.ip = b;
			}
			break;
		case op_jmp:
			a = vm_get_constant(wk, wk->vm.code, &wk->vm.ip);
			wk->vm.ip = a;
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
push_constant_at(obj v, uint8_t *code)
{
	code[0] = (v >> 16) & 0xff;
	code[1] = (v >> 8) & 0xff;
	code[2] = v & 0xff;
}

static void
push_constant(struct compiler_state *c, struct node *n, obj v)
{
	push_code(c, n, (v >> 16) & 0xff);
	push_code(c, n, (v >> 8) & 0xff);
	push_code(c, n, v & 0xff);
}

static void compile_block(struct compiler_state *c, struct node *n);
static void compile_expr(struct compiler_state *c, struct node *n);

static void
comp_node(struct compiler_state *c, struct node *n)
{
	assert(n->type != node_type_stmt);

	/* L("%s", node_to_s(c->wk, n)); */
	switch (n->type) {
	case node_type_add: push_code(c, n, op_add); break;
	case node_type_sub: push_code(c, n, op_sub); break;
	case node_type_mul: push_code(c, n, op_mul); break;
	case node_type_div: push_code(c, n, op_div); break;
	case node_type_eq: push_code(c, n, op_eq); break;
	case node_type_neq: push_code(c, n, op_neq); break;
	case node_type_in: push_code(c, n, op_in); break;
	case node_type_not_in: push_code(c, n, op_not_in); break;
	case node_type_lt: push_code(c, n, op_lt); break;
	case node_type_gt: push_code(c, n, op_gt); break;
	case node_type_leq: push_code(c, n, op_leq); break;
	case node_type_geq: push_code(c, n, op_geq); break;
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
	case node_type_bool:
		push_code(c, n, op_constant);
		push_constant(c, n, n->data.num ? obj_bool_true : obj_bool_false);
		break;
	case node_type_assign:
		push_code(c, n, op_store);
		push_constant(c, n, n->l->data.str);
		break;
	case node_type_array:
		push_code(c, n, op_constant_list);
		push_constant(c, n, n->data.num);
		break;
	case node_type_call: {
		bool native = false;
		uint32_t idx;

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
	case node_type_foreach: {
		uint32_t break_jmp_patch_tgt, loop_body_start;
		struct node *ida = n->l->l->l; //, *idb = n->l->l->r;

		compile_expr(c, n->l->r);
		push_code(c, n, op_iterator);

		loop_body_start = c->code.len;

		push_code(c, n, op_iterator_next);
		push_code(c, n, op_jmp_if_null);
		break_jmp_patch_tgt = c->code.len;
		push_constant(c, n, 0);

		push_code(c, n, op_store);
		push_constant(c, n, ida->data.str);
		push_code(c, n, op_pop);

		compile_block(c, n->r);

		push_code(c, n, op_jmp);
		push_constant(c, n, loop_body_start);
		push_constant_at(c->code.len, arr_get(&c->code, break_jmp_patch_tgt));
		break;
	}
	case node_type_if: {
		uint32_t else_jmp = 0, end_jmp;
		uint32_t patch_tgts = 0;

		while (n) {
			if (n->l->l) {
				compile_expr(c, n->l->l);
				push_code(c, n, op_jmp_if_false);
				else_jmp = c->code.len;
				push_constant(c, n, 0);
			}

			compile_block(c, n->l->r);

			push_code(c, n, op_jmp);

			end_jmp = c->code.len;
			arr_push(&c->jmp_stack, &end_jmp);
			++patch_tgts;
			push_constant(c, n, 0);

			if (n->l->l) {
				push_constant_at(c->code.len, arr_get(&c->code, else_jmp));
			}

			n = n->r;
		}

		for (uint32_t i = 0; i < patch_tgts; ++i) {
			arr_pop(&c->jmp_stack, &end_jmp);
			push_constant_at(c->code.len, arr_get(&c->code, end_jmp));
		}
		break;
	}
	default:
		L("skipping %s", node_to_s(c->wk, n));
		break;
	}
}

static void
compile_expr(struct compiler_state *c, struct node *n)
{
	struct node *peek, *prev = 0;

	uint32_t stack_base = c->node_stack.len;

	while (c->node_stack.len > stack_base || n) {
		if (n) {
			if (n->type == node_type_foreach || n->type == node_type_if) {
				comp_node(c, n);
				n = 0;
			} else {
				arr_push(&c->node_stack, &n);
				n = n->l;
			}
		} else {
			peek = *(struct node **)arr_get(&c->node_stack, c->node_stack.len - 1);
			if (peek->r && prev != peek->r) {
				n = peek->r;
			} else {
				comp_node(c, peek);
				arr_pop(&c->node_stack, &prev);
			}
		}
	}
}

static void
compile_block(struct compiler_state *c, struct node *n)
{
	while (n && n->l) {
		assert(n->type == node_type_stmt);
		compile_expr(c, n->l);
		if (n->l->type != node_type_if) {
			push_code(c, n, op_pop);
		}
		n = n->r;
	}
}

bool
compile(struct workspace *wk, struct source *src, uint32_t flags)
{
	struct node *n;
	struct compiler_state _c = { .wk = wk, }, *c = &_c;

	arr_init(&c->node_stack, 4096, sizeof(struct node *));
	arr_init(&c->jmp_stack, 1024, sizeof(uint32_t));
	bucket_arr_init(&c->nodes, 2048, sizeof(struct node));
	arr_init(&c->code, 4 * 1024, 1);
	arr_init(&c->locations, 1024, sizeof(struct source_location));

	if (!(n = parse(wk, src, &c->nodes))) {
		return false;
	}

	print_ast(c->wk, n);

	compile_block(c, n);

	wk->src = src;
	vm_execute(wk, &c->code, &c->locations);
	return true;
}

/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: Simon Zeni <simon@bl4ckb0ne.ca>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

#include "buf_size.h"
#include "error.h"
#include "lang/eval.h"
#include "lang/lexer.h"
#include "lang/parser.h"
#include "lang/string.h"
#include "lang/typecheck.h"
#include "lang/workspace.h"
#include "log.h"
#include "tracy.h"

const uint32_t arithmetic_type_count = 5;

struct parser {
	struct lexer lexer;
	struct source *src;
	struct token previous, current;
	struct ast *ast;
	struct workspace *wk;
	uint32_t token_i, loop_depth, parse_depth, func_depth;
	bool caused_effect, valid, preserve_fmt_eol, parsing_func_def_args;

	enum parse_mode mode;
};

const char *
node_type_to_s(enum node_type t)
{
	switch (t) {
	case node_null: return "null";
	case node_bool: return "bool";
	case node_id: return "id";
	case node_number: return "number";
	case node_string: return "string";
	case node_continue: return "continue";
	case node_break: return "break";
	case node_argument: return "argument";
	case node_array: return "array";
	case node_dict: return "dict";
	case node_empty: return "empty";
	case node_or: return "or";
	case node_and: return "and";
	case node_comparison: return "comparison";
	case node_arithmetic: return "arithmetic";
	case node_not: return "not";
	case node_index: return "index";
	case node_method: return "method";
	case node_function: return "function";
	case node_assignment: return "assignment";
	case node_foreach_args: return "foreach_args";
	case node_foreach: return "foreach";
	case node_if: return "if";
	case node_u_minus: return "u_minus";
	case node_ternary: return "ternary";
	case node_block: return "block";
	case node_stringify: return "stringify";
	case node_func_def: return "func_def";
	case node_return: return "return";
	case node_empty_line: return "empty_line";
	case node_paren: return "paren";
	case node_plusassign: return "plusassign";
	}

	assert(false && "unreachable");
	return "";
}

static void
parse_diagnostic(struct parser *p, struct token *err_tok, enum log_level lvl, const char *fmt, va_list args)
{
	if (p->mode & pm_quiet) {
		return;
	}

	if (!err_tok) {
		err_tok = &p->current;
	}

	error_messagev(p->src, err_tok->location, lvl, fmt, args);
}

MUON_ATTR_FORMAT(printf, 3, 4)
static void
parse_error(struct parser *p, struct token *err_tok, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	parse_diagnostic(p, err_tok, log_error, fmt, args);
	va_end(args);
}

static void
parse_advance(struct parser *p)
{
	p->previous = p->current;
	lexer_next(&p->lexer, &p->current);
	/* L("got tok %s", token_to_s(p->wk, &p->current)); */
}

static void
accept_comment_for(struct parser *p, uint32_t id)
{
	if (p->current.type == token_type_comment) {
		struct node *n = bucket_arr_get(&p->ast->nodes, id);
		/* L("adding comment to %s:%d", node_to_s(n), id); */

		if (!n->comments.len) {
			n->comments.start = p->ast->comments.len;
		}

		assert(false && "fix this");
		arr_push(&p->ast->comments, &p->current.data.str);
		++n->comments.len;
		parse_advance(p);
	}
}

static void
accept_comment(struct parser *p)
{
	accept_comment_for(p, p->ast->nodes.len - 1);
}

static bool
accept(struct parser *p, enum token_type type)
{
	if (p->mode & pm_keep_formatting && !p->preserve_fmt_eol) {
		while (p->current.type == token_type_fmt_eol) {
			parse_advance(p);
		}
	}

	accept_comment(p);

	bool got_error = false;
	while (p->current.type == token_type_error) {
		got_error = true;
		parse_error(p, &p->current, "%s", get_cstr(p->wk, p->current.data.str));
		/* L("accepting %s? %d (%s)", token_type_to_s(type), p->current.type == type, token_to_s(p->wk, &p->current)); */
		parse_advance(p);
	}

	if (got_error) {
		p->valid = false;
	}

	/* L("accepting %s? %d (%s)", token_type_to_s(type), p->current.type == type, token_to_s(p->wk, &p->current)); */
	if (p->current.type == type) {
		parse_advance(p);
		return true;
	}

	return false;
}

static bool
expect(struct parser *p, enum token_type type)
{
	accept_comment(p);

	if (!accept(p, type)) {
		parse_error(p, NULL, "expected '%s', got '%s'", token_type_to_s(type), token_type_to_s(p->current.type));
		return false;
	}

	return true;
}

static void
consume_until(struct parser *p, enum token_type t)
{
	while (p->current.type != t
	       && p->current.type != token_type_eof) {
		parse_advance(p);
	}
}

struct node *
get_node(struct ast *ast, uint32_t i)
{
	return bucket_arr_get(&ast->nodes, i);
}

static struct node *
make_node(struct parser *p, uint32_t *idx, enum node_type t)
{
	*idx = p->ast->nodes.len;
	bucket_arr_push(&p->ast->nodes, &(struct node){ .type = t });
	struct node *n = bucket_arr_get(&p->ast->nodes, *idx);

	if (p->previous.type) {
		n->location = p->previous.location;
		n->data = p->previous.data;
	}

	return n;
}

uint32_t *
get_node_child(struct node *n, uint32_t c)
{
	enum node_child_flag chflg = 1 << c;
	assert(chflg & n->chflg);
	switch (chflg) {
	case node_child_l:
		return &n->l;
	case node_child_r:
		return &n->r;
	case node_child_c:
		return &n->c;
	case node_child_d:
		return &n->d;
	}

	assert(false && "unreachable");
	return 0;
}

static void
add_child(struct parser *p, uint32_t parent, enum node_child_flag chflg, uint32_t c_id)
{
	struct node *n = bucket_arr_get(&p->ast->nodes, parent);
	assert(!(chflg & n->chflg) && "you tried to set the same child more than once");
	n->chflg |= chflg;

	switch (chflg) {
	case node_child_l:
		n->l = c_id;
		break;
	case node_child_r:
		n->r = c_id;
		break;
	case node_child_c:
		n->c = c_id;
		break;
	case node_child_d:
		n->d = c_id;
		break;
	}
}

void
print_ast_at(struct workspace *wk, struct ast *ast, uint32_t id, uint32_t d, char label)
{
	struct node *n = get_node(ast, id);
	uint32_t i;

	for (i = 0; i < d; ++i) {
		printf("  ");
	}

	printf("%d:%c:%s", id, label, node_to_s(wk, n));
	if (n->comments.len) {
		printf("#%d", n->comments.len);
	}
	printf("\n");

	static const char *child_labels = "lrcd";

	for (i = 0; i < NODE_MAX_CHILDREN; ++i) {
		if ((1 << i) & n->chflg) {
			print_ast_at(wk, ast, *get_node_child(n, i), d + 1, child_labels[i]);
		}
	}
}

void
print_ast(struct workspace *wk, struct ast *ast)
{
	print_ast_at(wk, ast, ast->root, 0, 'l');
}

const char *
node_to_s(struct workspace *wk, const struct node *n)
{
	static char buf[BUF_SIZE_S + 1];
	uint32_t i = 0;

	i += snprintf(&buf[i], BUF_SIZE_S - i, "%s", node_type_to_s(n->type));

	switch (n->type) {
	case node_id:
		i += snprintf(&buf[i], BUF_SIZE_S - i, ":%s", get_cstr(wk, n->data.str));
		break;
	case node_string:
		i += obj_snprintf(wk, &buf[i], BUF_SIZE_S - i, ":%o", n->data.str);
		break;
	case node_number:
		i += snprintf(&buf[i], BUF_SIZE_S - i, ":%" PRId64, n->data.num);
		break;
	case node_argument:
		i += snprintf(&buf[i], BUF_SIZE_S - i, ":%s", n->subtype == arg_kwarg ? "kwarg" : "normal");
		break;
	case node_if: {
		const char *l = NULL;
		if (n->subtype == if_if) {
			l = "if";
		} else if (n->subtype == if_elseif) {
			l = "elseif";
		} else if (n->subtype == if_else) {
			l = "else";
		}

		i += snprintf(&buf[i], BUF_SIZE_S - i, ":%s", l);
		break;
	}
	case node_arithmetic: {
		const char *s;
		switch ((enum arithmetic_type)n->subtype) {
		case arith_add:
			s = "+";
			break;
		case arith_sub:
			s = "-";
			break;
		case arith_mod:
			s = "%";
			break;
		case arith_mul:
			s = "*";
			break;
		case arith_div:
			s = "/";
			break;
		default:
			assert(false);
			return NULL;
		}

		i += snprintf(&buf[i], BUF_SIZE_S - i, " op '%s'", s);
		break;
	}
	default:
		break;
	}

	return buf;
}

static bool
check_binary_operands(struct parser *p, uint32_t l_id, uint32_t r_id, struct token *err_tok)
{
	if (get_node(p->ast, l_id)->type == node_empty
	    || get_node(p->ast, r_id)->type == node_empty) {
		parse_error(p, err_tok, "missing operand to binary operator");
		return false;
	}

	return true;
}

static bool
ensure_in_loop(struct parser *p)
{
	if (!p->loop_depth) {
		parse_error(p, NULL, "statement not allowed outside of a foreach loop");
		return false;
	}

	return true;
}

static bool
parse_type(struct parser *p, type_tag *type, bool top_level)
{
	*type = 0;

	const char *typestr = 0;
	if (accept(p, token_type_identifier)) {
		typestr = get_cstr(p->wk, p->previous.data.str);
		type_tag t;
		if (s_to_type_tag(typestr, &t)) {
			*type = t;
		} else {
			parse_error(p, NULL, "unknown type %s", typestr);
			return false;
		}
	} else if (accept(p, token_type_func)) {
		*type = tc_func;
	}

	if (!top_level) {
		const char *err_type = 0;
		if ((*type & TYPE_TAG_LISTIFY)) {
			err_type = "listify";
		} else if ((*type & TYPE_TAG_GLOB)) {
			err_type = "glob";
		}

		if (err_type) {
			parse_error(p, &p->previous, "%s can only be specified as the top level type", err_type);
			return false;
		}
	}

	bool has_sub_type = *type == TYPE_TAG_LISTIFY || *type == TYPE_TAG_GLOB
			    || *type == tc_dict || *type == tc_array;

	if (has_sub_type) {
		if (!accept(p, token_type_lbrack)) {
			parse_error(p, &p->previous, "the type %s requires a sub type (e.g. %s[any])", typestr, typestr);
			return false;
		}

		type_tag sub_type;
		if (!parse_type(p, &sub_type, false)) {
			return false;
		}

		if (!sub_type) {
			parse_error(p, &p->previous, "expected type");
		}

		if (!expect(p, token_type_rbrack)) {
			return false;
		}

		if (*type == TYPE_TAG_LISTIFY || *type == TYPE_TAG_GLOB) {
			*type |= sub_type;
		} else if (*type == tc_dict || *type == tc_array) {
			*type = make_complex_type(p->wk, complex_type_nested, *type, sub_type);
		} else {
			UNREACHABLE;
		}
	}

	if (accept(p, token_type_bitor)) {
		type_tag ord_type;
		if (!parse_type(p, &ord_type, false)) {
			return false;
		}

		if ((ord_type & TYPE_TAG_COMPLEX) || (*type & TYPE_TAG_COMPLEX)) {
			*type = make_complex_type(p->wk, complex_type_or, *type, ord_type);
		} else {
			*type |= ord_type;
		}
	}

	return true;
}

typedef bool (*parse_func)(struct parser *, uint32_t *);
static bool parse_expr(struct parser *p, uint32_t *id);
static bool parse_block(struct parser *p, uint32_t *id);

enum parse_list_mode {
	parse_list_mode_array = 1 << 0,
	parse_list_mode_dictionary = 1 << 1,
	parse_list_mode_arguments = 1 << 2,
	parse_list_mode_tail = 1 << 3,
	parse_list_mode_types = 1 << 4,
};

static bool
parse_list_recurse(struct parser *p, uint32_t *id, enum parse_list_mode mode)
{
	uint32_t s_id, c_id, v_id;
	enum arg_type at = arg_normal;
	make_node(p, id, node_argument);
	struct node *n;

	if ((p->mode & pm_keep_formatting) && (p->current.type == token_type_comment || p->current.type == token_type_fmt_eol)) {
		make_node(p, &s_id, node_empty_line);
		accept(p, token_type_fmt_eol);
	} else if (mode == parse_list_mode_tail) {
		*id = 0;
		return true;
	} else {
		p->preserve_fmt_eol = false;
		if (!parse_expr(p, &s_id)) {
			return false;
		}
		p->preserve_fmt_eol = true;

		if (get_node(p->ast, s_id)->type == node_empty) {
			*id = s_id;
			return true;
		}

		if (mode & parse_list_mode_types) {
			type_tag type;
			if (!parse_type(p, &type, true)) {
				return false;
			} else if (!type) {
				parse_error(p, &p->previous, "missing type specifier for function argument");
				return false;
			}

			n = get_node(p->ast, *id);
			n->data.type = type;
		}

		bool have_colon = false;

		if (mode & parse_list_mode_arguments) {
			have_colon = accept(p, token_type_colon);
		} else if (mode == parse_list_mode_dictionary) {
			if (!expect(p, token_type_colon)) {
				return false;
			}
			have_colon = true;
		}

		if (have_colon) {
			at = arg_kwarg;

			if ((mode & parse_list_mode_arguments)
			    && get_node(p->ast, s_id)->type != node_id) {
				parse_error(p, NULL, "keyword argument key must be a plain identifier (not a %s)",
					node_type_to_s(get_node(p->ast, s_id)->type));
				return false;
			}

			p->preserve_fmt_eol = false;
			if (!parse_expr(p, &v_id)) {
				return false;
			}
			p->preserve_fmt_eol = true;

			if (get_node(p->ast, v_id)->type == node_empty && !p->parsing_func_def_args) {
				parse_error(p, &p->previous, "missing value");
				return false;
			}
		}

		if (!accept(p, token_type_comma)) {
			mode = parse_list_mode_tail;
		}

		if ((p->mode & pm_keep_formatting)) {
			accept(p, token_type_fmt_eol);
		}
	}

	if (!parse_list_recurse(p, &c_id, mode)) {
		return false;
	}

	n = get_node(p->ast, *id);
	n->subtype = at;

	add_child(p, *id, node_child_l, s_id);
	if (at == arg_kwarg) {
		add_child(p, *id, node_child_r, v_id);
	}
	if (c_id) {
		add_child(p, *id, node_child_c, c_id);
	}

	return true;
}

static bool
parse_list(struct parser *p, uint32_t *id, enum parse_list_mode mode)
{
	p->preserve_fmt_eol = true;
	if (p->mode & pm_keep_formatting) {
		accept(p, token_type_fmt_eol);
	}

	if (!parse_list_recurse(p, id, mode)) {
		return false;
	}

	if (p->mode & pm_keep_formatting) {
		accept(p, token_type_fmt_eol);
	}

	assert(p->preserve_fmt_eol);
	p->preserve_fmt_eol = false;
	return true;
}

static bool
parse_index_call(struct parser *p, uint32_t *id, uint32_t l_id, bool have_l)
{
	uint32_t r_id;

	if (!parse_expr(p, &r_id)) {
		return false;
	}

	if (get_node(p->ast, r_id)->type == node_empty) {
		UNREACHABLE;
	}

	if (!expect(p, token_type_rbrack)) {
		return false;
	}

	make_node(p, id, node_index);
	if (have_l) {
		add_child(p, *id, node_child_l, l_id);
	}

	add_child(p, *id, node_child_r, r_id);

	return true;
}

static bool
parse_func_def(struct parser *p, uint32_t *id)
{
	make_node(p, id, node_func_def);

	uint32_t args, l_id, c_id;

	if (!expect(p, token_type_identifier)) {
		return false;
	}
	make_node(p, &l_id, node_id);

	p->parsing_func_def_args = true;
	if (!expect(p, token_type_lparen)) {
		return false;
	} else if (!parse_list(p, &args, parse_list_mode_arguments | parse_list_mode_types)) {
		return false;
	} else if (!expect(p, token_type_rparen)) {
		return false;
	}
	p->parsing_func_def_args = false;

	if (accept(p, token_type_returntype)) {
		type_tag type;
		if (!parse_type(p, &type, true)) {
			return false;
		}

		get_node(p->ast, *id)->data.type = type;
	}

	if (!expect(p, token_type_eol)) {
		return false;
	}

	++p->func_depth;
	if (!parse_block(p, &c_id)) {
		return false;
	}
	--p->func_depth;

	if (get_node(p->ast, c_id)->type != node_empty) {
		p->caused_effect = true;
	}

	add_child(p, *id, node_child_l, l_id);
	add_child(p, *id, node_child_r, args);
	add_child(p, *id, node_child_c, c_id);

	return true;
}

static bool
parse_fstring(struct parser *p, uint32_t *id)
{
	uint32_t i, j;
	const struct str *fstr = get_str(p->wk, p->previous.data.str);
	struct str str = { fstr->s }, identifier;

	struct node *n, *c;

	n = make_node(p, id, node_arithmetic);
	n->subtype = arith_add;
	n->chflg = node_child_l | node_child_r;
	c = make_node(p, &n->l, node_string);
	c->data.str = c->l = make_str(p->wk, "");

	for (i = 0; i < fstr->len;) {
		if (fstr->s[i] == '@' && is_valid_start_of_identifier(fstr->s[i + 1])) {
			for (j = i + 1; j < fstr->len && is_valid_inside_of_identifier(fstr->s[j]); ++j) {
			}

			if (fstr->s[j] == '@' && (identifier.len = j - i - 1) > 0) {
				identifier.s = &fstr->s[i + 1];

				n = make_node(p, &n->r, node_arithmetic);
				n->subtype = arith_add;
				n->chflg = node_child_l | node_child_r;

				c = make_node(p, &n->l, node_string);
				c->l = c->data.str = make_strn(p->wk, str.s, str.len);

				n = make_node(p, &n->r, node_arithmetic);
				n->subtype = arith_add;
				n->chflg = node_child_l | node_child_r;

				c = make_node(p, &n->l, node_stringify);
				c->chflg = node_child_l;
				c = make_node(p, &c->l, node_id);
				c->l = c->data.str = make_strn(p->wk, identifier.s, identifier.len);

				i = j + 1;

				str = (struct str) { &fstr->s[i] };
				continue;
			}
		}

		++str.len;
		++i;
	}

	c = make_node(p, &n->r, node_string);
	c->l = c->data.str = make_strn(p->wk, str.s, str.len);

	return true;
}

static bool
parse_e9(struct parser *p, uint32_t *id)
{
	struct node *n;

	if (accept(p, token_type_true)) {
		n = make_node(p, id, node_bool);
		n->subtype = 1;
		n->l = obj_bool_true;
	} else if (accept(p, token_type_false)) {
		n = make_node(p, id, node_bool);
		n->subtype = 0;
		n->l = obj_bool_false;
	} else if (accept(p, token_type_identifier)) {
		make_node(p, id, node_id);
	} else if (accept(p, token_type_number)) {
		n = make_node(p, id, node_number);
		make_obj(p->wk, &n->l, obj_number);
		set_obj_number(p->wk, n->l, n->data.num);
	} else if (accept(p, token_type_string)) {
		n = make_node(p, id, node_string);
		n->data = p->previous.data;
		n->l = p->previous.data.str;
	} else if (accept(p, token_type_fstring)) {
		return parse_fstring(p, id);
	} else {
		make_node(p, id, node_empty);
	}

	return true;
}

static bool
parse_method_call(struct parser *p, uint32_t *id, uint32_t l_id, bool have_l)
{
	p->caused_effect = true;

	uint32_t meth_id, args, c_id = 0;
	bool have_c = false;
	struct token start_tok = p->current;
	struct node *n;

	if (!expect(p, token_type_identifier)) {
		return false;
	}
	make_node(p, &meth_id, node_id);

	make_node(p, id, node_method);

	if (!expect(p, token_type_lparen)) {
		return false;
	} else if (!parse_list(p, &args, parse_list_mode_arguments)) {
		return false;
	} else if (!expect(p, token_type_rparen)) {
		return false;
	}

	struct node *args_n = get_node(p->ast, *id);
	n = get_node(p->ast, *id);
	args_n->location = n->location;

	n = get_node(p->ast, *id);
	n->subtype = have_c;

	if (have_l) {
		add_child(p, *id, node_child_l, l_id);
	}

	if (get_node(p->ast, meth_id)->type == node_empty) {
		parse_error(p, &start_tok, "missing method name");
		return false;
	}

	add_child(p, *id, node_child_r, meth_id);
	add_child(p, *id, node_child_c, args);

	if (have_c) {
		add_child(p, *id, node_child_d, c_id);
	}

	return true;
}

static bool
parse_e8(struct parser *p, uint32_t *id)
{
	uint32_t v;

	if (accept(p, token_type_lparen)) {
		if (p->mode & pm_keep_formatting) {
			make_node(p, id, node_paren);
		}

		if (!parse_expr(p, &v)) {
			return false;
		}

		if (!expect(p, token_type_rparen)) {
			return false;
		}

		if (get_node(p->ast, v)->type == node_empty) {
			parse_error(p, &p->previous, "unexpected token '%s'", token_type_to_s(token_type_rparen));
			return false;
		}

		if (p->mode & pm_keep_formatting) {
			uint32_t rparen;
			make_node(p, &rparen, node_paren);

			add_child(p, *id, node_child_l, v);
			add_child(p, *id, node_child_r, rparen);
		} else {
			*id = v;
		}
	} else if (accept(p, token_type_lbrack)) {
		make_node(p, id, node_array);

		if (!parse_list(p, &v, parse_list_mode_array)) {
			return false;
		}

		if (!expect(p, token_type_rbrack)) {
			return false;
		}

		add_child(p, *id, node_child_l, v);
		accept_comment_for(p, v);
	} else if (accept(p, token_type_lcurl)) {
		make_node(p, id, node_dict);

		if (!parse_list(p, &v, parse_list_mode_dictionary)) {
			return false;
		}

		if (!expect(p, token_type_rcurl)) {
			return false;
		}

		add_child(p, *id, node_child_l, v);
		accept_comment_for(p, v);
	} else {
		return parse_e9(p, id);
	}

	return true;
}

static bool
parse_chained(struct parser *p, uint32_t *id, uint32_t l_id, bool have_l)
{
	bool loop = false, l_empty = have_l && get_node(p->ast, l_id)->type == node_empty;

	*id = 0;

	if (accept(p, token_type_dot)) {
		if (l_empty) {
			parse_error(p, &p->previous, "unexpected token '%s'", token_type_to_s(token_type_dot));
			return false;
		}

		loop = true;

		if (!parse_method_call(p, id, l_id, have_l)) {
			return false;
		}
	} else if (accept(p, token_type_lbrack)) {
		if (l_empty) {
			parse_error(p, &p->previous, "unexpected token '%s'", token_type_to_s(token_type_lbrack));
			return false;
		}

		loop = true;

		if (!parse_index_call(p, id, l_id, have_l)) {
			return false;
		}
	} else if (accept(p, token_type_lparen)) {
		if (l_empty) {
			parse_error(p, &p->previous, "unexpected token '%s'", token_type_to_s(token_type_lparen));
			return false;
		}

		loop = true;

		p->caused_effect = true;

		make_node(p, id, node_function);

		obj args;
		if (!parse_list(p, &args, parse_list_mode_arguments)) {
			return false;
		} else if (!expect(p, token_type_rparen)) {
			return false;
		}

		if (have_l) {
			add_child(p, *id, node_child_l, l_id);
		}
		add_child(p, *id, node_child_r, args);
	}

	if (*id) {
		accept_comment_for(p, *id);
	}

	if (loop) {
		uint32_t child_id;
		if (!parse_chained(p, &child_id, 0, false)) {
			return false;
		}

		if (child_id) {
			add_child(p, *id, node_child_d, child_id);
		}
		return true;
	} else {
		*id = l_id;
		return true;
	}
}

static bool
parse_e7(struct parser *p, uint32_t *id)
{
	uint32_t l_id;
	if (!(parse_e8(p, &l_id))) {
		return false;
	}

	if (!parse_chained(p, id, l_id, true)) {
		return false;
	}

	return true;
}

static bool
parse_e6(struct parser *p, uint32_t *id)
{
	uint32_t l_id;
	enum node_type t = 0;

	if (accept(p, token_type_not)) {
		t = node_not;
	} else if (accept(p, token_type_minus)) {
		t = node_u_minus;
	}

	struct token op_tok = p->previous;

	if (!(parse_e7(p, &l_id))) {
		return false;
	}

	if (t) {
		if (get_node(p->ast, l_id)->type == node_empty) {
			parse_error(p, &op_tok, "missing operand to unary operator");
			return false;
		}
		make_node(p, id, t);
		add_child(p, *id, node_child_l, l_id);
	} else {
		*id = l_id;
	}

	return true;
}

static bool parse_arith(struct parser *p, uint32_t *id, enum arithmetic_type type);

static bool
parse_arith_upper(struct parser *p, uint32_t *id, enum arithmetic_type type)
{
	if (type == arith_div) {
		return parse_e6(p, id);
	} else {
		return parse_arith(p, id, type + 1);
	}
}

static bool
parse_arith(struct parser *p, uint32_t *id, enum arithmetic_type type)
{
	const struct {
		enum token_type tok;
	} op_map[] = {
		[arith_add] = { token_type_plus,   },
		[arith_sub] = { token_type_minus,  },
		[arith_mod] = { token_type_modulo, },
		[arith_mul] = { token_type_star,   },
		[arith_div] = { token_type_slash,  },
	};

	struct node *n;

	uint32_t l_id, r_id;

	if (!(parse_arith_upper(p, &l_id, type))) {
		return false;
	}

	struct token op_tok;

	enum token_type tok = op_map[type].tok;

	while (accept(p, tok)) {
		op_tok = p->previous;

		if (!(parse_arith_upper(p, &r_id, type))) {
			return false;
		}

		if (op_tok.type) {
			p->previous = op_tok;
		}
		if (!check_binary_operands(p, l_id, r_id, &op_tok)) {
			return false;
		}

		uint32_t new_l;
		n = make_node(p, &new_l, node_arithmetic);
		n->subtype = type;
		add_child(p, new_l, node_child_l, l_id);
		add_child(p, new_l, node_child_r, r_id);

		l_id = new_l;
	}

	*id = l_id;
	return true;
}

static bool
parse_e5(struct parser *p, uint32_t *id)
{
	return parse_arith(p, id, arith_add);
}

static bool
make_comparison_node(struct parser *p, uint32_t *id, uint32_t l_id, enum comparison_type comp)
{
	uint32_t r_id;
	struct node *n = make_node(p, id, node_comparison);
	n->subtype = comp;

	struct token comp_op = p->previous;

	if (!(parse_e5(p, &r_id))) {
		return false;
	}

	if (!check_binary_operands(p, l_id, r_id, &comp_op)) {
		return false;
	}

	add_child(p, *id, node_child_l, l_id);
	add_child(p, *id, node_child_r, r_id);

	return true;
}

static bool
parse_e4(struct parser *p, uint32_t *id)
{
	static enum token_type map[] = {
		[comp_equal] = token_type_eq,
		[comp_nequal] = token_type_neq,
		[comp_lt] = token_type_lt,
		[comp_le] = token_type_leq,
		[comp_gt] = token_type_gt,
		[comp_ge] = token_type_geq,
		[comp_in] = token_type_in,
	};

	uint32_t i, l_id;
	if (!(parse_e5(p, &l_id))) {
		return false;
	}

	for (i = 0; i < comp_not_in; ++i) {
		if (accept(p, map[i])) {
			return make_comparison_node(p, id, l_id, i);
		}
	}

	if (accept(p, token_type_not) && accept(p, token_type_in)) {
		return make_comparison_node(p, id, l_id, comp_not_in);
	}

	*id = l_id;
	return true;
}

static bool
parse_e3(struct parser *p, uint32_t *id)
{
	uint32_t l_id, r_id;
	if (!(parse_e4(p, &l_id))) {
		return false;
	}

	if (accept(p, token_type_and)) {
		struct token and = p->previous;

		if (!parse_e3(p, &r_id)) {
			return false;
		}

		if (!check_binary_operands(p, l_id, r_id, &and)) {
			return false;
		}

		make_node(p, id, node_and);
		add_child(p, *id, node_child_l, l_id);
		add_child(p, *id, node_child_r, r_id);
	} else {
		*id = l_id;
	}

	return true;
}

static bool
parse_e2(struct parser *p, uint32_t *id)
{
	uint32_t l_id, r_id;
	if (!(parse_e3(p, &l_id))) {
		return false;
	}

	if (accept(p, token_type_or)) {
		struct token or = p->previous;

		if (!parse_e2(p, &r_id)) {
			return false;
		}

		if (!check_binary_operands(p, l_id, r_id, &or)) {
			return false;
		}

		make_node(p, id, node_or);
		add_child(p, *id, node_child_l, l_id);
		add_child(p, *id, node_child_r, r_id);
	} else {
		*id = l_id;
	}

	return true;
}

static bool
parse_expr(struct parser *p, uint32_t *id)
{
	bool ret = false;
	uint32_t l_id = 0; // compiler thinks this won't get initialized...

	if (++p->parse_depth > 4096) {
		parse_error(p, NULL, "stack overflow while parsing nested expression");
		goto ret;
	}

	if (!(parse_e2(p, &l_id))) {
		goto ret;
	}

	if (accept(p, token_type_question_mark)) {
		uint32_t a, b;

		if (!(parse_expr(p, &a))) {
			goto ret;
		} else if (!expect(p, token_type_colon)) {
			goto ret;
		} else if (!(parse_expr(p, &b))) {
			goto ret;
		}

		/* NOTE: a bare ?: is actually valid in meson, none of the
		 * fields have to be filled. I'm making it an error here though,
		 * because missing fields in ternary expressions is probably an
		 * error
		 */
		if (get_node(p->ast, l_id)->type == node_empty) {
			parse_error(p, NULL, "missing condition expression");
			goto ret;
		} else if (get_node(p->ast, a)->type == node_empty) {
			parse_error(p, NULL, "missing true expression");
			goto ret;
		} else if (get_node(p->ast, b)->type == node_empty) {
			parse_error(p, NULL, "missing false expression");
			goto ret;
		}

		make_node(p, id, node_ternary);
		add_child(p, *id, node_child_l, l_id);
		add_child(p, *id, node_child_r, a);
		add_child(p, *id, node_child_c, b);
	} else {
		*id = l_id;
	}

	ret = true;
ret:
	--p->parse_depth;
	return ret;
}

static bool
parse_assignment(struct parser *p, uint32_t *id)
{
	uint32_t l_id = 0;
	if (!(parse_expr(p, &l_id))) {
		return false;
	}

	if (accept(p, token_type_plus_assign)) {
		p->caused_effect = true;

		uint32_t v, arith;
		make_node(p, &arith, node_arithmetic);

		if (get_node(p->ast, l_id)->type != node_id) {
			parse_error(p, NULL, "assignment target must be an id (got %s)", node_to_s(p->wk, get_node(p->ast, l_id)));
			return false;
		} else if (!parse_expr(p, &v)) {
			return false;
		}

		if (get_node(p->ast, v)->type == node_empty) {
			parse_error(p, NULL, "missing rhs");
			return false;
		}

		struct node *n = get_node(p->ast, arith);
		n->type = node_plusassign;
		add_child(p, arith, node_child_l, l_id);
		add_child(p, arith, node_child_r, v);
		*id = arith;
	} else if (accept(p, token_type_assign)) {
		p->caused_effect = true;

		uint32_t v;
		make_node(p, id, node_assignment);

		if (get_node(p->ast, l_id)->type != node_id) {
			parse_error(p, NULL, "assignment target must be an id (got %s)", node_to_s(p->wk, get_node(p->ast, l_id)));
			return false;
		} else if (!parse_expr(p, &v)) {
			return false;
		}

		if (get_node(p->ast, v)->type == node_empty) {
			parse_error(p, NULL, "missing rhs");
			return false;
		}

		add_child(p, *id, node_child_l, l_id);
		add_child(p, *id, node_child_r, v);
	} else {
		*id = l_id;
	}

	return true;
}

static bool
parse_if(struct parser *p, uint32_t *id, enum if_type if_type)
{
	enum if_type child_type;
	uint32_t cond_id, block_id, c_id;
	bool have_c = false;

	struct token if_ = p->current;
	if (if_type == if_if || if_type == if_elseif) {
		if (!parse_expr(p, &cond_id)) {
			return false;
		}

		if (get_node(p->ast, cond_id)->type == node_empty) {
			parse_error(p, &if_, "missing condition");
			return false;
		}
	}

	if (!expect(p, token_type_eol)) {
		return false;
	}

	if (!parse_block(p, &block_id)) {
		return false;
	}

	if (if_type == if_if || if_type == if_elseif) {
		if (accept(p, token_type_elif)) {
			have_c = true;
			child_type = if_elseif;
		} else if (accept(p, token_type_else)) {
			have_c = true;
			child_type = if_else;
		}

		if (have_c) {
			if (!parse_if(p, &c_id, child_type)) {
				return false;
			}
		}
	}

	struct node *n = make_node(p, id, node_if);
	n->subtype = if_type;

	if (if_type == if_if || if_type == if_elseif) {
		add_child(p, *id, node_child_l, cond_id);
	}

	if (get_node(p->ast, block_id)->type != node_empty) {
		p->caused_effect = true;
	}
	add_child(p, *id, node_child_r, block_id);

	if (have_c) {
		add_child(p, *id, node_child_c, c_id);
	}

	return true;
}

static bool
parse_foreach_args(struct parser *p, uint32_t *id, uint32_t d)
{
	uint32_t l_id, r_id;
	bool have_r = false;

	if (!expect(p, token_type_identifier)) {
		return false;
	}

	make_node(p, &l_id, node_id);

	if (d <= 0 && accept(p, token_type_comma)) {
		have_r = true;
		if (!parse_foreach_args(p, &r_id, d + 1)) {
			return false;
		}
	}

	make_node(p, id, node_foreach_args);
	add_child(p, *id, node_child_l, l_id);

	if (have_r) {
		add_child(p, *id, node_child_r, r_id);
	}

	return true;
}

static bool
parse_foreach(struct parser *p, uint32_t *id)
{
	uint32_t args_id, r_id, c_id;

	make_node(p, id, node_foreach);

	if (!parse_foreach_args(p, &args_id, 0)) {
		return false;
	}

	struct token colon = p->current;

	if (!expect(p, token_type_colon)) {
		return false;
	} else if (!parse_expr(p, &r_id)) {
		return false;
	}

	if (!expect(p, token_type_eol)) {
		return false;
	}

	if (get_node(p->ast, r_id)->type == node_empty) {
		parse_error(p, &colon, "expected statement");
		return false;
	}

	++p->loop_depth;
	if (!parse_block(p, &c_id)) {
		return false;
	}
	--p->loop_depth;

	if (get_node(p->ast, c_id)->type != node_empty) {
		p->caused_effect = true;
	}

	add_child(p, *id, node_child_l, args_id);
	add_child(p, *id, node_child_r, r_id);
	add_child(p, *id, node_child_c, c_id);

	return true;
}

static bool
parse_line(struct parser *p, uint32_t *id)
{
	if (p->mode & pm_keep_formatting) {
		struct token *next = NULL;

		assert(false && "TODO");
		/* if (p->token_i < p->toks->tok.len) { */
		/* 	next = arr_get(&p->toks->tok, p->token_i); */
		/* } */

		if (p->current.type == token_type_eol ||
		    (next && p->current.type == token_type_comment && next->type == token_type_eol)) {
			make_node(p, id, node_empty_line);
			return true;
		}
	} else {
		while (accept(p, token_type_eol)) {
		}
	}

	switch (p->current.type) {
	case token_type_endforeach:
	case token_type_else:
	case token_type_elif:
	case token_type_endif:
	case token_type_endfunc:
	case token_type_eof:
		make_node(p, id, node_empty);
		return true;
	default:
		break;
	}

	bool caused_effect_old = p->caused_effect,
	     ret = true;
	p->caused_effect = false;

	struct token stmt_start = p->current;

	if (accept(p, token_type_if)) {
		if (!parse_if(p, id, if_if)) {
			ret = false;
			consume_until(p, token_type_endif);
		}

		if (!expect(p, token_type_endif)) {
			return false;
		}
	} else if (accept(p, token_type_foreach)) {
		if (!parse_foreach(p, id)) {
			ret = false;
			consume_until(p, token_type_endforeach);
		}

		if (!expect(p, token_type_endforeach)) {
			return false;
		}
	} else if ((p->mode & pm_functions) && accept(p, token_type_return)) {
		p->caused_effect = true;

		make_node(p, id, node_return);

		uint32_t l_id;
		if (!parse_expr(p, &l_id)) {
			return false;
		}

		add_child(p, *id, node_child_l, l_id);
	} else if (accept(p, token_type_continue)) {
		p->caused_effect = true;

		if (!ensure_in_loop(p)) {
			return false;
		}

		make_node(p, id, node_continue);
	} else if (accept(p, token_type_break)) {
		p->caused_effect = true;

		if (!ensure_in_loop(p)) {
			return false;
		}

		make_node(p, id, node_break);
	} else {
		if ((p->mode & pm_functions) && accept(p, token_type_func)) {
			if (!parse_func_def(p, id)) {
				ret = false;
				consume_until(p, token_type_endfunc);
			}

			if (!expect(p, token_type_endfunc)) {
				return false;
			}
		} else {
			if (!parse_assignment(p, id)) {
				return false;
			}
		}
	}

	if (!(p->mode & pm_ignore_statement_with_no_effect)
	    && ret && !p->caused_effect) {
		parse_error(p, &stmt_start, "statement with no effect");
		return false;
	}

	p->caused_effect = caused_effect_old;

	if (ret) {
		struct node *res = get_node(p->ast, *id);
		res->location = stmt_start.location;
	}

	return ret;
}

static bool
parse_block(struct parser *p, uint32_t *id)
{
	uint32_t l_id, r_id;
	bool loop = true, have_eol = true, have_r = false;

	make_node(p, id, node_block);

	while (loop) {
		if (parse_line(p, &l_id)) {
			if (get_node(p->ast, l_id)->type != node_empty) {
				loop = false;
			}
		} else {
			// just make a dummy node here since l_id may not have
			// been initialized.  Set the type to something other
			// than node_empty so we continue parsing.
			make_node(p, &l_id, node_block);
			p->valid = false;
			loop = false;
			consume_until(p, token_type_eol);
		}

		if (!accept(p, token_type_eol)) {
			have_eol = false;
			break;
		}
	}

	if (have_eol) {
		if (!parse_block(p, &r_id)) {
			return false;
		}
		have_r = true;
	}

	if (get_node(p->ast, l_id)->type == node_empty) {
		assert(!have_r);
		*id = l_id;
		return true;
	}

	add_child(p, *id, node_child_l, l_id);
	if (have_r) {
		add_child(p, *id, node_child_r, r_id);
	}

	return true;
}

bool
parser_parse(struct workspace *wk, struct ast *ast, struct source *src, enum parse_mode mode)
{
	TracyCZoneAutoS;
	bool ret = false;

	enum lexer_mode lexer_mode = 0;
	if (mode & pm_keep_formatting) {
		lexer_mode |= lexer_mode_format;
	}
	if (mode & pm_functions) {
		lexer_mode |= lexer_mode_functions;
	}

	struct parser parser = {
		.src = src,
		.ast = ast,
		.valid = true,
		.mode = mode | pm_ignore_statement_with_no_effect,
		.wk = wk,
	};

	lexer_init(&parser.lexer, wk, src, lexer_mode);

	bucket_arr_init(&ast->nodes, 2048, sizeof(struct node));

	if (mode & pm_keep_formatting) {
		arr_init(&ast->comments, 2048, sizeof(char *));
	}

	uint32_t id;
	make_node(&parser, &id, node_null);
	assert(id == 0);

	parse_advance(&parser);

	if (!parse_block(&parser, &ast->root)) {
		goto ret;
	} else if (!expect(&parser, token_type_eof)) {
		goto ret;
	}

	ret = parser.valid;
ret:
	TracyCZoneAutoE;
	return ret;
}

void
ast_destroy(struct ast *ast)
{
	bucket_arr_destroy(&ast->nodes);
	arr_destroy(&ast->comments);
}

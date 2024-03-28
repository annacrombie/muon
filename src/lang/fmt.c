/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: Ailin Nemui <ailin@d5421s.localdomain>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "buf_size.h"
#include "error.h"
#include "formats/editorconfig.h"
#include "formats/ini.h"
#include "lang/fmt.h"
#include "lang/interpreter.h"
#include "lang/string.h"
#include "lang/typecheck.h"
#include "log.h"
#include "platform/mem.h"
#include "platform/path.h"

struct arg_elem {
	uint32_t kw, val, next;
	type_tag type;
};

struct fmt_ctx {
	struct ast *ast;
	struct workspace *wk;
	struct sbuf *out_buf;
	uint32_t indent, col, enclosed;
	bool force_ml;
	bool trailing_comment;

	/* options */
	bool space_array, kwa_ml, wide_colon, no_single_comma_function;
	uint32_t max_line_len;
	const char *indent_by;
};

enum special_fmt {
	special_fmt_sort_files = 1 << 0,
	special_fmt_collapse_lone_array_arg = 1 << 1,
	special_fmt_cmd_array  = 1 << 2,
};

struct fmt_stack {
	uint32_t parent;
	const char *node_sep;
	const char *arg_container;
	enum special_fmt special_fmt;
	bool write;
	bool ml;
	bool fmt_and_or_force_ml;
};

typedef uint32_t ((*fmt_func)(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id));
static uint32_t fmt_node(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id);
static uint32_t fmt_chain(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id);

static const char *
get_comment(struct fmt_ctx *ctx, struct node *n, uint32_t i)
{
	return *(const char **)arr_get(&ctx->ast->comments, n->comments.start + i);
}

static bool
fmt_str_startwith(struct fmt_ctx *ctx, uint32_t n_id, const char *pre)
{
	struct node *n = get_node(ctx->ast, n_id);
	if (n->type != node_string) {
		return false;
	}

	const char *s = get_cstr(ctx->wk, n->data.str);
	if (*s == 'f') {
		++s;
	}

	if (strncmp(s, "'''", 3) == 0) {
		s += 3;
	} else {
		++s;
	}

	return strncmp(s, pre, strlen(pre)) == 0;
}

static bool
fmt_id_eql(struct fmt_ctx *ctx, uint32_t n_id, const char *id)
{
	struct node *n = get_node(ctx->ast, n_id);
	if (n->type != node_id) {
		return false;
	}

	return str_eql(get_str(ctx->wk, n->data.str), &WKSTR(id));
}

static struct fmt_stack *
fmt_setup_fst(const struct fmt_stack *pfst)
{
	static struct fmt_stack fst;
	fst = *pfst;
	fst.node_sep = NULL;
	return &fst;
}

static uint32_t
fmt_check(struct fmt_ctx *ctx, const struct fmt_stack *pfst, fmt_func func, uint32_t n_id)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);
	fst.node_sep = pfst->node_sep;

	if (!fst.write) {
		return func(ctx, &fst, n_id);
	}

	bool old_force_ml = ctx->force_ml;
	ctx->force_ml = false;

	struct fmt_stack tmp = fst;
	tmp.write = false;
	tmp.ml = false;

	uint32_t len = func(ctx, &tmp, n_id);

	fst.ml = ctx->force_ml || (len + ctx->col > ctx->max_line_len);

	len = func(ctx, &fst, n_id);

	ctx->force_ml = old_force_ml;
	return len;
}

static uint32_t
fmt_write(struct fmt_ctx *ctx, const struct fmt_stack *pfst, char c)
{
	uint32_t len;
	assert(c != '\n');

	if (c == '\t') {
		len = 8;
	} else {
		len = 1;
	}

	if (pfst->write) {
		sbuf_push(NULL, ctx->out_buf, c);
		ctx->col += len;
	}
	return len;
}

static uint32_t
fmt_writeml(struct fmt_ctx *ctx, const struct fmt_stack *pfst, const char *s)
{
	uint32_t len = 0;
	for (; *s; ++s) {
		if (*s == '\n') {
			if (pfst->write) {
				sbuf_push(NULL, ctx->out_buf, '\n');
				ctx->col = 0;
			}

			ctx->force_ml = true;
		} else {
			len += fmt_write(ctx, pfst, *s);
		}
	}

	return len;
}

static uint32_t
fmt_writes(struct fmt_ctx *ctx, const struct fmt_stack *pfst, const char *s)
{
	uint32_t len = 0;
	for (; *s; ++s) {
		len += fmt_write(ctx, pfst, *s);
	}
	return len;
}

static void
fmt_newline(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t next)
{
	uint32_t i;
	if (pfst->ml) {
		ctx->trailing_comment = false;
	}

	if (pfst->write && pfst->ml) {
		sbuf_push(NULL, ctx->out_buf, '\n');
		ctx->col = 0;

		if (next != 0) {
			struct node *n = get_node(ctx->ast, next);
			if (n->type == node_block) {
				n = get_node(ctx->ast, n->l);
			}

			if (n->type == node_empty_line && !n->comments.len) {
				return;
			}
		}

		for (i = 0; i < ctx->indent; ++i) {
			fmt_writes(ctx, pfst, ctx->indent_by);
		}
	}
}

static void
fmt_newline_force(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t next)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);
	fst.ml = true;

	fmt_newline(ctx, &fst, next);
}

static uint32_t
fmt_newline_or_space(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t next)
{
	if (pfst->ml) {
		fmt_newline(ctx, pfst, next);
		return 0;
	} else {
		return fmt_write(ctx, pfst, ' ');
	}
}

static uint32_t
fmt_breaking_space(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t next)
{
	if (ctx->col >= ctx->max_line_len) {
		fmt_newline_force(ctx, pfst, next);
		ctx->force_ml = true;
		return 0;
	} else {
		return fmt_write(ctx, pfst, ' ');
	}
}

static void
fmt_begin_block(struct fmt_ctx *ctx)
{
	++ctx->indent;
}

static void
fmt_end_block(struct fmt_ctx *ctx)
{
	--ctx->indent;
}

MUON_ATTR_FORMAT(printf, 3, 4)
static uint32_t
fmt_writef(struct fmt_ctx *ctx, const struct fmt_stack *pfst, const char *fmt, ...)
{
	uint32_t len;
	va_list args;
	va_start(args, fmt);

	static char buf[BUF_SIZE_4k];
	if (pfst->write) {
		vsnprintf(buf, BUF_SIZE_4k, fmt, args);
		len = fmt_writes(ctx, pfst, buf);
	} else {
		len = vsnprintf(NULL, 0, fmt, args);
	}
	va_end(args);
	return len;
}

static uint32_t
fmt_comments(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id, bool allow_leading_space)
{
	struct node *n = get_node(ctx->ast, n_id);
	uint32_t len = 0;

	if (!n->comments.len) {
		return 0;
	}

	bool leading_space = allow_leading_space && n->type != node_empty_line;

	ctx->force_ml = true;

	uint32_t i;
	for (i = 0; i < n->comments.len; ++i) {
		len += fmt_writef(ctx, pfst, "%s#%s",
			leading_space ? " " : "",
			get_comment(ctx, n, i));

		if (i < n->comments.len - 1) { // && !trailing_line) {
			fmt_newline_force(ctx, pfst, n_id);
		} else {
			if (pfst->write) {
				ctx->trailing_comment = true;
			}
		}

		leading_space = false;
	}

	return len;
}

static uint32_t
fmt_check_trailing_comment_or_space(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	if (ctx->trailing_comment) {
		fmt_newline_force(ctx, pfst, n_id);
		return 0;
	} else {
		fmt_write(ctx, pfst, ' ');
		return 1;
	}
}

static uint32_t
fmt_tail(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	uint32_t len = 0;

	if (pfst->node_sep) {
		len += fmt_writes(ctx, pfst, pfst->node_sep);
	}

	len += fmt_comments(ctx, pfst, n_id, true);
	return len;
}

static int32_t
fmt_files_args_sort_cmp(const void *a, const void *b, void *_ctx)
{
	struct fmt_ctx *ctx = _ctx;
	const struct arg_elem *ae1 = a, *ae2 = b;

	struct node *v1 = get_node(ctx->ast, ae1->val),
		    *v2 = get_node(ctx->ast, ae2->val);

	if (v1->type == node_string && v2->type == node_string) {
		const char *s1 = get_cstr(ctx->wk, v1->data.str), *s2 = get_cstr(ctx->wk, v2->data.str);
		bool s1_hasdir = strchr(s1, '/') != NULL,
		     s2_hasdir = strchr(s2, '/') != NULL;

		if ((s1_hasdir && s2_hasdir) || (!s1_hasdir && !s2_hasdir)) {
			return strcmp(s1, s2);
		} else if (s1_hasdir) {
			return -1;
		} else {
			return 1;
		}
	} else if (v1->type == node_string && v2->type != node_string) {
		return -1;
	} else if (v1->type != node_string && v2->type == node_string) {
		return 1;
	} else {
		return 0;
	}
}

static uint32_t
fmt_args(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_args)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);
	fst.node_sep = pfst->node_sep;

	uint32_t len = 0, kwa = 0;
	struct node *arg = get_node(ctx->ast, n_args),
		    *arg_val;
	bool last = false;

	if (fst.special_fmt & special_fmt_collapse_lone_array_arg) {
		assert(arg->type != node_empty);
		struct node *arr = get_node(ctx->ast, arg->l);
		assert(arr->type == node_array);
		arg = get_node(ctx->ast, arr->l);
	}

	if (arg->type == node_empty) {
		return 0;
	}

	++ctx->enclosed;
	fmt_begin_block(ctx);
	if (pfst->arg_container[0] == '[' && ctx->space_array) {
		fmt_newline_or_space(ctx, &fst, n_args);
	} else {
		fmt_newline(ctx, &fst, n_args);
	}

	struct arr args;
	arr_init(&args, 64, sizeof(struct arg_elem));

	while (arg->type != node_empty) {
		arr_push(&args, &(struct arg_elem) { 0 });
		struct arg_elem *ae = arr_get(&args, args.len - 1);

		if (arg->subtype == arg_kwarg) {
			ae->kw = arg->l;
			ae->val = arg->r;
			++kwa;
		} else {
			ae->kw = 0;
			ae->val = arg->l;
		}

		ae->type = arg->data.type;

		arg_val = get_node(ctx->ast, ae->val);

		{ // deal with empty lines and comment lines
			if (arg_val->type == node_empty_line) {
				ctx->force_ml = true;
			}
		}

		if (arg->chflg & node_child_c) {
			arg = get_node(ctx->ast, arg->c);
			ae->next = arg->subtype == arg_kwarg ? arg->r : arg->l;

			// this means there was a trailing comma
			if (arg->type == node_empty) {
				ctx->force_ml = true;
			}
		} else {
			break;
		}
	}
	if (kwa > 1 && ctx->kwa_ml) {
		ctx->force_ml = true;
	}

	if (fst.special_fmt & special_fmt_sort_files) {
		arr_sort(&args, ctx, fmt_files_args_sort_cmp);
	}

	uint32_t i;
	for (i = 0; i < args.len; ++i) {
		struct arg_elem *ae = arr_get(&args, i);
		fst.special_fmt = 0;

		last = i == args.len - 1;

		struct node *n = get_node(ctx->ast, ae->val);

		if (ae->kw) {
			if (fmt_id_eql(ctx, ae->kw, "command")) {
				fst.special_fmt |= special_fmt_cmd_array;
			}

			const char *kw_sep = ctx->wide_colon ? " : " : ": ";

			if (ae->type) {
				fst.node_sep = 0;
				len += fmt_node(ctx, &fst, ae->kw);
				len += fmt_writes(ctx, &fst, " ");
				len += fmt_writes(ctx, &fst, typechecking_type_to_s(ctx->wk, ae->type));
				fst.node_sep = kw_sep;
				len += fmt_tail(ctx, &fst, ae->kw);
			} else {
				fst.node_sep = kw_sep;
				len += fmt_node(ctx, &fst, ae->kw);
			}
		}

		bool need_comma;
		if (n->type == node_empty_line) {
			// never put commas on empty lines
			need_comma = false;
		} else if (pfst->arg_container[0] == '('
			   && ctx->no_single_comma_function
			   && args.len == 1
			   && !ae->kw) {
			need_comma = false;
		} else if (last && !fst.ml) {
			need_comma = false;
		} else {
			need_comma = true;
		}

		const char *val_sep = need_comma ? "," : NULL;
		if (!ae->kw && ae->type) {
			fst.node_sep = 0;
			len += fmt_node(ctx, &fst, ae->val);
			len += fmt_writes(ctx, &fst, " ");
			len += fmt_writes(ctx, &fst, typechecking_type_to_s(ctx->wk, ae->type));
			fst.node_sep = val_sep;
			len += fmt_tail(ctx, &fst, ae->val);
		} else {
			fst.node_sep = val_sep;
			len += fmt_check(ctx, &fst, fmt_node, ae->val);
		}

		if (!last) {
			if ((pfst->special_fmt & special_fmt_cmd_array)
			    && fmt_str_startwith(ctx, ae->val, "-")
			    && !fmt_str_startwith(ctx, ae->next, "-")) {
				len += fmt_write(ctx, &fst, ' ');
			} else {
				len += fmt_newline_or_space(ctx, &fst, ae->next);
			}
		} else {
			break;
		}
	}

	arr_destroy(&args);

	--ctx->enclosed;
	fmt_end_block(ctx);
	if (pfst->arg_container[1] == ']' && ctx->space_array) {
		fmt_newline_or_space(ctx, &fst, 0);
	}else {
		fmt_newline(ctx, &fst, 0);
	}
	return len;
}

static uint32_t
fmt_arg_container(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);
	/* struct node *p = get_node(ctx->ast, fst.parent); */

	uint32_t len = 0;
	len += fmt_write(ctx, &fst, fst.arg_container[0]);
	len += fmt_comments(ctx, pfst, fst.parent, true);
	/* if (p->comment) { */
	/* 	len += fmt_writef(ctx, pfst, " #%s", p->comment); */
	/* 	ctx->force_ml = true; */
	/* } */

	len += fmt_args(ctx, &fst, n_id);
	len += fmt_write(ctx, &fst, fst.arg_container[1]);
	return len;
}

static uint32_t
fmt_function_common(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_name, uint32_t n_args)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);

	uint32_t len = 0;
	if (n_name) {
		const char *name = get_cstr(ctx->wk, get_node(ctx->ast, n_name)->data.str);
		len += fmt_writes(ctx, &fst, name);
		fst.parent = n_name;
	}

	fst.arg_container = "()";
	len += fmt_arg_container(ctx, &fst, n_args);
	return len;
}

static uint32_t
fmt_function(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);
	fst.special_fmt = 0;

	uint32_t len = 0;
	struct node *f = get_node(ctx->ast, n_id);

	if (fmt_id_eql(ctx, f->l, "files")) {
		// fire only if an array is the lone argument
		struct node *arg = get_node(ctx->ast, f->r);
		if (arg->type != node_empty
		    && arg->subtype != arg_kwarg
		    && (!(arg->chflg & node_child_c)
			|| get_node(ctx->ast, arg->c)->type == node_empty)
		    && get_node(ctx->ast, arg->l)->type == node_array
		    ) {
			fst.special_fmt |= special_fmt_collapse_lone_array_arg;
		}

		fst.special_fmt |= special_fmt_sort_files;
	}

	len += fmt_function_common(ctx, &fst, 0, f->r);
	return len;
}

static uint32_t
fmt_method(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);

	uint32_t len = 0;
	struct node *f = get_node(ctx->ast, n_id);

	len += fmt_function_common(ctx, &fst, f->r, f->c);

	return len;
}

static uint32_t
fmt_node_wrapped(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);

	uint32_t len = 0;
	bool node_needs_paren;

	switch (get_node(ctx->ast, n_id)->type) {
	case node_method:
	case node_array:
	case node_dict:
	case node_function:
	case node_index:
	case node_paren:
	case node_string:
	case node_number:
		node_needs_paren = false;
		break;
	default:
		node_needs_paren = true;
		break;
	}

	node_needs_paren &= ctx->enclosed == 0;

	if (fst.ml && node_needs_paren) {
		++ctx->enclosed;
		len += fmt_write(ctx, &fst, '(');
		fmt_begin_block(ctx);
		fmt_newline(ctx, &fst, n_id);
	}

	len += fmt_node(ctx, &fst, n_id);

	if (fst.ml && node_needs_paren) {
		--ctx->enclosed;
		fmt_end_block(ctx);
		fmt_newline(ctx, &fst, 0);
		len += fmt_write(ctx, &fst, ')');
	}

	return len;
}

static uint32_t
fmt_parens(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);

	uint32_t len = 0;
	struct node *n = get_node(ctx->ast, n_id);

	len += fmt_write(ctx, &fst, '(');
	++ctx->enclosed;

	if (fst.ml) {
		fmt_begin_block(ctx);
		fmt_newline(ctx, &fst, n->l);
	}

	len += fmt_comments(ctx, &fst, n_id, false);
	len += fmt_node(ctx, &fst, n->l);

	if (fst.ml) {
		fmt_end_block(ctx);
		fmt_newline(ctx, &fst, 0);
	}

	--ctx->enclosed;
	len += fmt_write(ctx, &fst, ')');

	return len;
}

static void
fmt_if(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id, bool first)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);

	struct node *n = get_node(ctx->ast, n_id);

	if (n->subtype == if_if || n->subtype == if_elseif) {
		fmt_writef(ctx, &fst, "%s ", first ? "if" : "elif");
		fmt_check(ctx, &fst, fmt_node_wrapped, n->l);
	} else {
		fmt_writes(ctx, &fst, "else");
	}

	struct node *block = get_node(ctx->ast, n->r);
	if (block->type == node_empty) {
		fmt_comments(ctx, pfst, n->r, true);
		fmt_newline_force(ctx, &fst, n->r);
	} else {
		fmt_begin_block(ctx);
		fmt_newline_force(ctx, &fst, n->r);
		fmt_node(ctx, &fst, n->r);
		fmt_end_block(ctx);
		fmt_newline_force(ctx, &fst, n->c);
	}

	if (n->chflg & node_child_c) {
		fmt_if(ctx, &fst, n->c, false);
	}
}

static uint32_t
fmt_chain(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);
	uint32_t len = 0;
	struct node *n = get_node(ctx->ast, n_id);

	switch (n->type) {
	case node_method:
		len += fmt_write(ctx, pfst, '.');
		len += fmt_check(ctx, &fst, fmt_method, n_id);
		break;
	case node_index:
		len += fmt_write(ctx, pfst, '[');
		++ctx->enclosed;
		len += fmt_check(ctx, &fst, fmt_node, n->r);
		--ctx->enclosed;
		len += fmt_write(ctx, pfst, ']');
		break;
	case node_function:
		len += fmt_check(ctx, &fst, fmt_function, n_id);
		break;
	default:
		UNREACHABLE_RETURN;
	}

	if (n->chflg & node_child_d) {
		len += fmt_chain(ctx, pfst, n->d);
	} else {
		len += fmt_tail(ctx, pfst, n_id);
	}
	return len;
}

static uint32_t
fmt_and_or(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);
	fst.fmt_and_or_force_ml = pfst->fmt_and_or_force_ml;

	struct node *n = get_node(ctx->ast, n_id);
	uint32_t len = 0;

	len += fmt_check(ctx, &fst, fmt_node, n->l);

	if ((ctx->enclosed && fst.ml) || fst.fmt_and_or_force_ml) {
		fmt_newline_force(ctx, &fst, 0);
		fst.fmt_and_or_force_ml = true;
	} else {
		len += fmt_writes(ctx, &fst, " ");
	}

	len += fmt_writes(ctx, &fst, n->type == node_or ? "or " : "and ");

	len += fmt_check(ctx, &fst, fmt_node, n->r);
	return len;
}

static uint32_t
fmt_node(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);

	uint32_t len = 0;
	struct node *n = get_node(ctx->ast, n_id);

	if (ctx->trailing_comment && n->type != node_empty_line) {
		fmt_newline_force(ctx, pfst, n_id);
	}

	/* if (pfst->write) { */
	/* 	L("formatting %s", node_to_s(n)); */
	/* } else { */
	/* 	L("checking %s", node_to_s(n)); */
	/* } */

	switch (n->type) {
	/* literals */
	case node_bool:
		len += fmt_writes(ctx, &fst, n->subtype ? "true" : "false");
		break;
	case node_string:
		len += fmt_writeml(ctx, &fst, get_cstr(ctx->wk, n->data.str));
		break;
	case node_array:
	case node_dict:
		fst.arg_container = n->type == node_array ? "[]" : "{}";
		fst.parent = n_id;
		n_id = n->l;
		n = get_node(ctx->ast, n_id);
		len += fmt_check(ctx, &fst, fmt_arg_container, n_id);
		break;
	case node_id:
		len += fmt_writes(ctx, &fst, get_cstr(ctx->wk, n->data.str));
		break;
	case node_number:
		len += fmt_writes(ctx, &fst, get_cstr(ctx->wk, n->data.str));
		break;

	/* control flow */
	case node_block: {
		len = fmt_check(ctx, &fst, fmt_node, n->l);

		if (n->chflg & node_child_r) {
			struct node *bnext = get_node(ctx->ast, n->r);

			if (bnext->type != node_empty) {
				fmt_newline_force(ctx, &fst, n->r);
				fmt_node(ctx, &fst, n->r);
			}
		}
		break;
	}
	case node_if: {
		fmt_if(ctx, &fst, n_id, true);
		fmt_writes(ctx, &fst, "endif");
		break;
	}
	case node_foreach_args:
		len += fmt_node(ctx, &fst, n->l);
		if (n->chflg & node_child_r) {
			len += fmt_writes(ctx, &fst, ", ");
			len += fmt_node(ctx, &fst, n->r);
		}
		break;
	case node_foreach:
		fmt_writes(ctx, &fst, "foreach ");
		fmt_node(ctx, &fst, n->l);
		fmt_writes(ctx, &fst, " : ");
		fmt_check(ctx, &fst, fmt_node_wrapped, n->r);

		struct node *block = get_node(ctx->ast, n->c);
		if (block->type == node_empty) {
			fmt_comments(ctx, pfst, n->c, true);
			fmt_newline_force(ctx, &fst, n->r);
		} else {
			fmt_begin_block(ctx);
			fmt_newline_force(ctx, &fst, n->c);
			fmt_node(ctx, &fst, n->c);
			fmt_end_block(ctx);
			fmt_newline_force(ctx, &fst, 0);
		}

		fmt_writes(ctx, &fst, "endforeach");
		break;
	case node_continue:
		len += fmt_writes(ctx, &fst, "continue");
		break;
	case node_break:
		len += fmt_writes(ctx, &fst, "break");
		break;
	case node_return:
		len += fmt_writes(ctx, &fst, "return ");
		len += fmt_node(ctx, &fst, n->l);
		break;

	/* functions */
	case node_function:
	case node_method:
	case node_index:
		len += fmt_node(ctx, &fst, n->l);
		return len + fmt_chain(ctx, pfst, n_id);
	case node_func_def: {
		fmt_writes(ctx, &fst, "func ");
		len += fmt_node(ctx, &fst, n->l);

		struct fmt_stack arg_fst = *fmt_setup_fst(&fst);
		arg_fst.arg_container = "()";
		arg_fst.ml = false;
		len += fmt_check(ctx, &arg_fst, fmt_arg_container, n->r);

		if (n->data.type) {
			len += fmt_writes(ctx, &fst, " -> ");
			len += fmt_writes(ctx, &fst, typechecking_type_to_s(ctx->wk, n->data.type));
		}

		struct node *block = get_node(ctx->ast, n->c);
		if (block->type == node_empty) {
			fmt_comments(ctx, pfst, n->c, true);
			fmt_newline_force(ctx, &fst, n->r);
		} else {
			fmt_begin_block(ctx);
			fmt_newline_force(ctx, &fst, n->c);
			fmt_node(ctx, &fst, n->c);
			fmt_end_block(ctx);
			fmt_newline_force(ctx, &fst, 0);
		}

		fmt_writes(ctx, &fst, "endfunc");
		break;
	}

	/* assignment */
	case node_assignment:
		len += fmt_node(ctx, &fst, n->l);
		len += fmt_check_trailing_comment_or_space(ctx, &fst, n->l);
		len += fmt_writes(ctx, &fst, "= ");
		len += fmt_node_wrapped(ctx, &fst, n->r);
		break;
	case node_plusassign:
		len += fmt_node(ctx, &fst, n->l);
		len += fmt_check_trailing_comment_or_space(ctx, &fst, n->l);
		len += fmt_writes(ctx, &fst, "+= ");
		len += fmt_node_wrapped(ctx, &fst, n->r);
		break;

	/* comparison stuff */
	case node_not:
		len += fmt_writes(ctx, &fst, "not ");
		len += fmt_node(ctx, &fst, n->l);
		break;
	case node_and:
	case node_or:
		len += fmt_and_or(ctx, &fst, n_id);
		break;
	case node_comparison: {
		assert(n->subtype <= comp_not_in);

		const char *kw = (char *[]){
			[comp_equal] = "==",
			[comp_nequal] = "!=",
			[comp_lt] = "<",
			[comp_le] = "<=",
			[comp_gt] = ">",
			[comp_ge] = ">=",
			[comp_in] = "in",
			[comp_not_in] = "not in",
		}[n->subtype];

		len += fmt_node(ctx, &fst, n->l);
		len += fmt_check_trailing_comment_or_space(ctx, &fst, n->l);
		len += fmt_writef(ctx, &fst, "%s ", kw);
		len += fmt_node(ctx, &fst, n->r);
		break;
	}
	case node_ternary:
		len += fmt_node(ctx, &fst, n->l);
		len += fmt_check_trailing_comment_or_space(ctx, &fst, n->l);
		len += fmt_writef(ctx, &fst, "? ");
		len += fmt_node(ctx, &fst, n->r);
		len += fmt_check_trailing_comment_or_space(ctx, &fst, n->l);
		len += fmt_writef(ctx, &fst, ": ");
		len += fmt_node(ctx, &fst, n->c);
		break;

	/* math */
	case node_u_minus:
		len += fmt_write(ctx, &fst, '-');
		len += fmt_node(ctx, &fst, n->l);
		break;
	case node_arithmetic:
		len += fmt_node(ctx, &fst, n->l);
		len += fmt_check_trailing_comment_or_space(ctx, &fst, n->l);
		len += fmt_write(ctx, &fst, "+-%*/"[n->subtype]);
		len += fmt_breaking_space(ctx, &fst, n->r);
		len += fmt_node(ctx, &fst, n->r);
		break;

	/* formatting */
	case node_paren:
		len += fmt_check(ctx, &fst, fmt_parens, n_id);
		n_id = n->r;
		break;
	case node_empty_line:
		break;

	/* handled in other places */
	case node_argument:
		assert(false && "unreachable");
		break;

	case node_empty:
		break;

	/* never valid */
	case node_stringify:
	case node_null:
		assert(false);
		break;
	}

	len += fmt_tail(ctx, pfst, n_id);
	return len;
}

/*
 * config parsing
 */

static bool
fmt_cfg_parse_cb(void *_ctx, struct source *src, const char *sect,
	const char *k, const char *v, struct source_location location)
{
	struct fmt_ctx *ctx = _ctx;

	enum val_type {
		type_uint,
		type_str,
		type_bool,
	};

	static const struct { const char *name; enum val_type type; uint32_t off; } keys[] = {
		{ "max_line_len", type_uint, offsetof(struct fmt_ctx, max_line_len) },
		{ "indent_by", type_str, offsetof(struct fmt_ctx, indent_by) },
		{ "space_array", type_bool, offsetof(struct fmt_ctx, space_array) },
		{ "kwargs_force_multiline", type_bool, offsetof(struct fmt_ctx, kwa_ml) },
		{ "kwa_ml", type_bool, offsetof(struct fmt_ctx, kwa_ml) }, // kept for backwards compat
		{ "wide_colon", type_bool, offsetof(struct fmt_ctx, wide_colon) },
		{ "no_single_comma_function", type_bool, offsetof(struct fmt_ctx, no_single_comma_function) },
		0
	};

	if (!k || !*k) {
		error_messagef(src, location, log_error, "missing key");
		return false;
	} else if (!v || !*v) {
		error_messagef(src, location, log_error, "missing value");
		return false;
	} else if (sect) {
		error_messagef(src, location, log_error, "invalid section");
		return false;
	}

	uint32_t i;
	for (i = 0; keys[i].name; ++i) {
		if (strcmp(k, keys[i].name) == 0) {
			void *val_dest = ((uint8_t *)ctx + keys[i].off);

			switch (keys[i].type) {
			case type_uint: {
				char *endptr = NULL;
				long long lval = strtoll(v, &endptr, 10);
				if (*endptr) {
					error_messagef(src, location, log_error, "unable to parse integer");
					return false;
				} else if (lval < 0 || lval > (long long)UINT32_MAX) {
					error_messagef(src, location, log_error, "integer outside of range 0-%u", UINT32_MAX);
					return false;
				}

				uint32_t val = lval;
				memcpy(val_dest, &val, sizeof(uint32_t));
				break;
			}
			case type_str: {
				char *start, *end;
				start = strchr(v, '\'');
				end = strrchr(v, '\'');

				if (!start || !end || start == end) {
					error_messagef(src, location, log_error, "expected single-quoted string");
					return false;
				}

				*end = 0;
				++start;

				memcpy(val_dest, &start, sizeof(char *));
				break;
			}
			case type_bool: {
				bool val;
				if (strcmp(v, "true") == 0) {
					val = true;
				} else if (strcmp(v, "false") == 0) {
					val = false;
				} else {
					error_messagef(src, location, log_error, "invalid value for bool, expected true/false");
					return false;
				}

				memcpy(val_dest, &val, sizeof(bool));
				break;
			}
			}
		}
	}

	return true;
}

bool
fmt(struct source *src, FILE *out, const char *cfg_path, bool check_only, bool editorconfig)
{
	bool ret = false;
	struct ast ast = { 0 };
	struct sbuf out_buf;
	struct workspace wk = { 0 };
	workspace_init_bare(&wk);
	struct fmt_ctx ctx = {
		.ast = &ast,
		.wk = &wk,
		.out_buf = &out_buf,
		.max_line_len = 80,
		.indent_by = "    ",
		.space_array = false,
		.kwa_ml = false,
		.wide_colon = false,
		.no_single_comma_function = false,
	};
	struct fmt_stack fst = {
		.write = true,
	};

	if (check_only) {
		sbuf_init(&out_buf, NULL, 0, sbuf_flag_overflow_alloc);
	} else {
		out_buf.flags = sbuf_flag_write;
		out_buf.buf = (void *)out;
	}

	if (editorconfig) {
		struct editorconfig_opts editorconfig_opts;
		try_parse_editorconfig(src, &editorconfig_opts);
		if (editorconfig_opts.indent_by) {
			ctx.indent_by = editorconfig_opts.indent_by;
		}
	}

	char *cfg_buf = NULL;
	struct source cfg_src = { 0 };
	if (cfg_path) {
		if (!fs_read_entire_file(cfg_path, &cfg_src)) {
			goto ret;
		} else if (!ini_parse(cfg_path, &cfg_src, &cfg_buf, fmt_cfg_parse_cb, &ctx)) {
			goto ret;
		}
	}

	enum parse_mode parse_mode = pm_keep_formatting | pm_ignore_statement_with_no_effect;
	if (str_endswith(&WKSTR(src->label), &WKSTR(".meson"))) {
		parse_mode |= pm_functions;
	}

	if (!parser_parse(&wk, &ast, src, parse_mode)) {
		goto ret;
	}

	fmt_node(&ctx, &fst, ast.root);
	assert(!ctx.indent);
	fmt_newline_force(&ctx, &fst, 0);

	if (check_only) {
		if (src->len != out_buf.len) {
			goto ret;
		} else if (memcmp(src->src, out_buf.buf, src->len)) {
			goto ret;
		}
	}

	ret = true;
ret:
	workspace_destroy_bare(&wk);
	if (cfg_buf) {
		z_free(cfg_buf);
	}
	sbuf_destroy(&out_buf);
	fs_source_destroy(&cfg_src);
	ast_destroy(&ast);
	return ret;
}

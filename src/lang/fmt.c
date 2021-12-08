#include "posix.h"

#include <inttypes.h>

#include "buf_size.h"
#include "lang/fmt.h"
#include "log.h"

struct fmt_ctx {
	struct ast *ast;
	FILE *out;
	uint32_t indent, col, enclosed;
	bool force_ml;

	uint32_t max_line_len;
	const char *indent_by;
};

struct fmt_stack {
	uint32_t parent;
	const char *node_sep;
	const char *arg_container;
	bool write;
	bool ml;
};

typedef uint32_t ((*fmt_func)(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id));
static uint32_t fmt_node(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id);
static uint32_t fmt_chain(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id);

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
		fputc(c, ctx->out);
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
				fputc('\n', ctx->out);
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
	if (pfst->write && pfst->ml) {
		fputc('\n', ctx->out);
		ctx->col = 0;

		if (next != 0) {
			struct node *bnext = get_node(ctx->ast, next);
			if (bnext->type != node_empty) {
				struct node *next = get_node(ctx->ast, bnext->l);
				if (next->type == node_empty_line && !next->comment) {
					return;
				}
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

__attribute__ ((format(printf, 3, 4)))
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
fmt_tail(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	uint32_t len = 0;
	struct node *n = get_node(ctx->ast, n_id);

	if (pfst->node_sep) {
		len += fmt_writes(ctx, pfst, pfst->node_sep);
	}

	if (n->comment) {
		len += fmt_writef(ctx, pfst, "%s#%s",
			n->type == node_empty_line ? "" : " ",
			n->comment);
		ctx->force_ml = true;
	}

	return len;
}

static uint32_t
fmt_args(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_args)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);
	fst.node_sep = pfst->node_sep;

	uint32_t len = 0;
	uint32_t arg_val;
	struct node *arg = get_node(ctx->ast, n_args), *narg = NULL;
	bool last = false;

	if (arg->type == node_empty) {
		return 0;
	}

	++ctx->enclosed;
	fmt_begin_block(ctx);
	fmt_newline(ctx, &fst, n_args);

	while (arg->type != node_empty) {
		last = !((arg->chflg & node_child_c)
			 && (narg = get_node(ctx->ast, arg->c))->type != node_empty);

		if (arg->subtype == arg_kwarg) {
			fst.node_sep = ": ";
			len += fmt_node(ctx, &fst, arg->l);
			arg_val = arg->r;
		} else {
			arg_val = arg->l;
		}

		if (last && !fst.ml) {
			fst.node_sep = NULL;
		} else {
			fst.node_sep = ",";
		}
		len += fmt_check(ctx, &fst, fmt_node, arg_val);

		if (!last) {
			fmt_newline_or_space(ctx, &fst, arg->c);
			arg = narg;
		} else {
			break;
		}
	}

	--ctx->enclosed;
	fmt_end_block(ctx);
	fmt_newline(ctx, &fst, 0);
	return len;
}

static uint32_t
fmt_arg_container(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);
	struct node *p = get_node(ctx->ast, fst.parent);

	uint32_t len = 0;
	len += fmt_write(ctx, &fst, fst.arg_container[0]);
	if (p->comment) {
		len += fmt_writef(ctx, pfst, " #%s", p->comment);
		ctx->force_ml = true;
	}

	len += fmt_args(ctx, &fst, n_id);
	len += fmt_write(ctx, &fst, fst.arg_container[1]);
	return len;
}

static uint32_t
fmt_function_common(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_name, uint32_t n_args)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);

	uint32_t len = 0;
	const char *name = get_node(ctx->ast, n_name)->dat.s;

	len += fmt_writes(ctx, &fst, name);

	fst.arg_container = "()";
	fst.parent = n_name;
	len += fmt_arg_container(ctx, &fst, n_args);
	return len;
}

static uint32_t
fmt_function(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);

	uint32_t len = 0;
	struct node *f = get_node(ctx->ast, n_id);

	len += fmt_function_common(ctx, &fst, f->l, f->r);

	if (f->chflg & node_child_d) {
		len += fmt_chain(ctx, &fst, f->d);
	}

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

	len += fmt_node(ctx, &fst, n->l);

	if (fst.ml) {
		fmt_end_block(ctx);
		fmt_newline(ctx, &fst, 0);
	}

	--ctx->enclosed;
	len += fmt_write(ctx, &fst, ')');
	n = get_node(ctx->ast, n->r);

	return len;
}

static void
fmt_if(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id, bool first)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);

	struct node *n = get_node(ctx->ast, n_id);

	if (n->subtype == if_normal) {
		fmt_writef(ctx, &fst, "%s ", first ? "if" : "elif");
		fmt_check(ctx, &fst, fmt_node_wrapped, n->l);
	} else {
		fmt_writes(ctx, &fst, "else");
	}

	fmt_begin_block(ctx);
	fmt_newline_force(ctx, &fst, n->r);
	fmt_node(ctx, &fst, n->r);
	fmt_end_block(ctx);
	fmt_newline_force(ctx, &fst, n->c);
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
		len += fmt_write(ctx, &fst, '.');
		len += fmt_function_common(ctx, &fst, n->r, n->c);
		break;
	case node_index:
		len += fmt_write(ctx, &fst, '[');
		++ctx->enclosed;
		len += fmt_node(ctx, &fst, n->r);
		--ctx->enclosed;
		len += fmt_write(ctx, &fst, ']');
		break;
	default:
		assert(false && "unreachable");
		break;
	}

	if (n->chflg & node_child_d) {
		len += fmt_tail(ctx, &fst, n_id);
		len += fmt_chain(ctx, &fst, n->d);
	}
	return len;
}

static uint32_t
fmt_node(struct fmt_ctx *ctx, const struct fmt_stack *pfst, uint32_t n_id)
{
	struct fmt_stack fst = *fmt_setup_fst(pfst);

	uint32_t len = 0;
	struct node *n = get_node(ctx->ast, n_id);

	switch (n->type) {
	/* literals */
	case node_bool:
		len += fmt_writes(ctx, &fst, n->subtype ? "true" : "false");
		break;
	case node_string:
		len += fmt_writeml(ctx, &fst, n->dat.s);
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
		len += fmt_writes(ctx, &fst, n->dat.s);
		break;
	case node_number:
		len += fmt_writes(ctx, &fst, n->dat.s);
		break;

	/* control flow */
	case node_block: {
		fmt_check(ctx, &fst, fmt_node, n->l);

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

		fmt_begin_block(ctx);
		fmt_newline_force(ctx, &fst, n->c);

		fmt_node(ctx, &fst, n->c);

		fmt_end_block(ctx);
		fmt_newline_force(ctx, &fst, 0);

		fmt_writes(ctx, &fst, "endforeach");
		break;
	case node_continue:
		len += fmt_writes(ctx, &fst, "continue");
		break;
	case node_break:
		len += fmt_writes(ctx, &fst, "break");
		break;

	/* functions */
	case node_function:
		len += fmt_check(ctx, &fst, fmt_function, n_id);
		break;

	case node_method:
		len += fmt_node(ctx, &fst, n->l);
		len += fmt_chain(ctx, &fst, n_id);
		break;
	case node_index:
		assert(n->chflg & node_child_l);
		len += fmt_node(ctx, &fst, n->l);
		len += fmt_chain(ctx, &fst, n_id);
		break;

	/* assignment */
	case node_assignment:
		len += fmt_node(ctx, &fst, n->l);
		len += fmt_writes(ctx, &fst, " = ");
		len += fmt_node_wrapped(ctx, &fst, n->r);
		break;
	case node_plusassign:
		len += fmt_node(ctx, &fst, n->l);
		len += fmt_writes(ctx, &fst, " += ");
		len += fmt_node_wrapped(ctx, &fst, n->r);
		break;

	/* comparison stuff */
	case node_not:
		len += fmt_writes(ctx, &fst, "not ");
		len += fmt_node(ctx, &fst, n->l);
		break;
	case node_and:
		len += fmt_node(ctx, &fst, n->l);
		len += fmt_writes(ctx, &fst, " and ");
		len += fmt_node(ctx, &fst, n->r);
		break;
	case node_or:
		len += fmt_node(ctx, &fst, n->l);
		len += fmt_writes(ctx, &fst, " or ");
		len += fmt_node(ctx, &fst, n->r);
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
		len += fmt_writef(ctx, &fst, " %s ", kw);
		len += fmt_node(ctx, &fst, n->r);
		break;
	}
	case node_ternary:
		len += fmt_node(ctx, &fst, n->l);
		len += fmt_writef(ctx, &fst, " ? ");
		len += fmt_node(ctx, &fst, n->r);
		len += fmt_writef(ctx, &fst, " : ");
		len += fmt_node(ctx, &fst, n->c);
		break;

	/* math */
	case node_u_minus:
		len += fmt_write(ctx, &fst, '-');
		len += fmt_node(ctx, &fst, n->l);
		break;
	case node_arithmetic:
		len += fmt_node(ctx, &fst, n->l);
		len += fmt_write(ctx, &fst, ' ');
		len += fmt_write(ctx, &fst, "+-%*/"[n->subtype]);
		len += fmt_breaking_space(ctx, &fst, n->r);
		len += fmt_node(ctx, &fst, n->r);
		break;

	/* fmtting */
	case node_paren:
		len += fmt_check(ctx, &fst, fmt_parens, n_id);
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

bool
fmt(struct ast *ast, FILE *out)
{
	struct fmt_ctx ctx = {
		.ast = ast,
		.out = out,
		.max_line_len = 80,
		.indent_by = "  ",
	};

	struct fmt_stack fst = {
		.write = true,
	};

	fmt_node(&ctx, &fst, ast->root);
	assert(!ctx.indent);
	fmt_newline_force(&ctx, &fst, 0);
	return true;
}
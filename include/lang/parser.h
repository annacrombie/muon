/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_LANG_PARSER_H
#define MUON_LANG_PARSER_H

#include <stdint.h>
#include <stdbool.h>

#include "datastructures/arr.h"
#include "lang/lexer.h"

#define NODE_MAX_CHILDREN 4

enum comparison_type {
	comp_equal,
	comp_nequal,
	comp_lt,
	comp_le,
	comp_gt,
	comp_ge,
	comp_in,
	comp_not_in // keep at the end,
};

enum arithmetic_type {
	arith_add = 0,
	arith_sub = 1,
	arith_mod = 2,
	arith_mul = 3,
	arith_div = 4,
};

enum arg_type {
	arg_normal,
	arg_kwarg,
};

enum if_type {
	if_if,
	if_elseif,
	if_else,
};

enum node_type {
	node_null, // only used for parsing
	node_bool,
	node_id,
	node_number,
	node_string,
	node_continue,
	node_break,
	node_argument,
	node_array,
	node_dict,
	node_empty,
	node_or,
	node_and,
	node_comparison,
	node_arithmetic,
	node_not,
	node_index,
	node_method,
	node_function,
	node_assignment,
	node_foreach,
	node_foreach_args,
	node_if,
	node_u_minus,
	node_ternary,
	node_block,

	/* special */
	node_stringify,
	node_func_def,
	node_return,

	/* formatting-only nodes */
	node_empty_line,
	node_paren,
	node_plusassign,
};

enum node_child_flag {
	node_child_l = 1 << 0,
	node_child_r = 1 << 1,
	node_child_c = 1 << 2,
	node_child_d = 1 << 3,
};

enum {
	node_visited = 1 << 4, // for analyzer
};

struct node {
	enum node_type type;
	struct source_location location;
	uint32_t subtype;
	uint32_t l, r, c, d;
	union literal_data data;
	struct { uint32_t start, len; } comments;
	uint8_t chflg;
};

struct ast {
	struct bucket_arr nodes;
	struct arr comments;
	uint32_t root;
	uint32_t src_id; // used for diagnostics in the analyzer
};

enum parse_mode {
	pm_ignore_statement_with_no_effect = 1 << 0,
	pm_keep_formatting = 1 << 1,
	pm_quiet = 1 << 2,
	pm_functions = 1 << 3,
};

void print_ast(struct workspace *wk, struct ast *ast);
void print_ast_at(struct workspace *wk, struct ast *ast, uint32_t id, uint32_t d, char label);
bool parser_parse(struct workspace *wk, struct ast *ast, struct source *src, enum parse_mode mode);
struct node *get_node(struct ast *ast, uint32_t i);
uint32_t *get_node_child(struct node *n, uint32_t c);
const char *node_to_s(struct workspace *wk, const struct node *n);
const char *node_type_to_s(enum node_type t);
void ast_destroy(struct ast *ast);
#endif

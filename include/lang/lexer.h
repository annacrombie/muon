/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_LANG_LEXER_H
#define MUON_LANG_LEXER_H

#include <stdbool.h>
#include <stdint.h>

#include "datastructures/bucket_arr.h"
#include "error.h"
#include "lang/string.h"

enum token_type {
	token_type_error = -1,
	token_type_eof,
	token_type_eol,
	token_type_lparen = '(',
	token_type_rparen = ')',
	token_type_lbrack = '[',
	token_type_rbrack = ']',
	token_type_lcurl = '{',
	token_type_rcurl = '}',
	token_type_dot = '.',
	token_type_comma = ',',
	token_type_colon = ':',
	token_type_question_mark = '?',

	/* math */
	token_type_plus = '+',
	token_type_minus = '-',
	token_type_star = '*',
	token_type_slash = '/',
	token_type_modulo = '%',

	/* comparison single char */
	token_type_gt = '>',
	token_type_lt = '<',

	/* special single char */
	token_type_bitor = '|',

	/* assign */
	token_type_assign = '=',
	token_type_plus_assign = 256,

	/* comparison multi char */
	token_type_eq,
	token_type_neq,
	token_type_geq,
	token_type_leq,

	/* keywords */
	token_type_if,
	token_type_else,
	token_type_elif,
	token_type_endif,
	token_type_and,
	token_type_or,
	token_type_not,
	token_type_foreach,
	token_type_endforeach,
	token_type_in,
	token_type_not_in,
	token_type_continue,
	token_type_break,

	/* literals */
	token_type_identifier,
	token_type_string,
	token_type_fstring,
	token_type_number,
	token_type_true,
	token_type_false,

	/* special */
	token_type_func,
	token_type_endfunc,
	token_type_return,
	token_type_returntype,

	/* formatting only */
	token_type_comment,
	token_type_fmt_eol,
};

union literal_data {
	obj literal;
	obj str;
	int64_t num;
	uint64_t type;
};

struct token {
	enum token_type type;
	union literal_data data;
	struct source_location location;
};

enum lexer_mode {
	lexer_mode_format = 1 << 0,
	lexer_mode_functions = 1 << 1,
};

struct lexer {
	struct workspace *wk;
	struct source *source;
	const char *src;
	uint32_t i, line, line_start, enclosing;
	enum lexer_mode mode;
};

bool is_valid_inside_of_identifier(const char c);
bool is_valid_start_of_identifier(const char c);

void lexer_init(struct lexer *lexer, struct workspace *wk, struct source *src, enum lexer_mode mode);
void lexer_next(struct lexer *lexer, struct token *token);

const char *token_type_to_s(enum token_type type);
const char *token_to_s(struct workspace *wk, struct token *token);
#endif

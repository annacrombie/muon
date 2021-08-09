#include "posix.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "buf_size.h"
#include "error.h"
#include "lang/eval.h"
#include "lang/lexer.h"
#include "log.h"
#include "platform/filesystem.h"
#include "platform/mem.h"

enum lex_result {
	lex_cont,
	lex_done,
	lex_fail,
};

struct lexer {
	struct tokens *toks;
	struct source *source;
	struct source_data *sdata;
	const char *src;
	uint32_t i, data_i, line, line_start;
	struct { uint32_t paren, bracket, curl; } enclosing;
};

const char *
tok_type_to_s(enum token_type type)
{
	switch (type) {
	case tok_eof: return "end of file";
	case tok_eol: return "end of line";
	case tok_lparen: return "(";
	case tok_rparen: return ")";
	case tok_lbrack: return "[";
	case tok_rbrack: return "]";
	case tok_lcurl: return "{";
	case tok_rcurl: return "}";
	case tok_dot: return ".";
	case tok_comma: return ",";
	case tok_colon: return ":";
	case tok_assign: return "=";
	case tok_plus: return "+";
	case tok_minus: return "-";
	case tok_star: return "*";
	case tok_slash: return "/";
	case tok_modulo: return "%";
	case tok_plus_assign: return "+=";
	case tok_eq: return "==";
	case tok_neq: return "!=";
	case tok_gt: return ">";
	case tok_geq: return ">=";
	case tok_lt: return "<";
	case tok_leq: return "<=";
	case tok_true: return "true";
	case tok_false: return "false";
	case tok_if: return "if";
	case tok_else: return "else";
	case tok_elif: return "elif";
	case tok_endif: return "endif";
	case tok_and: return "and";
	case tok_or: return "or";
	case tok_not: return "not";
	case tok_foreach: return "foreach";
	case tok_endforeach: return "endforeach";
	case tok_in: return "in";
	case tok_continue: return "continue";
	case tok_break: return "break";
	case tok_identifier: return "identifier";
	case tok_string: return "string";
	case tok_number: return "number";
	case tok_question_mark: return "?";
	}

	assert(false && "unreachable");
	return "";
}

__attribute__ ((format(printf, 2, 3)))
static void
lex_error(struct lexer *l, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	struct token *last_tok = darr_get(&l->toks->tok, l->toks->tok.len - 1);
	error_message(l->source, last_tok->line, last_tok->col, fmt, args);
	va_end(args);
}

const char *
tok_to_s(struct token *tok)
{
	static char buf[BUF_SIZE_S + 1];
	uint32_t i;

	i = snprintf(buf, BUF_SIZE_S, "%s", tok_type_to_s(tok->type));
	if (tok->n) {
		i += snprintf(&buf[i], BUF_SIZE_S - i, ":'%s'", tok->dat.s);
	}

	i += snprintf(&buf[i], BUF_SIZE_S - i, " line %d, col: %d", tok->line, tok->col);

	return buf;
}

static void
advance(struct lexer *l)
{
	if (l->src[l->i] == '\n') {
		++l->line;
		l->line_start = l->i + 1;
	}

	++l->i;
}

static struct token *
next_tok(struct lexer *l)
{
	uint32_t idx = darr_push(&l->toks->tok, &(struct token){ 0 });
	return darr_get(&l->toks->tok, idx);
}

static bool
is_valid_start_of_identifier(const char c)
{
	return c == '_' || ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z');
}

static bool
is_digit(const char c)
{
	return '0' <= c && c <= '9';
}

static bool
is_hex_digit(const char c)
{
	return is_digit(c) || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F');
}

static bool
is_valid_inside_of_identifier(const char c)
{
	return is_valid_start_of_identifier(c) || is_digit(c);
}

static bool
is_skipchar(const char c)
{
	return c == ' ' || c == '\t' || c == '#';
}

struct kw_table {
	const char *name;
	uint32_t val;
};

static bool
kw_lookup(const struct kw_table *table, const char *kw, uint32_t len, uint32_t *res)
{
	uint32_t i;
	for (i = 0; table[i].name; ++i) {
		if (strlen(table[i].name) == len && strncmp(kw, table[i].name, len) == 0) {
			*res = table[i].val;
			return true;
		}
	}

	return false;
}

static bool
keyword(struct lexer *lexer, const char *id, uint32_t len, enum token_type *res)
{
	static const struct kw_table keywords[] = {
		{ "and", tok_and },
		{ "break", tok_break },
		{ "continue", tok_continue },
		{ "elif", tok_elif },
		{ "else", tok_else },
		{ "endforeach", tok_endforeach },
		{ "endif", tok_endif },
		{ "false", tok_false },
		{ "foreach", tok_foreach },
		{ "if", tok_if },
		{ "in", tok_in },
		{ "not", tok_not },
		{ "or", tok_or },
		{ "true", tok_true },
		{ 0 },
	};

	if (kw_lookup(keywords, id, len, res)) {
		return true;
	}

	return false;
}

static enum lex_result
number(struct lexer *lexer, struct token *tok)
{
	tok->type = tok_number;

	uint32_t base = 10;

	if (lexer->src[lexer->i] == '0') {
		switch (lexer->src[lexer->i + 1]) {
		case 'x':
			base = 16;
			lexer->i += 2;
			break;
		case 'b':
			base = 2;
			lexer->i += 2;
			break;
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			base = 8;
			advance(lexer);
			break;
		default:
			tok->dat.n = 0;
			advance(lexer);
			return lex_cont;
		}
	}

	char *endptr = NULL;

	errno = 0;
	int64_t val = strtol(&lexer->src[lexer->i], &endptr, base);

	assert(endptr);
	if (endptr == &lexer->src[lexer->i]) {
		lex_error(lexer, "invalid number");
		return lex_fail;
	}

	if (errno == ERANGE) {
		if (val == LONG_MIN) {
			lex_error(lexer, "underflow when parsing number");
		} else if (val == LONG_MAX) {
			lex_error(lexer, "overflow when parsing number");
		}

		return lex_fail;
	}

	lexer->i += endptr - &lexer->src[lexer->i];

	tok->dat.n = val;

	return lex_cont;
}

static enum lex_result
identifier(struct lexer *lexer, struct token *token)
{
	const char *start = &lexer->src[lexer->i];
	uint32_t len = 0;

	while (is_valid_inside_of_identifier(lexer->src[lexer->i])) {
		++len;
		advance(lexer);
	}

	assert(len);

	if (!keyword(lexer, start, len, &token->type)) {
		char *str = &lexer->sdata->data[lexer->data_i];
		token->dat.s = str;
		token->type = tok_identifier;
		token->n = len;

		strncpy(str, start, token->n);
		str[token->n] = 0;
		lexer->data_i += token->n + 1;
	}

	return lex_cont;
}

static bool
write_utf8(struct lexer *l, struct token *tok, char *str, uint32_t val)
{
	uint8_t pre, b, pre_len;
	uint32_t len, i;

	/* From: https://en.wikipedia.org/wiki/UTF-8#Encoding
	 * U+0000  - U+007F   0x00 0xxxxxxx
	 * U+0080  - U+07FF   0xc0 110xxxxx 10xxxxxx
	 * U+0800  - U+FFFF   0xe0 1110xxxx 10xxxxxx 10xxxxxx
	 * U+10000 - U+10FFFF 0xf0 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
	 * */

	if (val <= 0x7f) {
		str[tok->n] = val;
		++tok->n;
		return true;
	} else if (val <= 0x07ff) {
		len = 2;
		pre_len = 5;
		pre = 0xc0;
		b = 11;
	} else if (val <= 0xffff) {
		len = 3;
		pre_len = 4;
		pre = 0xe0;
		b = 16;
	} else if (val <= 0x10ffff) {
		len = 4;
		pre_len = 3;
		pre = 0xf0;
		b = 21;
	} else {
		lex_error(l, "invalid utf-8 escape 0x%x", val);
		return false;
	}


	str[tok->n] = pre | (val >> (b - pre_len));
	++tok->n;

	for (i = 1; i < len; ++i) {
		str[tok->n] = 0x80 | ((val >> (b - pre_len - (6 * i))) & 0x3f);
		++tok->n;
	}

	return true;
}

static enum lex_result
string(struct lexer *lexer, struct token *token)
{
	token->type = tok_string;

	advance(lexer);

	char *str = &lexer->sdata->data[lexer->data_i];
	token->dat.s = str;

	bool multiline = false, loop = true;

	while (loop) {
		switch (lexer->src[lexer->i]) {
		case '\n':
			if (lexer->src[lexer->i] == '\n') {
				if (multiline) {
				} else {
					lex_error(lexer, "newline in string");
					return lex_fail;
				}
			}
			break;
		case '\\':
			switch (lexer->src[lexer->i + 1]) {
			case '\\':
			case '\'':
				advance(lexer);
				str[token->n] = lexer->src[lexer->i];
				++token->n;
				break;
			case 'a':
				advance(lexer);
				str[token->n] = '\a';
				++token->n;
				break;
			case 'b':
				advance(lexer);
				str[token->n] = '\b';
				++token->n;
				break;
			case 'f':
				advance(lexer);
				str[token->n] = '\f';
				++token->n;
				break;
			case 'r':
				advance(lexer);
				str[token->n] = '\r';
				++token->n;
				break;
			case 't':
				advance(lexer);
				str[token->n] = '\t';
				++token->n;
				break;
			case 'v':
				advance(lexer);
				str[token->n] = '\v';
				++token->n;
				break;
			case 'n':
				advance(lexer);
				str[token->n] = '\n';
				++token->n;
				break;
			case 'x':
			case 'u':
			case 'U': {
				uint32_t len = 0;
				switch (lexer->src[lexer->i + 1]) {
				case 'x':
					len = 2;
					break;
				case 'u':
					len = 4;
					break;
				case 'U':
					len = 8;
					break;
				}
				advance(lexer);

				char num[9] = { 0 };
				uint32_t i;

				for (i = 0; i < len; ++i) {
					num[i] = lexer->src[lexer->i + 1];
					if (!is_hex_digit(num[i])) {
						lex_error(lexer, "unterminated hex escape");
						return lex_fail;
					}
					advance(lexer);
				}

				uint32_t val = strtol(num, NULL, 16);

				if (!write_utf8(lexer, token, str, val)) {
					return lex_fail;
				}
				break;
			}
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9': {
				char num[4] = { 0 };
				uint32_t i;

				for (i = 0; i < 3; ++i) {
					num[i] = lexer->src[lexer->i + 1];
					if (!is_digit(num[i])) {
						lex_error(lexer, "unterminated octal escape");
						return lex_fail;
					}
					advance(lexer);
				}

				str[token->n] = strtol(num, NULL, 8);
				++token->n;
				break;
			}
			default:
				str[token->n] = lexer->src[lexer->i];
				++token->n;
				advance(lexer);
				str[token->n] = lexer->src[lexer->i];
				++token->n;
				break;
			case 0:
				lex_error(lexer, "unterminated escape");
				return lex_fail;
			}
			break;
		case 0:
			lex_error(lexer, "unmatched single quote (\')");
			return lex_fail;
		case '\'':
			str[token->n] = 0;
			loop = false;
			break;
		default:
			str[token->n] = lexer->src[lexer->i];
			++token->n;
			break;
		}

		advance(lexer);
	}

	lexer->data_i += token->n + 1;
	return lex_cont;
}

static enum lex_result
lexer_tokenize_one(struct lexer *lexer)
{
	struct token *token = next_tok(lexer);

	while (is_skipchar(lexer->src[lexer->i])) {
		if (lexer->src[lexer->i] == '#') {
			while (lexer->src[lexer->i] && lexer->src[lexer->i] != '\n') {
				advance(lexer);
			}
		} else {
			advance(lexer);
		}
	}

	*token = (struct token) {
		.line = lexer->line,
		.col = lexer->i - lexer->line_start + 1,
	};

	if (is_valid_start_of_identifier(lexer->src[lexer->i])) {
		return identifier(lexer, token);
	} else if (is_digit(lexer->src[lexer->i])) {
		return number(lexer, token);
	} else if (lexer->src[lexer->i] == '\'') {
		return string(lexer, token);
	} else {
		switch (lexer->src[lexer->i]) {
		case '\n':
			if (lexer->enclosing.paren || lexer->enclosing.bracket
			    || lexer->enclosing.curl) {
				goto skip;
			}

			token->type = tok_eol;
			break;
		case '(':
			++lexer->enclosing.paren;
			token->type = tok_lparen;
			break;
		case ')':
			if (!lexer->enclosing.paren) {
				lex_error(lexer, "closing ')' without a matching opening '('");
				return lex_fail;
			}
			--lexer->enclosing.paren;

			token->type = tok_rparen;
			break;
		case '[':
			++lexer->enclosing.bracket;
			token->type = tok_lbrack;
			break;
		case ']':
			if (!lexer->enclosing.bracket) {
				lex_error(lexer, "closing ']' without a matching opening '['");
				return lex_fail;
			}
			--lexer->enclosing.bracket;

			token->type = tok_rbrack;
			break;
		case '{':
			++lexer->enclosing.curl;
			token->type = tok_lcurl;
			break;
		case '}':
			if (!lexer->enclosing.curl) {
				lex_error(lexer, "closing '}' without a matching opening '{'");
				return lex_fail;
			}
			--lexer->enclosing.curl;

			token->type = tok_rcurl;
			break;
		case '.':
			token->type = tok_dot;
			break;
		case ',':
			token->type = tok_comma;
			break;
		case ':':
			token->type = tok_colon;
			break;
		case '?':
			token->type = tok_question_mark;
			break;
		// arithmetic
		case '+':
			if (lexer->src[lexer->i + 1] == '=') {
				advance(lexer);
				token->type = tok_plus_assign;
			} else {
				token->type = tok_plus;
			}
			break;
		case '-':
			token->type = tok_minus;
			break;
		case '*':
			token->type = tok_star;
			break;
		case '/':
			token->type = tok_slash;
			break;
		case '%':
			token->type = tok_modulo;
			break;
		case '=':
			if (lexer->src[lexer->i + 1] == '=') {
				advance(lexer);
				token->type = tok_eq;
			} else {
				token->type = tok_assign;
			}
			break;
		case '!':
			if (lexer->src[lexer->i + 1] == '=') {
				advance(lexer);
				token->type = tok_neq;
			} else {
				lex_error(lexer, "unexpected character: '%c'", lexer->src[lexer->i]);
				return lex_fail;
			}
			break;
		case '>':
			if (lexer->src[lexer->i + 1] == '=') {
				advance(lexer);
				token->type = tok_geq;
			} else {
				token->type = tok_gt;
			}
			break;
		case '<':
			if (lexer->src[lexer->i + 1] == '=') {
				advance(lexer);
				token->type = tok_leq;
			} else {
				token->type = tok_lt;
			}
			break;
		case '\0':
			token->type = tok_eof;
			return lex_done;
		default:
			lex_error(lexer, "unexpected character: '%c'", lexer->src[lexer->i]);
			return lex_fail;
		}

		advance(lexer);
		return lex_cont;
	}

	assert(false && "unreachable");
	return lex_fail;
skip:
	advance(lexer);
	--lexer->toks->tok.len;
	return lex_cont;
}


static bool
tokenize(struct lexer *lexer)
{

	while (true) {
		switch (lexer_tokenize_one(lexer)) {
		case lex_cont:
			break;
		case lex_fail:
			return false;
		case lex_done:
			goto done;
		}
	}

done:

#if 0
	uint32_t i;
	struct token *tok;
	for (i = 0; i < lexer->toks->tok.len; ++i) {
		tok = darr_get(&lexer->toks->tok, i);

		L("%s", tok_to_s(tok));
	}
#endif

	return true;
}

bool
lexer_lex(struct tokens *toks, struct source_data *sdata, struct source *src)
{
	*toks = (struct tokens) { 0 };

	struct lexer lexer = {
		.source = src,
		.line = 1,
		.src = src->src,
		.toks = toks,
		.sdata = sdata,
	};

	darr_init(&toks->tok, 2048, sizeof(struct token));

	// TODO: this could be done much more conservatively
	sdata->data_len = src->len + 1;
	sdata->data = z_malloc(sdata->data_len);

	return tokenize(&lexer);
}

void
tokens_destroy(struct tokens *toks)
{
	darr_destroy(&toks->tok);
}
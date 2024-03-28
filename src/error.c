/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <stdarg.h>
#include <stdlib.h> // exit
#include <string.h>

#include "buf_size.h"
#include "datastructures/arr.h"
#include "embedded.h"
#include "error.h"
#include "log.h"
#include "platform/mem.h"

struct error_diagnostic_message {
	struct source_location location;
	enum log_level lvl;
	const char *msg;
	uint32_t src_idx;
};

struct error_diagnostic_source {
	struct source src;
};

static struct {
	struct arr messages;
	struct arr sources;
	bool init;
	struct {
		struct source *src;
		struct source_location location;
		bool redirect;
	} redirect;
} error_diagnostic_store;

	void
error_diagnostic_store_redirect(struct source *src, struct source_location location)
{
	if (error_diagnostic_store.redirect.redirect) {
		return;
	}

	error_diagnostic_store.redirect.redirect = true;
	error_diagnostic_store.redirect.src = src;
	error_diagnostic_store.redirect.location = location;
}

void
error_diagnostic_store_redirect_reset(void)
{
	error_diagnostic_store.redirect.redirect = false;
}

void
error_diagnostic_store_init(void)
{
	arr_init(&error_diagnostic_store.messages, 32, sizeof(struct error_diagnostic_message));
	arr_init(&error_diagnostic_store.sources, 4, sizeof(struct error_diagnostic_source));
	error_diagnostic_store.init = true;
}

uint32_t
error_diagnostic_store_push_src(struct source *src)
{
	if (!src) {
		return 0;
	}

	uint32_t i;
	struct error_diagnostic_source *s = NULL;
	for (i = 0; i < error_diagnostic_store.sources.len; ++i) {
		s = arr_get(&error_diagnostic_store.sources, i);
		if (strcmp(s->src.label, src->label) == 0) {
			break;
		} else {
			s = NULL;
		}
	}

	if (!s) {
		struct source dup;
		fs_source_dup(src, &dup);

		arr_push(&error_diagnostic_store.sources, &(struct error_diagnostic_source) {
			.src = dup,
		});

		i = error_diagnostic_store.sources.len - 1;
	}

	s = arr_get(&error_diagnostic_store.sources, i);
	return i;
}

void
error_diagnostic_store_push(uint32_t src_idx, struct source_location location, enum log_level lvl, const char *msg)
{
	uint32_t mlen = strlen(msg);
	char *m = z_calloc(mlen + 1, 1);
	memcpy(m, msg, mlen);

	arr_push(&error_diagnostic_store.messages,
		&(struct error_diagnostic_message){
		.location = location,
		.lvl = lvl,
		.msg = m,
		.src_idx = src_idx,
	});
}

static int32_t
error_diagnostic_store_compare_except_lvl(const void *_a, const void *_b, void *ctx)
{
	const struct error_diagnostic_message *a = _a, *b = _b;
	int32_t v;

	if (a->src_idx != b->src_idx) {
		return (int32_t)a->src_idx - (int32_t)b->src_idx;
	} else if (a->location.line != b->location.line) {
		return (int32_t)a->location.line - (int32_t)b->location.line;
	} else if (a->location.col != b->location.col) {
		return (int32_t)a->location.col - (int32_t)b->location.col;
	} else if ((v = strcmp(a->msg, b->msg)) != 0) {
		return v;
	} else {
		return 0;
	}
}

static int32_t
error_diagnostic_store_compare(const void *_a, const void *_b, void *ctx)
{
	const struct error_diagnostic_message *a = _a, *b = _b;
	int32_t v;

	if ((v = error_diagnostic_store_compare_except_lvl(a, b, ctx)) != 0) {
		return v;
	} else if (a->lvl != b->lvl) {
		return a->lvl > b->lvl ? 1 : -1;
	} else {
		return 0;
	}
}

void
error_diagnostic_store_replay(enum error_diagnostic_store_replay_opts opts, bool *saw_error)
{
	error_diagnostic_store.init = false;

	uint32_t i;
	struct error_diagnostic_message *msg;
	struct error_diagnostic_source *last_src = NULL, *cur_src;

	arr_sort(&error_diagnostic_store.messages, NULL, error_diagnostic_store_compare);

	size_t tail, initial_len = error_diagnostic_store.messages.len;
	if (error_diagnostic_store.messages.len > 1) {
		struct error_diagnostic_message *prev_msg, tmp;
		tail = error_diagnostic_store.messages.len;

		uint32_t initial_len = error_diagnostic_store.messages.len;
		msg = arr_get(&error_diagnostic_store.messages, 0);
		arr_push(&error_diagnostic_store.messages, msg);
		for (i = 1; i < initial_len; ++i) {
			prev_msg = arr_get(&error_diagnostic_store.messages, i - 1);
			msg = arr_get(&error_diagnostic_store.messages, i);

			if (error_diagnostic_store_compare_except_lvl(prev_msg, msg, NULL) == 0) {
				continue;
			}

			tmp = *msg;
			arr_push(&error_diagnostic_store.messages, &tmp);
		}
	} else {
		tail = 0;
	}

	*saw_error = false;
	struct source src = { 0 };
	for (i = tail; i < error_diagnostic_store.messages.len; ++i) {
		msg = arr_get(&error_diagnostic_store.messages, i);

		if (opts & error_diagnostic_store_replay_werror) {
			msg->lvl = log_error;
		}

		if ((opts & error_diagnostic_store_replay_errors_only)
		    && msg->lvl != log_error) {
			continue;
		}

		if (msg->lvl == log_error) {
			*saw_error = true;
		}

		if ((cur_src = arr_get(&error_diagnostic_store.sources, msg->src_idx)) != last_src) {
			if (opts & error_diagnostic_store_replay_include_sources) {
				if (last_src) {
					log_plain("\n");
				}

				log_plain("%s%s%s\n",
					log_clr() ? "\033[31;1m" : "",
					cur_src->src.label,
					log_clr() ? "\033[0m" : "");
			}

			last_src = cur_src;
			src = cur_src->src;

			if (!(opts & error_diagnostic_store_replay_include_sources)) {
				src.len = 0;
			}
		}

		error_message(&src, msg->location, msg->lvl, msg->msg);
	}

	for (i = 0; i < initial_len; ++i) {
		msg = arr_get(&error_diagnostic_store.messages, i);
		z_free((char *)msg->msg);
	}

	for (i = 0; i < error_diagnostic_store.sources.len; ++i) {
		cur_src = arr_get(&error_diagnostic_store.sources, i);
		fs_source_destroy(&cur_src->src);
	}

	arr_destroy(&error_diagnostic_store.messages);
	arr_destroy(&error_diagnostic_store.sources);
}

void
error_unrecoverable(const char *fmt, ...)
{
	va_list ap;

	if (log_clr()) {
		log_plain("\033[31m");
	}

	log_plain("fatal error");

	if (log_clr()) {
		log_plain("\033[0m");
	}

	log_plain(": ");
	va_start(ap, fmt);
	log_plainv(fmt, ap);
	log_plain("\n");
	va_end(ap);

	exit(1);
}

struct source *
error_get_stored_source(uint32_t src_idx)
{
	return arr_get(&error_diagnostic_store.sources, src_idx);
}

static bool
list_line_internal(struct source *src, uint32_t lno, uint32_t *start_of_line, uint32_t *line_pre_len)
{
	*start_of_line = 0;

	uint64_t i, cl = 1;
	for (i = 0; i < src->len; ++i) {
		if (src->src[i] == '\n') {
			++cl;
			*start_of_line = i + 1;
		}

		if (cl == lno) {
			break;
		}
	}

	if (i == src->len) {
		return false;
	}

	char line_pre[32] = { 0 };
	*line_pre_len = snprintf(line_pre, 31, "%3d | ", lno);

	log_plain("%s", line_pre);
	for (i = *start_of_line; src->src[i] && src->src[i] != '\n'; ++i) {
		if (src->src[i] == '\t') {
			log_plain("        ");
		} else {
			putc(src->src[i], stderr);
		}
	}
	log_plain("\n");
	return true;
}

static void
list_line_col_marker(const struct source *src, uint32_t col, uint32_t sol, uint32_t line_pre_len)
{
	uint32_t i;
	for (i = 0; i < line_pre_len; ++i) {
		log_plain(" ");
	}

	if (sol + col >= src->len) {
		log_plain("^\n");
		return;
	}

	for (i = 0; i < col; ++i) {
		if (src->src[sol + i] == '\t') {
			log_plain("        ");
		} else {
			log_plain(i == col - 1 ? "^" : " ");
		}
	}
	log_plain("\n");
}

static void
reopen_source(struct source *src, bool *destroy_source)
{
	if (!src->len) {
		switch (src->reopen_type) {
		case source_reopen_type_none:
			return;
		case source_reopen_type_embedded:
			src->src = embedded_get(src->label);
			src->len = strlen(src->src);
			break;
		case source_reopen_type_file:
			if (!fs_read_entire_file(src->label, src)) {
				return;
			}
			*destroy_source = true;
			break;
		}
	}
}

void
list_line_range(struct source *src, uint32_t lno, uint32_t list_amt, uint32_t col)
{
	uint32_t lstart = 0, lend, i;

	if (lno > list_amt / 2) {
		lstart = lno - list_amt / 2;
	}
	lend = lstart + list_amt;

	log_plain("-> %s%s%s\n",
		log_clr() ? "\033[32m" : "",
		src->label,
		log_clr() ? "\033[0m" : ""
		);

	bool destroy_source = false;
	reopen_source(src, &destroy_source);

	for (i = lstart; i < lend; ++i) {
		uint32_t line_pre_len, sol;
		if (list_line_internal(src, i, &sol, &line_pre_len)) {
			if (i == lno && col) {
				list_line_col_marker(src, col, sol, line_pre_len);
			}
		}
	}

	if (destroy_source) {
		fs_source_destroy(src);
	}
}

void
error_message(struct source *src, struct source_location location, enum log_level lvl, const char *msg)
{
	if (error_diagnostic_store.init) {
		if (error_diagnostic_store.redirect.redirect) {
			src = error_diagnostic_store.redirect.src;
			location = error_diagnostic_store.redirect.location;
		}

		error_diagnostic_store_push(error_diagnostic_store_push_src(src), location, lvl, msg);
		return;
	}

	log_plain("%s:%d:%d: ", src->label, location.line, location.col);

	if (log_clr()) {
		log_plain("\033[%sm%s\033[0m ", log_level_clr[lvl], log_level_name[lvl]);
	} else {
		log_plain("%s ", log_level_name[lvl]);
	}

	log_plain("%s\n", msg);

	bool destroy_source = false;
	reopen_source(src, &destroy_source);

	uint32_t line_pre_len, sol;
	if (!list_line_internal(src, location.line, &sol, &line_pre_len)) {
		goto ret;
	}

	list_line_col_marker(src, location.col, sol, line_pre_len);

ret:
	if (destroy_source) {
		fs_source_destroy(src);
	}
}

void
error_messagev(struct source *src, struct source_location location, enum log_level lvl, const char *fmt, va_list args)
{
	static char buf[BUF_SIZE_4k];
	vsnprintf(buf, BUF_SIZE_4k, fmt, args);
	error_message(src, location, lvl, buf);
}

void
error_messagef(struct source *src, struct source_location location, enum log_level lvl, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	error_messagev(src, location, lvl, fmt, ap);
	va_end(ap);
}

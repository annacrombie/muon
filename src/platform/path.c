/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#include "buf_size.h"
#include "error.h"
#include "lang/string.h"
#include "log.h"
#include "platform/os.h"
#include "platform/path.h"

static struct {
	char cwd_buf[BUF_SIZE_2k], tmp1_buf[BUF_SIZE_2k], tmp2_buf[BUF_SIZE_2k];
	struct sbuf cwd, tmp1, tmp2;
} path_ctx;

// These functions are defined in platform/<plat>/os.c
//
// They should only be called indirectly through path_chdir and path_cwd though
// so the prototypes were removed from os.h.
bool os_chdir(const char *path);
char *os_getcwd(char *buf, size_t size);

static void
path_getcwd(void)
{
	sbuf_clear(&path_ctx.cwd);
	while (!os_getcwd(path_ctx.cwd.buf, path_ctx.cwd.cap)) {
		if (errno == ERANGE) {
			sbuf_grow(NULL, &path_ctx.cwd, path_ctx.cwd.cap);
		} else {
			error_unrecoverable("getcwd failed: %s", strerror(errno));
		}
	}
}

void
path_init(void)
{
	sbuf_init(&path_ctx.cwd, path_ctx.cwd_buf, ARRAY_LEN(path_ctx.cwd_buf), sbuf_flag_overflow_alloc);
	sbuf_init(&path_ctx.tmp1, path_ctx.tmp1_buf, ARRAY_LEN(path_ctx.tmp1_buf), sbuf_flag_overflow_alloc);
	sbuf_init(&path_ctx.tmp2, path_ctx.tmp2_buf, ARRAY_LEN(path_ctx.tmp2_buf), sbuf_flag_overflow_alloc);
	path_getcwd();
}

void
path_deinit(void)
{
	sbuf_destroy(&path_ctx.cwd);
	sbuf_destroy(&path_ctx.tmp1);
	sbuf_destroy(&path_ctx.tmp2);
}

void
_path_normalize(struct workspace *wk, struct sbuf *buf, bool optimize)
{
	uint32_t parents = 0;
	char *part, *sep;
	uint32_t part_len, i, slen = buf->len, blen = 0, sep_len;
	bool loop = true, skip_part;

	/* assert(slen && "path is empty"); */
	if (!buf->len) {
		return;
	}

	path_to_posix(buf->buf);
	part = buf->buf;

	if (*part == PATH_SEP) {
		++part;
		--slen;
		++blen;
	}

	while (*part && loop) {
		if (!(sep = strchr(part, PATH_SEP))) {
			sep = &part[strlen(part)];
			loop = false;
		}

		sep_len = *sep ? 1 : 0;
		part_len = sep - part;
		skip_part = false;

		if (!part_len || (part_len == 1 && *part == '.')) {
			// eliminate empty elements (a//b -> a/b) and
			// current-dir elements (a/./b -> a/b)
			skip_part = true;
		} else if (optimize && part_len == 2 && part[0] == '.' && part[1] == '.') {
			// convert something like a/../b into b
			if (parents) {
				for (i = (part - 2) - buf->buf; i > 0; --i) {
					if (buf->buf[i] == PATH_SEP) {
						break;
					}
				}

				if (buf->buf[i] == PATH_SEP) {
					++i;
				}

				part = &buf->buf[i];
				skip_part = true;
				blen -= (sep - &buf->buf[i]) - part_len;
				--parents;
			} else {
				part = sep + sep_len;
			}
		} else {
			++parents;
			part = sep + sep_len;
		}

		if (skip_part) {
			memmove(part, sep + sep_len, slen);
		} else {
			blen += part_len + sep_len;
		}

		slen -= part_len + sep_len;
	}

	if (!blen) {
		buf->buf[0] = '.';
		buf->len = 1;
	} else if (blen > 1 && buf->buf[blen - 1] == PATH_SEP) {
		buf->buf[blen - 1] = 0;
		buf->len = blen - 1;
	} else {
		buf->len = blen;
	}
}

void
path_copy(struct workspace *wk, struct sbuf *sb, const char *path)
{
	sbuf_clear(sb);
	sbuf_pushs(wk, sb, path);
	_path_normalize(wk, sb, false);
}

bool
path_chdir(const char *path)
{
	if (!os_chdir(path)) {
		LOG_E("failed chdir(%s): %s", path, strerror(errno));
		return false;
	}

	path_getcwd();
	return true;
}

void
path_copy_cwd(struct workspace *wk, struct sbuf *sb)
{
	path_copy(wk, sb, path_ctx.cwd.buf);
}

const char *
path_cwd(void)
{
	return path_ctx.cwd.buf;
}

void
path_join_absolute(struct workspace *wk, struct sbuf *sb, const char *a, const char *b)
{
	path_copy(wk, sb, a);
	sbuf_push(wk, sb, PATH_SEP);
	sbuf_pushs(wk, sb, b);
	_path_normalize(wk, sb, false);
}

void
path_push(struct workspace *wk, struct sbuf *sb, const char *b)
{
	if (path_is_absolute(b) || !sb->len) {
		path_copy(wk, sb, b);
	} else if (!*b) {
		/* Special-case path_1 / '' to mean "append a / to the current
		 * path".
		 */
		_path_normalize(wk, sb, false);
		sbuf_push(wk, sb, PATH_SEP);
	} else {
		sbuf_push(wk, sb, PATH_SEP);
		sbuf_pushs(wk, sb, b);
		_path_normalize(wk, sb, false);
	}
}

void
path_join(struct workspace *wk, struct sbuf *sb, const char *a, const char *b)
{
	sbuf_clear(sb);
	path_push(wk, sb, a);
	path_push(wk, sb, b);
}

void
path_make_absolute(struct workspace *wk, struct sbuf *buf, const char *path)
{
	if (path_is_absolute(path)) {
		path_copy(wk, buf, path);
	} else {
		path_join(wk, buf, path_ctx.cwd.buf, path);
	}
}

void
path_relative_to(struct workspace *wk, struct sbuf *buf, const char *base_raw, const char *path_raw)
{
	/*
	 * input: base="/path/to/build/"
	 *        path="/path/to/build/tgt/dir/libfoo.a"
	 * output: "tgt/dir/libfoo.a"
	 *
	 * input: base="/path/to/build"
	 *        path="/path/to/build/libfoo.a"
	 * output: "libfoo.a"
	 *
	 * input: base="/path/to/build"
	 *        path="/path/to/src/asd.c"
	 * output: "../src/asd.c"
	 */

	sbuf_clear(buf);

	sbuf_clear(&path_ctx.tmp1);
	sbuf_pushs(wk, &path_ctx.tmp1, base_raw);
	_path_normalize(wk, &path_ctx.tmp1, true);

	sbuf_clear(&path_ctx.tmp2);
	sbuf_pushs(wk, &path_ctx.tmp2, path_raw);
	_path_normalize(wk, &path_ctx.tmp2, true);

	const char *base = path_ctx.tmp1.buf, *path = path_ctx.tmp2.buf;

	if (!path_is_absolute(base)) {
		LOG_E("base path '%s' is not absolute", base);
		assert(false);
	} else if (!path_is_absolute(path)) {
		LOG_E("path '%s' is not absolute", path);
		assert(false);
	}

	uint32_t i = 0, common_end = 0;

	if (strcmp(base, path) == 0) {
		sbuf_push(wk, buf, '.');
		return;
	}

	while (base[i] && path[i] && base[i] == path[i]) {
		if (base[i] == PATH_SEP) {
			common_end = i;
		}

		++i;
	}

	if ((!base[i] && path[i] == PATH_SEP)) {
		common_end = i;
	} else if (!path[i] && base[i] == PATH_SEP) {
		common_end = i;
	}

	assert(i);
	if (i == 1) {
		/* -> base and path match only at root */
		path_copy(wk, buf, path);
		return;
	}

	if (base[common_end] && base[common_end + 1]) {
		bool have_part = true;
		i = common_end + 1;
		do {
			if (have_part) {
				sbuf_pushs(wk, buf, "..");
				sbuf_push(wk, buf, PATH_SEP);
				have_part = false;
			}

			if (base[i] == PATH_SEP) {
				have_part = true;
			}
			++i;
		} while (base[i]);
	}

	if (path[common_end]) {
		sbuf_pushs(wk, buf, &path[common_end + 1]);
	}

	_path_normalize(wk, buf, false);
}

void
path_without_ext(struct workspace *wk, struct sbuf *buf, const char *path)
{
	int32_t i;

	sbuf_clear(buf);

	if (!*path) {
		return;
	}

	bool have_ext = false;

	SBUF_manual(tmp);
	path_copy(NULL, &tmp, path);
	path = tmp.buf;
	for (i = strlen(path) - 1; i >= 0; --i) {
		if (path[i] == '.') {
			have_ext = true;
			break;
		} else if (path[i] == PATH_SEP) {
			break;
		}
	}

	if (have_ext) {
		sbuf_pushn(wk, buf, path, i);
	} else {
		path_copy(wk, buf, path);
	}
	_path_normalize(wk, buf, false);
	sbuf_destroy(&tmp);
}

void
path_basename(struct workspace *wk, struct sbuf *buf, const char *path)
{
	int32_t i;

	sbuf_clear(buf);

	if (!*path) {
		return;
	}

	SBUF_manual(tmp);
	path_copy(NULL, &tmp, path);
	path = tmp.buf;
	for (i = strlen(path) - 1; i >= 0; --i) {
		if (path[i] == PATH_SEP) {
			++i;
			break;
		}
	}

	if (i < 0) {
		i = 0;
	}

	sbuf_pushs(wk, buf, &path[i]);
	_path_normalize(wk, buf, false);
	sbuf_destroy(&tmp);
}

void
path_dirname(struct workspace *wk, struct sbuf *buf, const char *path)
{
	int32_t i;

	sbuf_clear(buf);

	if (!*path) {
		goto return_dot;
	}

	SBUF_manual(tmp);
	path_copy(NULL, &tmp, path);
	path = tmp.buf;
	for (i = strlen(path) - 1; i >= 0; --i) {
		if (path[i] == PATH_SEP) {
			if (i == 0) {
				/* make dirname of '/path' be '/', not '' */
				sbuf_pushn(wk, buf, path, 1);
			} else {
				sbuf_pushn(wk, buf, path, i);
			}

			_path_normalize(wk, buf, false);
			sbuf_destroy(&tmp);
			return;
		}
	}
	sbuf_destroy(&tmp);

return_dot:
	sbuf_pushs(wk, buf, ".");
}

bool
path_is_subpath(const char *base, const char *sub)
{
	if (!*base) {
		return false;
	}

	SBUF_manual(base_tmp);
	SBUF_manual(sub_tmp);
	path_copy(NULL, &base_tmp, base);
	base = base_tmp.buf;
	path_copy(NULL, &sub_tmp, sub);
	sub = sub_tmp.buf;

	uint32_t i = 0;
	while (true) {
		if (!base[i]) {
			assert(i);
			if (sub[i] == PATH_SEP || sub[i - 1] == PATH_SEP) {
				sbuf_destroy(&sub_tmp);
				sbuf_destroy(&base_tmp);
				return true;
			}
		}

		if (base[i] == sub[i]) {
			if (!base[i]) {
				sbuf_destroy(&sub_tmp);
				sbuf_destroy(&base_tmp);
				return true;
			}
		} else {
			sbuf_destroy(&sub_tmp);
			sbuf_destroy(&base_tmp);
			return false;
		}

		assert(base[i] && sub[i]);
		++i;
	}
}

void
path_executable(struct workspace *wk, struct sbuf *buf, const char *path)
{
	if (path_is_basename(path)) {
		sbuf_clear(buf);
		sbuf_push(wk, buf, '.');
		sbuf_push(wk, buf, PATH_SEP);
		sbuf_pushs(wk, buf, path);
	} else {
		path_copy(wk, buf, path);
	}
}

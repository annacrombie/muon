/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: illiliti <illiliti@dimension.sh>
 * SPDX-FileCopyrightText: Eli Schwartz <eschwartz@archlinux.org>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#ifdef __sun
/* for symlinkat() and fchmodat(), as _POSIX_C_SOURCE does not enable them
 * (yet)
 */
	#define __EXTENSIONS__
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
	#include <unistd.h>
#endif

#include "buf_size.h"
#include "lang/string.h"
#include "log.h"
#include "platform/filesystem.h"
#include "platform/mem.h"
#include "platform/os.h" // For S_ISDIR, S_ISREG on windows
#include "platform/path.h"

bool
fs_stat(const char *path, struct stat *sb)
{
	if (stat(path, sb) != 0) {
		LOG_E("failed stat(%s): %s", path, strerror(errno));
		return false;
	}

	return true;
}

bool
fs_mkdir_p(const char *path)
{
	bool res = false;
	uint32_t i, len = strlen(path);
	SBUF_manual(buf);
	path_copy(NULL, &buf, path);

	assert(len >= 1);

	for (i = 1; i < len; ++i) {
		if (buf.buf[i] == PATH_SEP) {
			buf.buf[i] = 0;
			if (!fs_dir_exists(buf.buf)) {
				if (!fs_mkdir(buf.buf)) {
					goto ret;
				}
			}
			buf.buf[i] = PATH_SEP;
		}
	}

	if (!fs_dir_exists(path)) {
		if (!fs_mkdir(path)) {
			goto ret;
		}
	}

	res = true;
ret:
	sbuf_destroy(&buf);
	return res;
}

FILE *
fs_fopen(const char *path, const char *mode)
{
	FILE *f;
	if (!(f = fopen(path, mode))) {
		LOG_E("failed to open '%s': %s", path, strerror(errno));
		return NULL;
	}

	return f;
}

bool
fs_fclose(FILE *file)
{
	if (fclose(file) != 0) {
		LOG_E("failed fclose: %s", strerror(errno));
		return false;
	}

	return true;
}

bool
fs_fseek(FILE *file, size_t off)
{
	if (fseek(file, off, 0) == -1) {
		LOG_E("failed fseek: %s", strerror(errno));
		return false;
	}
	return true;
}

bool
fs_ftell(FILE *file, uint64_t *res)
{
	int64_t pos;
	if ((pos = ftell(file)) == -1) {
		LOG_E("failed ftell: %s", strerror(errno));
		return false;
	}

	assert(pos >= 0);
	*res = pos;
	return true;
}

bool
fs_fsize(FILE *file, uint64_t *ret)
{
	if (fseek(file, 0, SEEK_END) == -1) {
		LOG_E("failed fseek: %s", strerror(errno));
		return false;
	} else if (!fs_ftell(file, ret)) {
		return false;
	}

	rewind(file);
	return true;
}

static bool
fs_is_seekable(FILE *file, bool *res)
{
	int fd;
	if (!fs_fileno(file, &fd)) {
		return false;
	}

	errno = 0;
	if (lseek(fd, 0, SEEK_CUR) == -1) {
		if (errno == ESPIPE) {
			*res = false;
			return true;
		}
		LOG_E("lseek returned an unexpected error");
		return false;
	}

	*res = true;
	return true;
}

bool
fs_read_entire_file(const char *path, struct source *src)
{
	FILE *f;
	bool opened = false;
	size_t read;
	char *buf = NULL;

	*src = (struct source) { .label = path };

	if (strcmp(path, "-") == 0) {
		f = stdin;
	} else {
		if (!fs_file_exists(path)) {
			LOG_E("'%s' is not a file", path);
			goto err;
		}

		if (!(f = fs_fopen(path, "rb"))) {
			goto err;
		}

		opened = true;
	}

	/* If the file is seekable (i.e. not a pipe), then we can get the size
	 * and read it all at once.  Otherwise, read it in chunks.
	 */

	bool seekable;
	if (!fs_is_seekable(f, &seekable)) {
		goto err;
	}

	if (seekable) {
		if (!fs_fsize(f, &src->len)) {
			goto err;
		}

		buf = z_calloc(src->len + 1, 1);
		read = fread(buf, 1, src->len, f);

		if (read != src->len) {
			LOG_E("failed to read entire file, only read %" PRIu64 "/%" PRId64 "bytes", (uint64_t)read, src->len);
			goto err;
		}
	} else {
		uint32_t buf_size = BUF_SIZE_4k;
		buf = z_calloc(buf_size + 1, 1);

		while ((read = fread(&buf[src->len], 1, buf_size - src->len, f))) {
			src->len += read;

			if (src->len >= buf_size) {
				buf_size *= 2;
				buf = z_realloc(buf, buf_size);
				memset(&buf[src->len], 0, buf_size - src->len);
			}
		}

		assert(src->len < buf_size && buf[src->len] == 0);

		if (!feof(f)) {
			LOG_E("failed to read entire file, only read %" PRId64 "bytes", src->len);
			goto err;
		}
	}

	if (opened) {
		if (!fs_fclose(f)) {
			goto err;
		}
	}

	src->src = buf;
	return true;
err:
	if (opened) {
		fs_fclose(f);
	}

	if (buf) {
		z_free(buf);
	}
	return false;
}

void
fs_source_dup(const struct source *src, struct source *dup)
{
	uint32_t label_len = strlen(src->label);
	char *buf = z_calloc(src->len + label_len + 1, 1);
	dup->label = &buf[src->len];
	dup->src = buf;
	dup->len = src->len;
	dup->reopen_type = src->reopen_type;

	memcpy(buf, src->src, src->len);
	memcpy(&buf[src->len], src->label, label_len);
}

void
fs_source_destroy(struct source *src)
{
	if (src->src) {
		z_free((char *)src->src);
	}
}

bool
fs_fwrite(const void *ptr, size_t size, FILE *f)
{
	size_t r;
	int err;

	if (!size) {
		return true;
	}

	r = fwrite(ptr, 1, size, f);
	assert(r <= size);

	if (r == size) {
		return true;
	} else {
		if ((err = ferror(f))) {
			LOG_E("fwrite failed: %s", strerror(err));
		} else {
			LOG_E("fwrite failed: unknown");
		}
		return false;
	}
}

bool
fs_fread(void *ptr, size_t size, FILE *f)
{
	size_t r;
	int err;

	if (!size) {
		return true;
	}

	r = fread(ptr, 1, size, f);
	assert(r <= size);

	if (r == size) {
		return true;
	} else {
		if (feof(f)) {
			LOG_E("fread got EOF");
		} else if ((err = ferror(f))) {
			LOG_E("fread failed: %s", strerror(err));
		} else {
			LOG_E("fread failed: unknown");
		}
		return false;
	}
}

bool
fs_write(const char *path, const uint8_t *buf, uint64_t buf_len)
{
	FILE *f;
	if (!(f = fs_fopen(path, "wb"))) {
		return false;
	}

	if (!fs_fwrite(buf, buf_len, f)) {
		LOG_E("failed to write entire file");
		fs_fclose(f);
		return false;
	}

	if (!fs_fclose(f)) {
		return false;
	}

	return true;
}

bool
fs_has_cmd(const char *cmd)
{
	SBUF_manual(buf);
	bool res = fs_find_cmd(NULL, &buf, cmd);
	sbuf_destroy(&buf);
	return res;
}

bool
fs_fileno(FILE *f, int *ret)
{
	int v;

	if ((v = fileno(f)) == -1) {
		LOG_E("failed fileno: %s", strerror(errno));
		return false;
	}

	*ret = v;
	return true;
}

struct fs_copy_dir_ctx {
	const char *src_base, *dest_base;
};

static enum iteration_result
fs_copy_dir_iter(void *_ctx, const char *path)
{
	enum iteration_result res = ir_err;
	struct fs_copy_dir_ctx *ctx = _ctx;
	struct stat sb;
	SBUF_manual(src);
	SBUF_manual(dest);

	path_join(NULL, &src, ctx->src_base, path);
	path_join(NULL, &dest, ctx->dest_base, path);

	if (!fs_stat(src.buf, &sb)) {
		goto ret;
	}

	if (S_ISDIR(sb.st_mode)) {
		if (!fs_dir_exists(dest.buf)) {
			if (!fs_mkdir(dest.buf)) {
				goto ret;
			}
		}

		if (!fs_copy_dir(src.buf, dest.buf)) {
			goto ret;
		}
	} else if (S_ISREG(sb.st_mode)) {
		if (!fs_copy_file(src.buf, dest.buf)) {
			goto ret;
		}
	} else {
		LOG_E("unhandled file type '%s'", path);
		goto ret;
	}

	res = ir_cont;
ret:
	sbuf_destroy(&src);
	sbuf_destroy(&dest);
	return res;
}

bool
fs_copy_dir(const char *src_base, const char *dest_base)
{
	struct fs_copy_dir_ctx ctx = {
		.src_base = src_base,
		.dest_base = dest_base,
	};

	if (!fs_dir_exists(dest_base)) {
		if (!fs_mkdir(dest_base)) {
			return ir_err;
		}
	}

	return fs_dir_foreach(src_base, &ctx, fs_copy_dir_iter);
}

struct fs_rmdir_ctx {
	const char *base_dir;
};

static enum iteration_result
fs_rmdir_iter(void *_ctx, const char *path)
{
	enum iteration_result ir_res = ir_err;
	struct fs_rmdir_ctx *ctx = _ctx;
	struct stat sb;

	SBUF(name);

	path_join(NULL, &name, ctx->base_dir, path);

	if (!fs_stat(name.buf, &sb)) {
		goto ret;
	}

	if (S_ISDIR(sb.st_mode)) {

		if (!fs_rmdir_recursive(name.buf)) {
			goto ret;
		}

		if (!fs_rmdir(name.buf)) {
			goto ret;
		}

	} else if (S_ISREG(sb.st_mode)) {

		if (!fs_remove(name.buf)) {
			goto ret;
		}

	} else {
		LOG_E("unhandled file type: %s", name.buf);
		goto ret;
	}

	ir_res = ir_cont;

ret:
	sbuf_destroy(&name);
	return ir_res;
}

bool
fs_rmdir_recursive(const char *path)
{
	struct fs_rmdir_ctx ctx = {
		.base_dir = path
	};

	return fs_dir_foreach(path, &ctx, fs_rmdir_iter);
}

bool
fs_is_a_tty(FILE *f)
{
	int fd;
	if (!fs_fileno(f, &fd)) {
		return false;
	}
	return fs_is_a_tty_from_fd(fd);
}

bool
fs_copy_metadata(const char *src, const char *dest)
{
	struct stat sb;
	if (!fs_stat(src, &sb)) {
		return false;
	}

	if (!fs_chmod(dest, sb.st_mode)) {
		return false;
	}

	return true;
}

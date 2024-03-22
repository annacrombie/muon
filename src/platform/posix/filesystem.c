/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include "buf_size.h"
#include "log.h"
#include "lang/string.h"
#include "platform/mem.h"
#include "platform/path.h"
#include "platform/filesystem.h"

static bool
fs_lstat(const char *path, struct stat *sb)
{
	if (lstat(path, sb) != 0) {
		LOG_E("failed lstat(%s): %s", path, strerror(errno));
		return false;
	}

	return true;
}

enum fs_mtime_result
fs_mtime(const char *path, int64_t *mtime)
{
	struct stat st;

	if (stat(path, &st) < 0) {
		if (errno != ENOENT) {
			LOG_E("failed stat(%s): %s", path, strerror(errno));
			return fs_mtime_result_err;
		}
		return fs_mtime_result_not_found;
	} else {
#ifdef __APPLE__
		*mtime = (int64_t)st.st_mtime * 1000000000 + st.st_mtimensec;
/*
   Illumos hides the members of st_mtim when you define _POSIX_C_SOURCE
   since it has not been updated to support POSIX.1-2008:
   https://www.illumos.org/issues/13327
 */
#elif defined(__sun) && !defined(__EXTENSIONS__)
		*mtime = (int64_t)st.st_mtim.__tv_sec * 1000000000 + st.st_mtim.__tv_nsec;
#else
		*mtime = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
#endif
		return fs_mtime_result_ok;
	}
}

bool
fs_exists(const char *path)
{
	return access(path, F_OK) == 0;
}

bool
fs_symlink_exists(const char *path)
{
	struct stat sb;

	// use lstat here instead of fs_lstat because we want to ignore errors
	return lstat(path, &sb) == 0 && S_ISLNK(sb.st_mode);
}

static bool
fs_lexists(const char *path)
{
	return fs_exists(path) || fs_symlink_exists(path);
}

bool
fs_file_exists(const char *path)
{
	struct stat sb;
	if (access(path, F_OK) != 0) {
		return false;
	} else if (!fs_stat(path, &sb)) {
		return false;
	} else if (!S_ISREG(sb.st_mode)) {
		return false;
	}

	return true;
}

bool
fs_exe_exists(const char *path)
{
	struct stat sb;
	if (access(path, X_OK) != 0) {
		return false;
	} else if (!fs_stat(path, &sb)) {
		return false;
	} else if (!S_ISREG(sb.st_mode)) {
		return false;
	}

	return true;
}

bool
fs_dir_exists(const char *path)
{
	struct stat sb;
	if (access(path, F_OK) != 0) {
		return false;
	} else if (!fs_stat(path, &sb)) {
		return false;
	} else if (!S_ISDIR(sb.st_mode)) {
		return false;
	}

	return true;
}

bool
fs_mkdir(const char *path)
{
	if (mkdir(path, 0755) == -1) {
		LOG_E("failed to create directory %s: %s", path, strerror(errno));
		return false;
	}

	return true;
}

bool
fs_rmdir(const char *path)
{
	if (rmdir(path) == -1) {
		LOG_E("failed to remove directory %s: %s", path, strerror(errno));
		return false;
	}

	return true;
}

static bool
fs_copy_link(const char *src, const char *dest)
{
	bool res = false;
	ssize_t n;
	char *buf;

	struct stat st;
	if (!fs_lstat(src, &st)) {
		return false;
	}

	if (!S_ISLNK(st.st_mode)) {
		return false;
	}

	// TODO: allow pseudo-files?
	assert(st.st_size > 0);

	buf = z_malloc(st.st_size + 1);
	n = readlink(src, buf, st.st_size);
	if (n == -1) {
		LOG_E("readlink('%s') failed: %s", src, strerror(errno));
		goto ret;
	}

	buf[n] = '\0';
	res = fs_make_symlink(buf, dest, true);
ret:
	z_free(buf);
	return res;
}

bool
fs_copy_file(const char *src, const char *dest)
{
	bool res = false;
	FILE *f_src = NULL;
	int f_dest = 0;

	struct stat st;
	if (!fs_lstat(src, &st)) {
		goto ret;
	}

	if (S_ISLNK(st.st_mode)) {
		return fs_copy_link(src, dest);
	} else if (!S_ISREG(st.st_mode)) {
		LOG_E("unhandled file type");
		goto ret;
	}

	if (!(f_src = fs_fopen(src, "r"))) {
		goto ret;
	}

	if ((f_dest = open(dest, O_CREAT | O_WRONLY | O_TRUNC, st.st_mode)) == -1) {
		LOG_E("failed to create destination file %s: %s", dest, strerror(errno));
		goto ret;
	}

	assert(f_dest != 0);

	size_t r;
	ssize_t w;
	char buf[BUF_SIZE_32k];

	while ((r = fread(buf, 1, BUF_SIZE_32k, f_src)) > 0) {
		errno = 0; // to ensure that we get the error from write() only

		if ((w = write(f_dest, buf, r)) == -1) {
			LOG_E("failed write(): %s", strerror(errno));
			goto ret;
		} else {
			assert(w >= 0);

			if ((size_t)w < r) {
				LOG_E("incomplete write: %s", strerror(errno));
				goto ret;
			}
		}
	}

	if (!feof(f_src)) {
		LOG_E("incomplete read: %s", strerror(errno));
		goto ret;
	}

	res = true;
ret:
	if (f_src) {
		if (!fs_fclose(f_src)) {
			res = false;
		}
	}

	if (f_dest > 0) {
		if (close(f_dest) == -1) {
			LOG_E("failed close(): %s", strerror(errno));
			res = false;
		}
	}

	return res;
}

bool
fs_dir_foreach(const char *path, void *_ctx, fs_dir_foreach_cb cb)
{
	DIR *d;
	struct dirent *ent;

	if (!(d = opendir(path))) {
		LOG_E("failed opendir(%s): %s", path, strerror(errno));
		return false;
	}

	bool loop = true, res = true;
	while (loop && (ent = readdir(d))) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
			continue;
		}

		switch (cb(_ctx, ent->d_name)) {
		case ir_cont:
			break;
		case ir_done:
			loop = false;
			break;
		case ir_err:
			loop = false;
			res = false;
			break;
		}
	}

	if (closedir(d) != 0) {
		LOG_E("failed closedir(): %s", strerror(errno));
		return false;
	}

	return res;
}

bool
fs_remove(const char *path)
{
	if (remove(path) != 0) {
		LOG_E("failed remove(\"%s\"): %s", path, strerror(errno));
		return false;
	}

	return true;
}

bool
fs_make_symlink(const char *target, const char *path, bool force)
{
	if (force && fs_lexists(path)) {
		if (!fs_remove(path)) {
			return false;
		}
	}

	if (symlinkat(target, 0, path) != 0) {
		LOG_E("failed symlink(\"%s\", \"%s\"): %s", target, path, strerror(errno));
		return false;
	}

	return true;
}

const char *
fs_user_home(void)
{
	return getenv("HOME");
}

bool
fs_is_a_tty_from_fd(int fd)
{
	errno = 0;
	if (isatty(fd) == 1) {
		return true;
	} else {
		if (errno != ENOTTY) {
			LOG_W("isatty() failed: %s", strerror(errno));
		}
		return false;
	}
}

bool
fs_chmod(const char *path, uint32_t mode)
{
	if (mode & S_ISVTX) {
		struct stat sb;
		if (!fs_stat(path, &sb)) {
			return false;
		}
		if (!S_ISDIR(sb.st_mode)) {
			LOG_E("attempt to set sticky bit on regular file: %s", path);
			return false;
		}
	}

	if (fs_symlink_exists(path)) {
		if (fchmodat(AT_FDCWD, path, (mode_t)mode, AT_SYMLINK_NOFOLLOW) == -1) {
			if (errno == EOPNOTSUPP) {
				LOG_W("changing permissions of symlinks not supported");
				return true;
			}

			LOG_E("failed fchmodat(AT_FCWD, %s, %o, AT_SYMLINK_NOFOLLOW): %s", path, mode, strerror(errno));
			return false;
		}
	} else if (chmod(path, (mode_t)mode) == -1) {
		LOG_E("failed chmod(%s, %o): %s", path, mode, strerror(errno));
		return false;
	}

	return true;
}

bool
fs_find_cmd(struct workspace *wk, struct sbuf *buf, const char *cmd)
{
	assert(*cmd);
	uint32_t len;
	const char *env_path, *base_start;

	sbuf_clear(buf);

	if (!path_is_basename(cmd)) {
		path_make_absolute(wk, buf, cmd);

		if (fs_exe_exists(buf->buf)) {
			return true;
		} else {
			return false;
		}
	}

	if (!(env_path = getenv("PATH"))) {
		LOG_E("failed to get the value of PATH");
		return false;
	}

	base_start = env_path;
	while (true) {
		if (!*env_path || *env_path == ENV_PATH_SEP) {
			len = env_path - base_start;

			sbuf_clear(buf);
			sbuf_pushn(wk, buf, base_start, len);

			base_start = env_path + 1;

			path_push(wk, buf, cmd);

			if (fs_exe_exists(buf->buf)) {
				return true;
			}

			if (!*env_path) {
				break;
			}
		}

		++env_path;
	}

	return false;
}

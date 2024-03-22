/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_PLATFORM_FILESYSTEM_H
#define MUON_PLATFORM_FILESYSTEM_H

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>

#include "datastructures/iterator.h"

enum source_reopen_type {
	source_reopen_type_none,
	source_reopen_type_embedded,
	source_reopen_type_file,
};

struct source {
	const char *label;
	const char *src;
	uint64_t len;
	//
	// only necessary if src is NULL.  If so, this source will be re-read
	// on error to fetch appropriate context lines.
	enum source_reopen_type reopen_type;
};

struct workspace;
struct sbuf;

bool fs_stat(const char *path, struct stat *sb);
enum fs_mtime_result { fs_mtime_result_ok, fs_mtime_result_not_found, fs_mtime_result_err };
enum fs_mtime_result fs_mtime(const char *path, int64_t *mtime);
bool fs_exists(const char *path);
bool fs_file_exists(const char *path);
bool fs_symlink_exists(const char *path);
bool fs_exe_exists(const char *path);
bool fs_dir_exists(const char *path);
bool fs_mkdir(const char *path);
bool fs_mkdir_p(const char *path);
bool fs_rmdir(const char *path);
bool fs_rmdir_recursive(const char *path);
bool fs_read_entire_file(const char *path, struct source *src);
bool fs_fsize(FILE *file, uint64_t *ret);
bool fs_fclose(FILE *file);
FILE *fs_fopen(const char *path, const char *mode);
bool fs_fwrite(const void *ptr, size_t size, FILE *f);
bool fs_fread(void *ptr, size_t size, FILE *f);
bool fs_write(const char *path, const uint8_t *buf, uint64_t buf_len);
bool fs_find_cmd(struct workspace *wk, struct sbuf *buf, const char *cmd);
bool fs_has_cmd(const char *cmd);
void fs_source_destroy(struct source *src);
void fs_source_dup(const struct source *src, struct source *dup);
bool fs_copy_file(const char *src, const char *dest);
bool fs_copy_dir(const char *src_base, const char *dest_base);
bool fs_fileno(FILE *f, int *ret);
bool fs_make_symlink(const char *target, const char *path, bool force);
bool fs_fseek(FILE *file, size_t off);
bool fs_ftell(FILE *file, uint64_t *res);
const char *fs_user_home(void);
bool fs_is_a_tty_from_fd(int fd);
bool fs_is_a_tty(FILE *f);
bool fs_chmod(const char *path, uint32_t mode);
bool fs_copy_metadata(const char *src, const char *dest);
bool fs_remove(const char *path);
/* Windows only */
bool fs_has_extension(const char *path, const char *ext);

typedef enum iteration_result ((*fs_dir_foreach_cb)(void *_ctx, const char *path));
bool fs_dir_foreach(const char *path, void *_ctx, fs_dir_foreach_cb cb);

#ifndef S_ISGID
	#define S_ISGID 0
#endif

#ifndef S_ISUID
	#define S_ISUID 0
#endif

#ifndef S_ISVTX
	#define S_ISVTX 0
#endif

#endif

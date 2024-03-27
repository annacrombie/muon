/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_PLATFORM_PATH_H
#define MUON_PLATFORM_PATH_H
#include <stdbool.h>
#include <stdint.h>

#define PATH_SEP '/'

#ifdef _WIN32
#define ENV_PATH_SEP ';'
#else
#define ENV_PATH_SEP ':'
#endif

#ifdef _WIN32
#define ENV_PATH_SEP_STR ";"
#else
#define ENV_PATH_SEP_STR ":"
#endif

struct workspace;
struct sbuf;

void path_init(void);
void path_deinit(void);

bool path_chdir(const char *path);

void path_copy(struct workspace *wk, struct sbuf *sb, const char *path);

const char *path_cwd(void);
void path_copy_cwd(struct workspace *wk, struct sbuf *sb);

bool path_is_absolute(const char *path);
bool path_is_basename(const char *path);
bool path_is_subpath(const char *base, const char *sub);

void path_push(struct workspace *wk, struct sbuf *sb, const char *b);
void path_join(struct workspace *wk, struct sbuf *sb, const char *a, const char *b);
// like path_join but won't discard a if b is an absolute path
void path_join_absolute(struct workspace *wk, struct sbuf *sb, const char *a, const char *b);

void path_make_absolute(struct workspace *wk, struct sbuf *buf, const char *path);
void path_relative_to(struct workspace *wk, struct sbuf *buf, const char *base_raw, const char *path_raw);
void path_without_ext(struct workspace *wk, struct sbuf *buf, const char *path);
void path_basename(struct workspace *wk, struct sbuf *buf, const char *path);
void path_dirname(struct workspace *wk, struct sbuf *buf, const char *path);
void path_executable(struct workspace *wk, struct sbuf *buf, const char *path);
void _path_normalize(struct workspace *wk, struct sbuf *buf, bool optimize);
void path_to_posix(char *path);
void shell_escape(struct workspace *wk, struct sbuf *sb, const char *str);
#endif

/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_PLATFORM_RUN_CMD_H
#define MUON_PLATFORM_RUN_CMD_H

#include <stdbool.h>
#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "lang/string.h"

enum run_cmd_state {
	run_cmd_running,
	run_cmd_finished,
	run_cmd_error,
};

enum run_cmd_ctx_flags {
	run_cmd_ctx_flag_async = 1 << 0,
	run_cmd_ctx_flag_dont_capture = 1 << 1,
};

#ifdef _WIN32
struct win_pipe_inst {
	OVERLAPPED overlapped;
	HANDLE handle;
	HANDLE child_handle;
	HANDLE event;
	char overlapped_buf[4 << 10];
	bool is_pending, is_eof;
};
#endif

struct run_cmd_ctx {
	struct sbuf err, out;
	const char *err_msg; // set on error
	const char *chdir; // set by caller
	const char *stdin_path; // set by caller
	int status;
	enum run_cmd_ctx_flags flags;
#ifdef _WIN32
	HANDLE process;
	bool close_pipes;
	struct win_pipe_inst pipe_out, pipe_err;
#else
	int pipefd_out[2], pipefd_err[2];
	int input_fd;
	int pid;

	bool input_fd_open;
	bool pipefd_out_open[2], pipefd_err_open[2];
#endif
};

void push_argv_single(const char **argv, uint32_t *len, uint32_t max, const char *arg);
void argstr_pushall(const char *argstr, uint32_t argc, const char **argv, uint32_t *argi, uint32_t max);
/*
 * argstr is a NUL delimited array of strings
 * envstr is like argstr, every two strings is considered a key/value pair
 */
uint32_t argstr_to_argv(const char *argstr, uint32_t argc, const char *prepend, char *const **res);

struct source;
bool run_cmd_determine_interpreter(struct source *src,
	const char *path,
	const char **err_msg,
	const char **new_argv0,
	const char **new_argv1);

bool run_cmd(struct run_cmd_ctx *ctx, const char *argstr, uint32_t argc, const char *envstr, uint32_t envc);
bool run_cmd_argv(struct run_cmd_ctx *ctx, char *const *argv, const char *envstr, uint32_t envc);
enum run_cmd_state run_cmd_collect(struct run_cmd_ctx *ctx);
void run_cmd_ctx_destroy(struct run_cmd_ctx *ctx);
bool run_cmd_kill(struct run_cmd_ctx *ctx, bool force);

// runs a command by passing a single string containing both the command and
// arguments.
// currently only used by samurai on windows
bool run_cmd_unsplit(struct run_cmd_ctx *ctx, char *cmd, const char *envstr, uint32_t envc);

#endif

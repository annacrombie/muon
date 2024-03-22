/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "args.h"
#include "buf_size.h"
#include "error.h"
#include "log.h"
#include "platform/filesystem.h"
#include "platform/mem.h"
#include "platform/path.h"
#include "platform/run_cmd.h"

extern char **environ;

enum copy_pipe_result {
	copy_pipe_result_finished,
	copy_pipe_result_waiting,
	copy_pipe_result_failed,
};

static enum copy_pipe_result
copy_pipe(int pipe, struct sbuf *sbuf)
{
	ssize_t b;
	char buf[4096];

	while (true) {
		b = read(pipe, buf, sizeof(buf));

		if (b == -1) {
			if (errno == EAGAIN) {
				return copy_pipe_result_waiting;
			} else {
				return copy_pipe_result_failed;
			}
		} else if (b == 0) {
			return copy_pipe_result_finished;
		}

		sbuf_pushn(0, sbuf, buf, b);
	}
}

static enum copy_pipe_result
copy_pipes(struct run_cmd_ctx *ctx)
{
	enum copy_pipe_result res;

	if ((res = copy_pipe(ctx->pipefd_out[0], &ctx->out)) == copy_pipe_result_failed) {
		return res;
	}

	switch (copy_pipe(ctx->pipefd_err[0], &ctx->err)) {
	case copy_pipe_result_waiting: return copy_pipe_result_waiting;
	case copy_pipe_result_finished: return res;
	case copy_pipe_result_failed: return copy_pipe_result_failed;
	default: UNREACHABLE_RETURN;
	}
}

static void
run_cmd_ctx_close_fds(struct run_cmd_ctx *ctx)
{
	if (ctx->pipefd_err_open[0] && close(ctx->pipefd_err[0]) == -1) {
		LOG_E("failed to close: %s", strerror(errno));
	}
	ctx->pipefd_err_open[0] = false;

	if (ctx->pipefd_err_open[1] && close(ctx->pipefd_err[1]) == -1) {
		LOG_E("failed to close: %s", strerror(errno));
	}
	ctx->pipefd_err_open[1] = false;

	if (ctx->pipefd_out_open[0] && close(ctx->pipefd_out[0]) == -1) {
		LOG_E("failed to close: %s", strerror(errno));
	}
	ctx->pipefd_out_open[0] = false;

	if (ctx->pipefd_out_open[1] && close(ctx->pipefd_out[1]) == -1) {
		LOG_E("failed to close: %s", strerror(errno));
	}
	ctx->pipefd_out_open[1] = false;

	if (ctx->input_fd_open && close(ctx->input_fd) == -1) {
		LOG_E("failed to close: %s", strerror(errno));
	}
	ctx->input_fd_open = false;
}

enum run_cmd_state
run_cmd_collect(struct run_cmd_ctx *ctx)
{
	int status;
	int r;

	enum copy_pipe_result pipe_res = 0;

	while (true) {
		if (!(ctx->flags & run_cmd_ctx_flag_dont_capture)) {
			if ((pipe_res = copy_pipes(ctx)) == copy_pipe_result_failed) {
				return run_cmd_error;
			}
		}

		if ((r = waitpid(ctx->pid, &status, WNOHANG)) == -1) {
			return run_cmd_error;
		} else if (r == 0) {
			if (ctx->flags & run_cmd_ctx_flag_async) {
				return run_cmd_running;
			} else {
				// sleep here for 1ms to give the process some
				// time to complete
				struct timespec req = {
					.tv_nsec = 1000000,
				};
				nanosleep(&req, NULL);
			}
		} else {
			break;
		}
	}

	assert(r == ctx->pid);

	if (!(ctx->flags & run_cmd_ctx_flag_dont_capture)) {
		while (pipe_res != copy_pipe_result_finished) {
			if ((pipe_res = copy_pipes(ctx)) == copy_pipe_result_failed) {
				return run_cmd_error;
			}
		}
	}

	run_cmd_ctx_close_fds(ctx);

	if (WIFEXITED(status)) {
		ctx->status = WEXITSTATUS(status);
	} else if (WIFSIGNALED(status)) {
		// TODO: it may be helpful to communicate the signal that
		// caused the command to terminate, this is available with
		// `WTERMSIG(status)`
		ctx->err_msg = "command terminated due to signal";
		return run_cmd_error;
	} else {
		ctx->err_msg = "command exited abnormally";
		return run_cmd_error;
	}

	return run_cmd_finished;
}

static bool
open_run_cmd_pipe(int fds[2], bool fds_open[2])
{
	if (pipe(fds) == -1) {
		LOG_E("failed to create pipe: %s", strerror(errno));
		return false;
	}

	fds_open[0] = true;
	fds_open[1] = true;

	int flags;
	if ((flags = fcntl(fds[0], F_GETFL)) == -1) {
		LOG_E("failed to get pipe flags: %s", strerror(errno));
		return false;
	} else if (fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) == -1) {
		LOG_E("failed to set pipe flag O_NONBLOCK: %s", strerror(errno));
		return false;
	}

	return true;
}

static bool
run_cmd_internal(struct run_cmd_ctx *ctx, const char *_cmd, char *const *argv, const char *envstr, uint32_t envc)
{
	const char *p;
	SBUF_manual(cmd);

	if (!fs_find_cmd(NULL, &cmd, _cmd)) {
		ctx->err_msg = "command not found";
		return false;
	}

	if (log_should_print(log_debug)) {
		LL("executing %s:", cmd.buf);
		char *const *ap;

		for (ap = argv; *ap; ++ap) {
			log_plain(" '%s'", *ap);
		}
		log_plain("\n");

		if (envstr) {
			const char *k;
			uint32_t i = 0;
			LL("env:");
			p = k = envstr;
			for (; envc; ++p) {
				if (!p[0]) {
					if (!k) {
						k = p + 1;
					} else {
						log_plain(" %s='%s'", k, p + 1);
						k = NULL;

						if (++i >= envc) {
							break;
						}
					}
				}
			}

			log_plain("\n");
		}
	}

	if (ctx->stdin_path) {
		ctx->input_fd = open(ctx->stdin_path, O_RDONLY);
		if (ctx->input_fd == -1) {
			LOG_E("failed to open %s: %s", ctx->stdin_path, strerror(errno));
			goto err;
		}

		ctx->input_fd_open = true;
	}

	if (!(ctx->flags & run_cmd_ctx_flag_dont_capture)) {
		sbuf_init(&ctx->out, 0, 0, sbuf_flag_overflow_alloc);
		sbuf_init(&ctx->err, 0, 0, sbuf_flag_overflow_alloc);

		if (!open_run_cmd_pipe(ctx->pipefd_out, ctx->pipefd_out_open)) {
			goto err;
		} else if (!open_run_cmd_pipe(ctx->pipefd_err, ctx->pipefd_err_open)) {
			goto err;
		}
	}

	if ((ctx->pid = fork()) == -1) {
		goto err;
	} else if (ctx->pid == 0 /* child */) {
		if (ctx->chdir) {
			if (chdir(ctx->chdir) == -1) {
				LOG_E("failed to chdir to %s: %s", ctx->chdir, strerror(errno));
				exit(1);
			}
		}

		if (ctx->stdin_path) {
			if (dup2(ctx->input_fd, 0) == -1) {
				LOG_E("failed to dup stdin: %s", strerror(errno));
				exit(1);
			}
		}

		if (!(ctx->flags & run_cmd_ctx_flag_dont_capture)) {
			if (dup2(ctx->pipefd_out[1], 1) == -1) {
				LOG_E("failed to dup stdout: %s", strerror(errno));
				exit(1);
			}
			if (dup2(ctx->pipefd_err[1], 2) == -1) {
				LOG_E("failed to dup stderr: %s", strerror(errno));
				exit(1);
			}
		}

		if (envstr) {
			const char *k;
			uint32_t i = 0;
			p = k = envstr;
			for (; envc; ++p) {
				if (!p[0]) {
					if (!k) {
						k = p + 1;
					} else {
						int err;
						if ((err = setenv(k, p + 1, 1)) != 0) {
							LOG_E("failed to set environment var %s='%s': %s",
								k,
								p + 1,
								strerror(err));
							exit(1);
						}
						k = NULL;

						if (++i >= envc) {
							break;
						}
					}
				}
			}
		}

		if (execve(cmd.buf, (char *const *)argv, environ) == -1) {
			LOG_E("%s: %s", cmd.buf, strerror(errno));
			exit(1);
		}

		abort();
	}

	/* parent */
	sbuf_destroy(&cmd);

	if (ctx->pipefd_err_open[1] && close(ctx->pipefd_err[1]) == -1) {
		LOG_E("failed to close: %s", strerror(errno));
	}
	ctx->pipefd_err_open[1] = false;

	if (ctx->pipefd_out_open[1] && close(ctx->pipefd_out[1]) == -1) {
		LOG_E("failed to close: %s", strerror(errno));
	}
	ctx->pipefd_out_open[1] = false;

	if (ctx->flags & run_cmd_ctx_flag_async) {
		return true;
	}

	return run_cmd_collect(ctx) == run_cmd_finished;
err:
	return false;
}

static bool
build_argv(struct run_cmd_ctx *ctx,
	struct source *src,
	const char *argstr,
	uint32_t argstr_argc,
	char *const *old_argv,
	struct sbuf *cmd,
	const char ***argv)
{
	const char *argv0, *new_argv0 = NULL, *new_argv1 = NULL;
	const char **new_argv;
	uint32_t argc = 0, argi = 0;

	if (argstr) {
		argv0 = argstr;
		argc = argstr_argc;
	} else {
		assert(old_argv);

		argv0 = old_argv[0];
		for (; old_argv[argc]; ++argc) {
		}
	}

	assert(*argv0 && "argv0 cannot be empty");

	sbuf_clear(cmd);
	sbuf_pushs(NULL, cmd, argv0);

	if (!path_is_basename(cmd->buf)) {
		path_make_absolute(NULL, cmd, argv0);

		if (!fs_exe_exists(cmd->buf)) {
			if (!run_cmd_determine_interpreter(src, cmd->buf, &ctx->err_msg, &new_argv0, &new_argv1)) {
				return false;
			}

			argc += new_argv1 ? 2 : 1;

			path_copy(NULL, cmd, new_argv0);
		}
	}

	assert(cmd->len);

	if (!new_argv0 && old_argv) {
		*argv = NULL;
		return true;
	}

	new_argv = z_calloc(argc + 1, sizeof(const char *));
	argi = 0;
	if (new_argv0) {
		push_argv_single(new_argv, &argi, argc, new_argv0);
		if (new_argv1) {
			push_argv_single(new_argv, &argi, argc, new_argv1);
		}
	}

	if (argstr) {
		argstr_pushall(argstr, argstr_argc, new_argv, &argi, argc);
	} else {
		uint32_t i;
		for (i = 0; old_argv[i]; ++i) {
			push_argv_single(new_argv, &argi, argc, old_argv[i]);
		}
	}

	*argv = new_argv;
	return true;
}

bool
run_cmd_argv(struct run_cmd_ctx *ctx, char *const *argv, const char *envstr, uint32_t envc)
{
	bool ret = false;
	struct source src = { 0 };
	const char **new_argv = NULL;

	SBUF_manual(cmd);
	if (!build_argv(ctx, &src, NULL, 0, argv, &cmd, &new_argv)) {
		goto err;
	}

	if (new_argv) {
		argv = (char **)new_argv;
	}

	ret = run_cmd_internal(ctx, cmd.buf, (char *const *)argv, envstr, envc);
err:
	fs_source_destroy(&src);
	if (new_argv) {
		z_free((void *)new_argv);
	}
	sbuf_destroy(&cmd);
	return ret;
}

bool
run_cmd(struct run_cmd_ctx *ctx, const char *argstr, uint32_t argc, const char *envstr, uint32_t envc)
{
	bool ret = false;
	struct source src = { 0 };
	const char **argv = NULL;

	SBUF_manual(cmd);
	if (!build_argv(ctx, &src, argstr, argc, NULL, &cmd, &argv)) {
		goto err;
	}

	ret = run_cmd_internal(ctx, cmd.buf, (char *const *)argv, envstr, envc);
err:
	fs_source_destroy(&src);
	if (argv) {
		z_free((void *)argv);
	}
	sbuf_destroy(&cmd);
	return ret;
}

void
run_cmd_ctx_destroy(struct run_cmd_ctx *ctx)
{
	run_cmd_ctx_close_fds(ctx);

	sbuf_destroy(&ctx->out);
	sbuf_destroy(&ctx->err);
}

bool
run_cmd_kill(struct run_cmd_ctx *ctx, bool force)
{
	int r;
	if (force) {
		r = kill(ctx->pid, SIGKILL);
	} else {
		r = kill(ctx->pid, SIGTERM);
	}

	if (r != 0) {
		LOG_E("error killing process %d: %s", ctx->pid, strerror(errno));
		return false;
	}

	return true;
}

bool
run_cmd_unsplit(struct run_cmd_ctx *ctx, char *cmd, const char *envstr, uint32_t envc)
{
	assert(false && "this function should only be called under windows");
}

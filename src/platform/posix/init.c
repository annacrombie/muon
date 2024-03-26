/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <signal.h>

#include "platform/init.h"

static struct {
	void((*handler)(void *ctx));
	void *ctx;
} platform_sigabrt_handler_ctx;

void
platform_sigabrt_handler(int signo, siginfo_t *info, void *ctx)
{
	if (platform_sigabrt_handler_ctx.handler) {
		platform_sigabrt_handler_ctx.handler(platform_sigabrt_handler_ctx.ctx);
	}
}

void
platform_init(void)
{
	{
		struct sigaction act = {
			.sa_flags = SA_SIGINFO,
			.sa_sigaction = platform_sigabrt_handler,
		};
		sigaction(SIGABRT, &act, 0);
	}
}

void
platform_set_abort_handler(void((*handler)(void *ctx)), void *ctx)
{
	platform_sigabrt_handler_ctx.handler = handler;
	platform_sigabrt_handler_ctx.ctx = ctx;
}

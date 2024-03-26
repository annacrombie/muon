/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <fcntl.h>
#include <stdio.h>

#include "platform/init.h"

void
platform_init(void)
{
	setmode(fileno(stdin), O_BINARY);
	setmode(fileno(stdout), O_BINARY);
	setmode(fileno(stderr), O_BINARY);
}

void
platform_set_abort_handler(void((*handler)(void *ctx)), void *ctx)
{
}

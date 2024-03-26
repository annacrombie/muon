/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_PLATFORM_INIT_H
#define MUON_PLATFORM_INIT_H

void platform_init(void);
void platform_set_abort_handler(void((*handler)(void *ctx)), void *ctx);

#endif

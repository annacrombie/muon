/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_LANG_COMPILER_H
#define MUON_LANG_COMPILER_H

#include <stdbool.h>
#include <stdint.h>

#include "lang/workspace.h"

bool compile(struct workspace *wk, struct source *src, uint32_t flags);
#endif

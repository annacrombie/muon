/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_LANG_COMPILER_H
#define MUON_LANG_COMPILER_H

#include <stdbool.h>
#include <stdint.h>

#include "lang/workspace.h"

void compiler_write_initial_code_segment(struct workspace *wk);
bool compile(struct workspace *wk, struct source *src, uint32_t flags, uint32_t *entry);
#endif

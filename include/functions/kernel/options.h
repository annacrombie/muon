/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_FUNCTIONS_KERNEL_OPTIONS_H
#define MUON_FUNCTIONS_KERNEL_OPTIONS_H

#include "lang/workspace.h"

bool func_option(struct workspace *wk, obj self, obj *res);
bool func_get_option(struct workspace *wk, obj self, obj *res);
#endif

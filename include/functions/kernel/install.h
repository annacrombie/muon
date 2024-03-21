/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_FUNCTIONS_KERNEL_INSTALL_H
#define MUON_FUNCTIONS_KERNEL_INSTALL_H
#include "functions/common.h"

bool func_install_subdir(struct workspace *wk, obj _, obj *ret);
bool func_install_man(struct workspace *wk, obj _, obj *ret);
bool func_install_symlink(struct workspace *wk, obj _, obj *ret);
bool func_install_emptydir(struct workspace *wk, obj _, obj *ret);
bool func_install_data(struct workspace *wk, obj _, obj *res);
bool func_install_headers(struct workspace *wk, obj _, obj *ret);
#endif

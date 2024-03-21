/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_FUNCTIONS_KERNEL_BUILD_TARGET_H
#define MUON_FUNCTIONS_KERNEL_BUILD_TARGET_H
#include "functions/common.h"

bool func_both_libraries(struct workspace *wk, obj _, obj *res);
bool func_build_target(struct workspace *wk, obj _, obj *res);
bool func_executable(struct workspace *wk, obj _, obj *res);
bool func_library(struct workspace *wk, obj _, obj *res);
bool func_shared_library(struct workspace *wk, obj _, obj *res);
bool func_static_library(struct workspace *wk, obj _, obj *res);
bool func_shared_module(struct workspace *wk, obj _, obj *res);
#endif

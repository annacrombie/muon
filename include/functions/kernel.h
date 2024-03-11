/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_FUNCTIONS_KERNEL_H
#define MUON_FUNCTIONS_KERNEL_H
#include "functions/common.h"

bool func_range_common(struct workspace *wk, uint32_t args_node, struct range_params *res);

extern const struct func_impl impl_tbl_kernel[];
extern const struct func_impl impl_tbl_kernel_internal[];
extern const struct func_impl impl_tbl_kernel_opts[];
#endif

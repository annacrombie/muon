/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "functions/common.h"
#include "functions/xxx.h"
#include "log.h"

static bool
func_(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
}

const struct func_impl impl_tbl_xxx[] = {
	{ "", func_ },
	{ NULL, NULL },
};

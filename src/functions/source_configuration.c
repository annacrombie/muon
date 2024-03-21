/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "functions/common.h"
#include "functions/source_configuration.h"
#include "lang/typecheck.h"
#include "log.h"

static bool
func_source_configuration_sources(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = get_obj_source_configuration(wk, self)->sources;
	return true;
}

static bool
func_source_configuration_dependencies(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = get_obj_source_configuration(wk, self)->dependencies;
	return true;
}

const struct func_impl impl_tbl_source_configuration[] = {
	{ "sources", func_source_configuration_sources, tc_array, true },
	{ "dependencies", func_source_configuration_dependencies, tc_array, true },
	{ NULL, NULL },
};

/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "error.h"
#include "formats/ini.h"
#include "functions/common.h"
#include "functions/modules/keyval.h"
#include "lang/typecheck.h"
#include "platform/filesystem.h"
#include "platform/mem.h"

struct keyval_parse_ctx {
	struct workspace *wk;
	obj dict;
};

static bool
keyval_parse_cb(void *_ctx, struct source *src, const char *sect, const char *k, const char *v, struct source_location location)
{
	struct keyval_parse_ctx *ctx = _ctx;

	obj_dict_set(ctx->wk, ctx->dict, make_str(ctx->wk, k), make_str(ctx->wk, v));
	return true;
}

static bool
func_module_keyval_load(struct workspace *wk, obj self, uint32_t args_node, obj *res)
{
	bool ret = false;
	struct args_norm an[] = { { tc_string | tc_file }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	const char *path = NULL;
	switch (get_obj_type(wk, an[0].val)) {
	case obj_file:
		path = get_file_path(wk, an[0].val);
		break;
	case obj_string:
		path = get_cstr(wk, an[0].val);
		break;
	default:
		UNREACHABLE;
	}

	make_obj(wk, res, obj_dict);

	struct keyval_parse_ctx ctx = {
		.wk = wk,
		.dict = *res,
	};

	struct source src = { 0 };
	char *buf = NULL;
	if (!keyval_parse(path, &src, &buf, keyval_parse_cb, &ctx)) {
		goto ret;
	}

	ret = true;
ret:
	fs_source_destroy(&src);
	if (buf) {
		z_free(buf);
	}
	return ret;
}

const struct func_impl impl_tbl_module_keyval[] = {
	{ "load", func_module_keyval_load, tc_dict, },
	{ NULL, NULL },
};

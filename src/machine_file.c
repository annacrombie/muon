/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <assert.h>
#include <string.h>

#include "error.h"
#include "formats/ini.h"
#include "machine_file.h"
#include "platform/filesystem.h"
#include "platform/mem.h"

struct machine_info {
	obj system, cpu_family, cpu, endian;
	struct {
		obj c, ld, ar;
	} binaries;
};

enum machine_file_section {
	mfile_section_constants,
	mfile_section_binaries,
	mfile_section_properties,
	mfile_section_host_machine,
	machine_file_section_count,
};

static const char *machine_file_section_names[machine_file_section_count] = {
	[mfile_section_constants] = "constants",
	[mfile_section_binaries] = "binaries",
	[mfile_section_properties] = "properties",
	[mfile_section_host_machine] = "host_machine",
};

static bool
mfile_lookup(const char *val, const char *table[], uint32_t len, uint32_t *ret)
{
	uint32_t i;
	for (i = 0; i < len; ++i) {
		if (strcmp(val, table[i]) == 0) {
			*ret = i;
			return true;
		}
	}

	return false;
}

struct machine_file_parse_ctx {
	struct workspace *wk, *dest_wk;
};

static bool
machine_file_parse_cb(void *_ctx, struct source *src, const char *_sect,
	const char *k, const char *v, struct source_location location)
{
	struct machine_file_parse_ctx *ctx = _ctx;
	enum machine_file_section sect;

	if (!mfile_lookup(_sect, machine_file_section_names,
		machine_file_section_count, &sect)) {
		if (!k) {
			error_messagef(src, location, log_error, "invalid section '%s'", _sect);
		}
		return false;
	} else if (!k) {
		/* TODO: hash_clear(&current_project(ctx->wk)->scope); */
		return true;
	}

	if (!_sect) {
		error_messagef(src, location, log_error, "key not under any section");
		return false;
	}

	struct source val_src = { .label = k, .src = v, .len = strlen(v), };

	obj res;
	if (!eval(ctx->wk, &val_src, eval_mode_default, &res)) {
		error_messagef(src, location, log_error, "failed to parse value");
		return false;
	}

	/* char buf[2048]; */
	/* obj_to_s(ctx->wk, res, buf, 2048); */

	if (sect == mfile_section_constants) {
		/* TODO: obj_dict_set(ctx->wk, ctx->wk->global_scope, make_str(ctx->wk, k), res); */
	} else {
		/* TODO: hash_set_str(&current_project(ctx->wk)->scope, k, res); */

		obj cloned, objkey, dest_dict;
		if (!obj_clone(ctx->wk, ctx->dest_wk, res, &cloned)) {
			return false;
		}

		objkey = make_str(ctx->dest_wk, k);

		switch (sect) {
		case mfile_section_binaries:
			/* printf("setting binaries.%s=%s\n", k, buf); */

			dest_dict = ctx->dest_wk->binaries;
			break;
		case mfile_section_host_machine:
			dest_dict = ctx->dest_wk->host_machine;
			break;
		default:
			assert(false && "todo");
			return false;
		}

		obj_dict_set(ctx->dest_wk, dest_dict, objkey, cloned);
	}

	return true;
}

bool
machine_file_parse(struct workspace *dest_wk, const char *path)
{
	bool ret = false;

	struct workspace wk;
	workspace_init(&wk);

	wk.vm.lang_mode = language_internal;

	uint32_t proj_id;
	make_project(&wk, &proj_id, "dummy", wk.source_root, wk.build_root);

	struct machine_file_parse_ctx ctx = { .wk = &wk, .dest_wk = dest_wk, };

	struct source src;
	char *buf = NULL;
	if (!ini_parse(path, &src, &buf, machine_file_parse_cb, &ctx)) {
		goto ret;
	}

	ret = true;
ret:
	if (buf) {
		z_free(buf);
	}
	fs_source_destroy(&src);
	workspace_destroy(&wk);
	return ret;

}

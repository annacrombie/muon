/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <assert.h>
#include <string.h>
#include <sys/stat.h>

#include "coerce.h"
#include "error.h"
#include "install.h"
#include "log.h"
#include "options.h"
#include "platform/os.h"
#include "platform/path.h"

static bool
rwx_to_perm(const char *rwx, uint32_t *perm)
{
	assert(rwx && perm);
	if (strlen(rwx) != 9) {
		return false;
	}

	uint32_t bit = S_IRUSR; // 400
	uint32_t i;
	for (i = 0; i < 9; ++i) {
		switch (rwx[i]) {
		case '-':
			break;
		case 't':
		case 'T':
			if (i != 8) {
				return false;
			}

			if (!S_ISVTX) {
				LOG_W("sticky bit requested, but support is not compiled in");
			}

			*perm |= S_ISVTX;
			break;
		case 's':
		case 'S':
			if (i != 2 && i != 5) {
				return false;
			}
			*perm |= i == 2 ? S_ISUID : S_ISGID;
			break;
		case 'r':
			if (i != 0 && i != 3 && i != 6) {
				return false;
			}
			break;
		case 'w':
			if (i != 1 && i != 4 && i != 7) {
				return false;
			}
			break;
		case 'x':
			if (i != 2 && i != 5 && i != 8) {
				return false;
			}
			break;
		default:
			return false;
		}
		if (rwx[i] != '-' && rwx[i] != 'S' && rwx[i] != 'T') {
			*perm |= bit;
		}
		bit >>= 1; // 400 200 100 40 20 10 4 2 1
	}

	// printf("%o\n", *perm);
	return true;
}

struct obj_install_target *
push_install_target(struct workspace *wk, obj src, obj dest, obj mode)
{
	obj id;
	make_obj(wk, &id, obj_install_target);
	struct obj_install_target *tgt = get_obj_install_target(wk, id);
	tgt->src = src;

	tgt->has_perm = false;
	if (mode) {
		uint32_t len = get_obj_array(wk, mode)->len;
		if (len > 3) {
			LOG_E("install_mode must have 3 or less elements");
			return NULL;
		}

		if (len > 1) {
			LOG_W("TODO: install user/group mode");
		}

		obj perm;
		obj_array_index(wk, mode, 0, &perm);
		switch (get_obj_type(wk, perm)) {
		case obj_bool:
			tgt->has_perm = false;
			break;
		case obj_string:
			if (!rwx_to_perm(get_cstr(wk, perm), &tgt->perm)) {
				LOG_E("install_mode has malformed permission string: %s", get_cstr(wk, perm));
				return NULL;
			}
			tgt->has_perm = true;
			break;
		default:
			return NULL;
		}
	}

	obj sdest;
	if (path_is_absolute(get_cstr(wk, dest))) {
		sdest = dest;
	} else {
		obj prefix;
		get_option_value(wk, current_project(wk), "prefix", &prefix);

		SBUF(buf);
		path_join(wk, &buf, get_cstr(wk, prefix), get_cstr(wk, dest));
		sdest = sbuf_into_str(wk, &buf);
	}

	tgt->dest = sdest;
	tgt->type = install_target_default;

	obj_array_push(wk, wk->install, id);
	return tgt;
}

bool
push_install_target_install_dir(struct workspace *wk, obj src,
	obj install_dir, obj mode)
{
	SBUF(basename);
	path_basename(wk, &basename, get_cstr(wk, src));

	SBUF(dest);
	path_join(wk, &dest, get_cstr(wk, install_dir), basename.buf);
	obj sdest = sbuf_into_str(wk, &dest);

	return !!push_install_target(wk, src, sdest, mode);
}

struct push_install_targets_ctx {
	obj install_dirs;
	obj install_mode;
	bool install_dirs_is_arr, preserve_path;
	uint32_t i, err_node;
};

static enum iteration_result
push_install_targets_iter(struct workspace *wk, void *_ctx, obj val_id)
{
	struct push_install_targets_ctx *ctx = _ctx;

	obj install_dir;

	if (ctx->install_dirs_is_arr) {
		obj_array_index(wk, ctx->install_dirs, ctx->i, &install_dir);
		assert(install_dir);
	} else {
		install_dir = ctx->install_dirs;
	}

	++ctx->i;

	enum obj_type dt = get_obj_type(wk, install_dir);

	if (dt == obj_bool && !get_obj_bool(wk, install_dir)) {
		// skip if we get passed `false` for an install dir
		return ir_cont;
	} else if (dt != obj_string) {
		vm_error_at(wk, ctx->err_node, "install_dir values must be strings, got %s", obj_type_to_s(dt));
		return ir_err;
	}

	obj src, dest, f;

	switch (get_obj_type(wk, val_id)) {
	case obj_string: {
		if (!coerce_file(wk, ctx->err_node, val_id, &f)) {
			return ir_err;
		}

		if (!ctx->preserve_path) {
			goto handle_file;
		}

		SBUF(dest_path);
		path_join(wk, &dest_path, get_cstr(wk, install_dir), get_cstr(wk, val_id));

		src = *get_obj_file(wk, f);
		dest = sbuf_into_str(wk, &dest_path);
		break;
	}
	case obj_file:
		if (ctx->preserve_path) {
			vm_error_at(wk, ctx->err_node, "file arguments are ambiguous with preserve_path: true");
			return ir_err;
		}

		f = val_id;

handle_file:    {
			SBUF(basename);
			path_basename(wk, &basename, get_file_path(wk, f));

			SBUF(dest_path);
			path_join(wk, &dest_path, get_cstr(wk, install_dir), basename.buf);

			src = *get_obj_file(wk, f);
			dest = sbuf_into_str(wk, &dest_path);
		}
		break;
	default:
		UNREACHABLE;
	}

	if (!push_install_target(wk, src, dest, ctx->install_mode)) {
		return ir_err;
	}
	return ir_cont;
}

bool
push_install_targets(struct workspace *wk, uint32_t err_node,
	obj filenames, obj install_dirs, obj install_mode, bool preserve_path)
{
	struct push_install_targets_ctx ctx = {
		.err_node = err_node,
		.preserve_path = preserve_path,
		.install_dirs = install_dirs,
		.install_mode = install_mode,
		.install_dirs_is_arr = get_obj_type(wk, install_dirs) == obj_array,
	};

	assert(ctx.install_dirs_is_arr || get_obj_type(wk, install_dirs) == obj_string);

	if (ctx.install_dirs_is_arr) {
		struct obj_array *a1 = get_obj_array(wk, filenames);
		struct obj_array *a2 = get_obj_array(wk, install_dirs);
		if (a1->len != a2->len) {
			vm_error_at(wk, err_node, "number of install_dirs does not match number of sources");
			return false;
		}
	}

	return obj_array_foreach(wk, filenames, &ctx, push_install_targets_iter);
}


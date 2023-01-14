/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: Ariadne Conill <ariadne@dereferenced.org>
 * SPDX-FileCopyrightText: Masayuki Yamamoto <ma3yuki.8mamo10@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "posix.h"

#include <libpkgconf/libpkgconf.h>
#include <stdlib.h>
#include <string.h>

#include "buf_size.h"
#include "external/libpkgconf.h"
#include "lang/object.h"
#include "lang/workspace.h"
#include "log.h"
#include "options.h"
#include "platform/filesystem.h"
#include "platform/path.h"

const bool have_libpkgconf = true;

static struct {
	pkgconf_client_t client;
	pkgconf_cross_personality_t *personality;
	const int maxdepth;
	bool init;
} pkgconf_ctx = {
	.maxdepth = 200,
};

static bool
error_handler(const char *msg, const pkgconf_client_t *client, void *data)
{
	if (log_should_print(log_debug)) {
		log_plain("dbg libpkgconf: %s", msg);
	}
	return true;
}

static bool
muon_pkgconf_init(struct workspace *wk)
{
	// HACK: TODO: libpkgconf breaks if you try use it after deiniting a
	// client.  Also there are memory leaks abound.
	if (pkgconf_ctx.init) {
		return true;
	}

	pkgconf_ctx.personality = pkgconf_cross_personality_default();
	pkgconf_client_init(&pkgconf_ctx.client, error_handler, NULL, pkgconf_ctx.personality);

	obj opt;
	get_option_value(wk, current_project(wk), "pkg_config_path", &opt);
	const struct str *pkg_config_path = get_str(wk, opt);

#ifdef MUON_STATIC
	if (!pkg_config_path->len && !getenv("PKG_CONFIG_PATH")) {
		LOG_E("Unable to determine pkgconf search path.  Please set "
			"PKG_CONFIG_PATH or -Dpkg_config_path to an appropriate value.");
		return false;
	}
#endif

	if (pkg_config_path->len) {
		pkgconf_path_split(pkg_config_path->s, &pkgconf_ctx.client.dir_list, true);
	} else {
		// pkgconf_client_dir_list_build uses PKG_CONFIG_PATH and
		// PKG_CONFIG_LIBDIR from the environment, as well as the
		// builtin path (personality->dir_list).
		//
		// Leaving this here just in case it ever looks like that is a
		// bad idea.
		// pkgconf_path_copy_list(&client.dir_list, &personality->dir_list);
		pkgconf_client_dir_list_build(&pkgconf_ctx.client, pkgconf_ctx.personality);
	}

	pkgconf_ctx.init = true;
	return true;
}

#if 0
static void
muon_pkgconf_deinit(void)
{
	return;
	pkgconf_path_free(&pkgconf_ctx.personality->dir_list);
	pkgconf_path_free(&pkgconf_ctx.personality->filter_libdirs);
	pkgconf_path_free(&pkgconf_ctx.personality->filter_includedirs);
	pkgconf_client_deinit(&pkgconf_ctx.client);
}
#endif

static const char *
pkgconf_strerr(int err)
{
	switch (err) {
	case PKGCONF_PKG_ERRF_OK:
		return "ok";
	case PKGCONF_PKG_ERRF_PACKAGE_NOT_FOUND:
		return "not found";
	case PKGCONF_PKG_ERRF_PACKAGE_VER_MISMATCH:
		return "ver mismatch";
	case PKGCONF_PKG_ERRF_PACKAGE_CONFLICT:
		return "package conflict";
	case PKGCONF_PKG_ERRF_DEPGRAPH_BREAK:
		return "depgraph break";
	}

	return "unknown";
}

typedef unsigned int (*apply_func)(pkgconf_client_t *client,
	pkgconf_pkg_t *world, pkgconf_list_t *list, int maxdepth);

struct pkgconf_lookup_ctx {
	apply_func apply_func;
	struct workspace *wk;
	struct pkgconf_info *info;
	obj libdirs;
	obj name;
	bool is_static;
};

struct find_lib_path_ctx {
	bool is_static;
	bool found;
	const char *name;
	struct sbuf *buf, *name_buf;
};

static bool
check_lib_path(struct workspace *wk, struct find_lib_path_ctx *ctx, const char *lib_path)
{
	enum ext { ext_a, ext_so, ext_dll_a, ext_count };
	static const char *ext[] = { [ext_a] = ".a", [ext_so] = ".so", [ext_dll_a] = ".dll.a" };
	static const uint8_t ext_order_static[] = { ext_a, ext_so, ext_dll_a },
			     ext_order_dynamic[] = { ext_dll_a, ext_so, ext_a },
			     *ext_order;

	if (ctx->is_static) {
		ext_order = ext_order_static;
	} else {
		ext_order = ext_order_dynamic;
	}

	uint32_t i;

	for (i = 0; i < ext_count; ++i) {
		sbuf_clear(ctx->name_buf);
		sbuf_pushf(wk, ctx->name_buf, "lib%s%s", ctx->name, ext[ext_order[i]]);

		path_join(wk, ctx->buf, lib_path, ctx->name_buf->buf);

		if (fs_file_exists(ctx->buf->buf)) {
			ctx->found = true;
			return true;
		}
	}

	return false;
}

static enum iteration_result
find_lib_path_iter(struct workspace *wk, void *_ctx, obj val_id)
{
	struct find_lib_path_ctx *ctx = _ctx;

	if (check_lib_path(wk, ctx, get_cstr(wk, val_id))) {
		return ir_done;
	}

	return ir_cont;
}

static obj
find_lib_path(pkgconf_client_t *client, struct pkgconf_lookup_ctx *ctx, const char *name)
{
	SBUF(buf);
	SBUF(name_buf);

	struct find_lib_path_ctx find_lib_path_ctx = {
		.buf = &buf,
		.name_buf = &name_buf,
		.name = name,
		.is_static = ctx->is_static
	};

	if (!obj_array_foreach(ctx->wk, ctx->libdirs, &find_lib_path_ctx, find_lib_path_iter)) {
		return 0;
	} else if (!find_lib_path_ctx.found) {
		return 0;
	}

	return sbuf_into_str(ctx->wk, &buf);
}

static bool
apply_and_collect(pkgconf_client_t *client, pkgconf_pkg_t *world, void *_ctx, int maxdepth)
{
	struct pkgconf_lookup_ctx *ctx = _ctx;
	int err;
	pkgconf_node_t *node;
	pkgconf_list_t list = PKGCONF_LIST_INITIALIZER;
	obj str;
	bool ret = true;

	err = ctx->apply_func(client, world, &list, maxdepth);
	if (err != PKGCONF_PKG_ERRF_OK) {
		LOG_E("apply_func failed: %s", pkgconf_strerr(err));
		ret = false;
		goto ret;
	}

	PKGCONF_FOREACH_LIST_ENTRY(list.head, node) {
		const pkgconf_fragment_t *frag = node->data;

		/* L("got option: -'%c' '%s'", frag->type, frag->data); */

		switch (frag->type) {
		case 'I':
			if (!pkgconf_fragment_has_system_dir(client, frag)) {
				make_obj(ctx->wk, &str, obj_include_directory);
				struct obj_include_directory *o = get_obj_include_directory(ctx->wk, str);
				o->path = make_str(ctx->wk, frag->data);
				o->is_system = false;
				obj_array_push(ctx->wk, ctx->info->includes, str);
			}
			break;
		case 'L':
			str = make_str(ctx->wk, frag->data);
			obj_array_push(ctx->wk, ctx->libdirs, str);
			break;
		case 'l': {
			obj path;
			if ((path = find_lib_path(client, ctx, frag->data))) {
				if (!obj_array_in(ctx->wk, ctx->info->libs, str)) {
					L("library '%s' found for dependency '%s'",
						get_cstr(ctx->wk, path), get_cstr(ctx->wk, ctx->name));

					obj_array_push(ctx->wk, ctx->info->libs, path);
				}
			} else {
				LOG_W("library '%s' not found for dependency '%s'", frag->data, get_cstr(ctx->wk, ctx->name));
				obj_array_push(ctx->wk, ctx->info->not_found_libs, make_str(ctx->wk, frag->data));
			}
			break;
		}
		default:
			if (frag->type) {
				obj_array_push(ctx->wk, ctx->info->compile_args,
					make_strf(ctx->wk, "-%c%s", frag->type, frag->data));
			} else {
				L("skipping null pkgconf fragment: '%s'", frag->data);
			}
			break;
		}
	}

ret:
	pkgconf_fragment_free(&list);
	return ret;

}

static bool
apply_modversion(pkgconf_client_t *client, pkgconf_pkg_t *world, void *_ctx, int maxdepth)
{
	struct pkgconf_lookup_ctx *ctx = _ctx;
	pkgconf_dependency_t *dep = world->required.head->data;
	pkgconf_pkg_t *pkg = dep->match;

	if (pkg != NULL && pkg->version != NULL) {
		strncpy(ctx->info->version, pkg->version, MAX_VERSION_LEN);
	}

	return true;
}

bool
muon_pkgconf_lookup(struct workspace *wk, obj name, bool is_static, struct pkgconf_info *info)
{
	if (!pkgconf_ctx.init) {
		if (!muon_pkgconf_init(wk)) {
			return false;
		}
	}

	int flags = 0;

	if (is_static) {
		flags |= (PKGCONF_PKG_PKGF_SEARCH_PRIVATE | PKGCONF_PKG_PKGF_MERGE_PRIVATE_FRAGMENTS);
	}

	pkgconf_client_set_flags(&pkgconf_ctx.client, flags);

	bool ret = true;
	pkgconf_list_t pkgq = PKGCONF_LIST_INITIALIZER;
	pkgconf_queue_push(&pkgq, get_cstr(wk, name));

	struct pkgconf_lookup_ctx ctx = { .wk = wk, .info = info, .name = name, .is_static = is_static };

	if (!pkgconf_queue_apply(&pkgconf_ctx.client, &pkgq, apply_modversion, pkgconf_ctx.maxdepth, &ctx)) {
		ret = false;
		goto ret;
	}

	make_obj(wk, &info->compile_args, obj_array);
	make_obj(wk, &info->link_args, obj_array);
	make_obj(wk, &info->includes, obj_array);
	make_obj(wk, &info->libs, obj_array);
	make_obj(wk, &info->not_found_libs, obj_array);
	make_obj(wk, &ctx.libdirs, obj_array);

	ctx.apply_func = pkgconf_pkg_libs;
	if (!pkgconf_queue_apply(&pkgconf_ctx.client, &pkgq, apply_and_collect, pkgconf_ctx.maxdepth, &ctx)) {
		ret = false;
		goto ret;
	}

	// meson runs pkg-config to look for cflags,
	// which honors Requires.private if any cflags are requested.
	pkgconf_client_set_flags(&pkgconf_ctx.client, flags | PKGCONF_PKG_PKGF_SEARCH_PRIVATE);

	ctx.apply_func = pkgconf_pkg_cflags;
	if (!pkgconf_queue_apply(&pkgconf_ctx.client, &pkgq, apply_and_collect, pkgconf_ctx.maxdepth, &ctx)) {
		ret = false;
		goto ret;
	}

	pkgconf_client_set_flags(&pkgconf_ctx.client, flags);

ret:
	pkgconf_queue_free(&pkgq);
	return ret;
}

struct pkgconf_get_variable_ctx {
	struct workspace *wk;
	const char *var;
	obj *res;
};

static bool
apply_variable(pkgconf_client_t *client, pkgconf_pkg_t *world, void *_ctx, int maxdepth)
{
	struct pkgconf_get_variable_ctx *ctx = _ctx;
	bool found = false;
	const char *var;
	pkgconf_dependency_t *dep = world->required.head->data;
	pkgconf_pkg_t *pkg = dep->match;

	if (pkg != NULL) {
		var = pkgconf_tuple_find(client, &pkg->vars, ctx->var);
		if (var != NULL) {
			*ctx->res = make_str(ctx->wk, var);
			found = true;
		}
	}

	return found;
}

bool
muon_pkgconf_get_variable(struct workspace *wk, const char *pkg_name, const char *var, obj *res)
{
	if (!pkgconf_ctx.init) {
		if (!muon_pkgconf_init(wk)) {
			return false;
		}
	}

	pkgconf_client_set_flags(&pkgconf_ctx.client, PKGCONF_PKG_PKGF_SEARCH_PRIVATE);

	pkgconf_list_t pkgq = PKGCONF_LIST_INITIALIZER;
	pkgconf_queue_push(&pkgq, pkg_name);
	bool ret = true;

	struct pkgconf_get_variable_ctx ctx = { .wk = wk, .res = res, .var = var, };

	if (!pkgconf_queue_apply(&pkgconf_ctx.client, &pkgq, apply_variable, pkgconf_ctx.maxdepth, &ctx)) {
		ret = false;
		goto ret;
	}

ret:
	pkgconf_queue_free(&pkgq);
	return ret;
}

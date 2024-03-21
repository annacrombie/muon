/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "embedded.h"
#include "functions/common.h"
#include "functions/modules.h"
#include "functions/modules/fs.h"
#include "functions/modules/keyval.h"
#include "functions/modules/pkgconfig.h"
#include "functions/modules/python.h"
#include "functions/modules/sourceset.h"
#include "lang/typecheck.h"
#include "log.h"
#include "platform/filesystem.h"

const char *module_names[module_count] = {
	[module_fs] = "fs",
	[module_keyval] = "keyval",
	[module_pkgconfig] = "pkgconfig",
	[module_python3] = "python3",
	[module_python] = "python",
	[module_sourceset] = "sourceset",

	// unimplemented
	[module_cmake] = "cmake",
	[module_dlang] = "dlang",
	[module_gnome] = "gnome",
	[module_hotdoc] = "hotdoc",
	[module_i18n] = "i18n",
	[module_java] = "java",
	[module_modtest] = "modtest",
	[module_qt] = "qt",
	[module_qt4] = "qt4",
	[module_qt5] = "qt5",
	[module_qt6] = "qt6",
	[module_unstable_cuda] = "unstable-cuda",
	[module_unstable_external_project] = "unstable-external_project",
	[module_unstable_icestorm] = "unstable-icestorm",
	[module_unstable_rust] = "unstable-rust",
	[module_unstable_simd] = "unstable-simd",
	[module_unstable_wayland] = "unstable-wayland",
	[module_windows] = "windows",
};

static bool
module_lookup_builtin(const char *name, enum module *res, bool *has_impl)
{
	enum module i;
	for (i = 0; i < module_count; ++i) {
		if (strcmp(name, module_names[i]) == 0) {
			*res = i;
			*has_impl = i < module_unimplemented_separator;
			return true;
		}
	}

	return false;
}

bool
module_import(struct workspace *wk, const char *name, bool encapsulate, obj *res)
{
	struct obj_module *m = 0;
	if (encapsulate) {
		make_obj(wk, res, obj_module);
		m = get_obj_module(wk, *res);
	}

	SBUF(module_src);
	sbuf_pushf(wk, &module_src, "modules/%s.meson", name);

	// script modules
	struct source src = { .label = module_src.buf };
	if ((src.src = embedded_get(src.label))) {
		src.len = strlen(src.src);

		bool ret = false;
		enum language_mode old_language_mode = wk->vm.lang_mode;
		wk->vm.lang_mode = language_extended;

		obj old_scope_stack;
		if (encapsulate) {
			old_scope_stack = current_project(wk)->scope_stack;
			/* current_project(wk)->scope_stack = wk->vm.behavior.scope_stack_dup(wk, wk->default_scope); */
		}

		obj res;
		if (!eval(wk, &src, eval_mode_default, &res)) {
			goto ret;
		}

		if (encapsulate) {
			/* if (!wk->returned) { */
			/* 	vm_error_at(wk, 0, "%s did not return anything", name); */
			/* 	goto ret; */
			/* } else if (!typecheck(wk, 0, wk->returned, make_complex_type(wk, complex_type_nested, tc_dict, tc_func))) { */
			/* 	goto ret; */
			/* } */

			/* m->found = true; */
			/* m->has_impl = true; */
			/* m->exports = wk->returned; */

			/* wk->returning = false; */
		}

		ret = true;
ret:
		if (encapsulate) {
			current_project(wk)->scope_stack = old_scope_stack;
		}
		wk->vm.lang_mode = old_language_mode;
		return ret;
	} else {
		enum module mod_type;
		bool has_impl = false;
		if (module_lookup_builtin(name, &mod_type, &has_impl)) {
			if (!encapsulate) {
				vm_error(wk, "builtin modules cannot be imported into the current scope");
				return false;
			}

			m->module = mod_type;
			m->found = has_impl;
			m->has_impl = has_impl;
			return true;
		}

		return false;
	}
}

static bool
func_module_found(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, get_obj_module(wk, rcvr)->found);
	return true;
}

// clang-format off
struct func_impl_group module_func_impl_groups[module_count][language_mode_count] = {
	[module_fs]        = { { impl_tbl_module_fs },        { impl_tbl_module_fs_internal } },
	[module_keyval]    = { { impl_tbl_module_keyval },    { 0 }                           },
	[module_pkgconfig] = { { impl_tbl_module_pkgconfig }, { 0 }                           },
	[module_python3]   = { { impl_tbl_module_python3 },   { 0 }                           },
	[module_python]    = { { impl_tbl_module_python },    { 0 }                           },
	[module_sourceset] = { { impl_tbl_module_sourceset }, { 0 }                           },
};

const struct func_impl impl_tbl_module[] = {
	{ "found", func_module_found, tc_bool, },
	{ 0 },
};
// clang-format on

bool
module_func_lookup(struct workspace *wk, const char *name, enum module mod, uint32_t *idx)
{
	if (strcmp(name, "found") == 0) {
		return true;
	}

	if (!func_lookup_for_group(module_func_impl_groups[mod], wk->vm.lang_mode, name, idx)) {
		return false;
	}

	return true;
}

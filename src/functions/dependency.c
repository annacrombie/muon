/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: illiliti <illiliti@thunix.net>
 * SPDX-FileCopyrightText: Harley Swick <fancycade@mycanofbeans.com>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "coerce.h"
#include "external/libpkgconf.h"
#include "functions/build_target.h"
#include "functions/common.h"
#include "functions/dependency.h"
#include "functions/kernel/dependency.h"
#include "lang/typecheck.h"
#include "log.h"
#include "platform/path.h"

static bool
func_dependency_found(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res,
		(get_obj_dependency(wk, rcvr)->flags & dep_flag_found) == dep_flag_found);
	return true;
}

static bool
dep_get_pkgconfig_variable(struct workspace *wk, obj dep, uint32_t node, obj var, obj *res)
{
	struct obj_dependency *d = get_obj_dependency(wk, dep);
	if (d->type != dependency_type_pkgconf) {
		vm_error_at(wk, node, "dependency not from pkgconf");
		return false;
	}

	if (!muon_pkgconf_get_variable(wk, get_cstr(wk, d->name), get_cstr(wk, var), res)) {
		return false;
	}
	return true;
}

static bool
func_dependency_get_pkgconfig_variable(struct workspace *wk, obj rcvr,
	uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_default,
	};
	struct args_kw akw[] = {
		[kw_default] = { "default", obj_string },
		0
	};
	if (!pop_args(wk, an, akw)) {
		return false;
	}

	if (!dep_get_pkgconfig_variable(wk, rcvr, an[0].node, an[0].val, res)) {
		if (akw[kw_default].set) {
			*res = akw[kw_default].val;
		} else {
			vm_error_at(wk, an[0].node, "undefined pkg_config variable");
			return false;
		}
	}

	return true;
}

static bool
dep_pkgconfig_define(struct workspace *wk, obj dep, uint32_t node, obj var)
{
	struct obj_array *array = get_obj_array(wk, var);
	uint32_t arraylen = array->len;
	if (arraylen % 2 != 0) {
		vm_error_at(wk, node, "non-even number of arguments in list");
		return false;
	}

	for (int64_t idx = 0; idx < arraylen; idx += 2) {
		obj key, val;
		obj_array_index(wk, var, idx, &key);
		obj_array_index(wk, var, idx + 1, &val);

		const char *ckey = get_cstr(wk, key);
		const char *cval = get_cstr(wk, val);
		if (!muon_pkgconf_define(wk, ckey, cval)) {
			vm_error_at(wk, node, "error setting %s=%s", ckey, cval);
			return false;
		}
	}

	return true;
}

static bool
func_dependency_get_variable(struct workspace *wk, obj rcvr,
	uint32_t args_node, obj *res)
{
	struct args_norm ao[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_pkgconfig,
		kw_pkgconfig_define,
		kw_internal,
		kw_default_value,
	};
	struct args_kw akw[] = {
		[kw_pkgconfig] = { "pkgconfig", obj_string },
		[kw_pkgconfig_define] = { "pkgconfig_define", TYPE_TAG_LISTIFY | obj_string },
		[kw_internal] = { "internal", obj_string },
		[kw_default_value] = { "default_value", obj_string },
		0
	};
	if (!pop_args(wk, NULL, akw)) {
		return false;
	}

	uint32_t node = args_node;

	if (ao[0].set) {
		node = ao[0].node;

		if (!akw[kw_pkgconfig].set) {
			akw[kw_pkgconfig].set = true;
			akw[kw_pkgconfig].node = ao[0].node;
			akw[kw_pkgconfig].val = ao[0].val;
		}

		if (!akw[kw_internal].set) {
			akw[kw_internal].set = true;
			akw[kw_internal].node = ao[0].node;
			akw[kw_internal].val = ao[0].val;
		}
	}

	struct obj_dependency *dep = get_obj_dependency(wk, rcvr);
	if (dep->type == dependency_type_pkgconf) {
		if (akw[kw_pkgconfig_define].set) {
			node = akw[kw_pkgconfig_define].node;

			if (!dep_pkgconfig_define(wk, rcvr, node, akw[kw_pkgconfig_define].val)) {
				return false;
			}
		}
		if (akw[kw_pkgconfig].set) {
			node = akw[kw_pkgconfig].node;

			if (dep_get_pkgconfig_variable(wk, rcvr, akw[kw_pkgconfig].node, akw[kw_pkgconfig].val, res)) {
				return true;
			}
		}
	} else if (dep->variables) {
		if (akw[kw_internal].set) {
			node = akw[kw_internal].node;

			if (obj_dict_index(wk, dep->variables, akw[kw_internal].val, res)) {
				return true;
			}
		}
	}

	if (akw[kw_default_value].set) {
		*res = akw[kw_default_value].val;
		return true;
	} else {
		vm_error_at(wk, node, "pkgconfig file has no such variable");
		return false;
	}
}

static bool
func_dependency_version(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	obj version = get_obj_dependency(wk, rcvr)->version;

	if (version) {
		*res = version;
	} else {
		*res = make_str(wk, "unknown");
	}

	return true;
}

static bool
func_dependency_type_name(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	struct obj_dependency *dep = get_obj_dependency(wk, rcvr);

	if (!(dep->flags & dep_flag_found)) {
		*res = make_str(wk, "not-found");
		return true;
	}

	const char *n = NULL;
	switch (dep->type) {
	case dependency_type_pkgconf:
		n = "pkgconfig";
		break;
	case dependency_type_declared:
		n = "internal";
		break;
	case dependency_type_appleframeworks:
	case dependency_type_threads:
		n = "system";
		break;
	case dependency_type_external_library:
		n = "library";
		break;
	case dependency_type_not_found:
		n = "not-found";
		break;
	}

	*res = make_str(wk, n);
	return true;
}

static bool
func_dependency_name(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	struct obj_dependency *dep = get_obj_dependency(wk, rcvr);

	if (dep->type == dependency_type_declared) {
		*res = make_str(wk, "internal");
	} else {
		*res = dep->name;
	}

	return true;
}

static bool
func_dependency_partial_dependency(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	enum kwargs {
		kw_compile_args,
		kw_includes,
		kw_link_args,
		kw_links,
		kw_sources,
	};
	struct args_kw akw[] = {
		[kw_compile_args] = { "compile_args", obj_bool },
		[kw_includes] = { "includes", obj_bool },
		[kw_link_args] = { "link_args", obj_bool },
		[kw_links] = { "links", obj_bool },
		[kw_sources] = { "sources", obj_bool },
		0
	};
	if (!pop_args(wk, NULL, akw)) {
		return false;
	}

	make_obj(wk, res, obj_dependency);
	struct obj_dependency *dep = get_obj_dependency(wk, rcvr),
			      *partial = get_obj_dependency(wk, *res);

	*partial = *dep;
	partial->dep = (struct build_dep){ 0 };

	if (akw[kw_compile_args].set && get_obj_bool(wk, akw[kw_compile_args].val)) {
		partial->dep.compile_args = dep->dep.compile_args;
	}

	if (akw[kw_includes].set && get_obj_bool(wk, akw[kw_includes].val)) {
		partial->dep.include_directories = dep->dep.include_directories;
	}

	if (akw[kw_link_args].set && get_obj_bool(wk, akw[kw_link_args].val)) {
		partial->dep.link_args = dep->dep.link_args;
	}

	if (akw[kw_links].set && get_obj_bool(wk, akw[kw_links].val)) {
		partial->dep.link_with = dep->dep.link_with;
		partial->dep.link_whole = dep->dep.link_whole;
		partial->dep.link_with_not_found = dep->dep.link_with_not_found;

		partial->dep.raw.link_with = dep->dep.raw.link_with;
		partial->dep.raw.link_whole = dep->dep.raw.link_whole;
	}

	if (akw[kw_sources].set && get_obj_bool(wk, akw[kw_sources].val)) {
		partial->dep.sources = dep->dep.sources;
	}
	return true;
}

static bool
func_dependency_as_system(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm ao[] = { { obj_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	enum include_type inc_type = include_type_system;
	if (ao[0].set) {
		if (!coerce_include_type(wk, get_str(wk, ao[0].val), ao[0].node, &inc_type)) {
			return false;
		}
	}

	make_obj(wk, res, obj_dependency);

	struct obj_dependency *dep = get_obj_dependency(wk, *res);
	*dep = *get_obj_dependency(wk, rcvr);

	obj old_includes = dep->dep.include_directories;
	make_obj(wk, &dep->dep.include_directories, obj_array);

	dep_process_includes(wk, old_includes, inc_type,
		dep->dep.include_directories);
	dep->include_type = inc_type;

	return true;
}

static bool
func_dependency_include_type(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	const char *s = NULL;
	switch (get_obj_dependency(wk, rcvr)->include_type) {
	case include_type_preserve:
		s = "preserve";
		break;
	case include_type_system:
		s = "system";
		break;
	case include_type_non_system:
		s = "non-system";
		break;
	default:
		assert(false && "unreachable");
		break;
	}

	*res = make_str(wk, s);
	return true;
}

const struct func_impl impl_tbl_dependency[] = {
	{ "as_system", func_dependency_as_system, tc_dependency },
	{ "found", func_dependency_found, tc_bool },
	{ "get_pkgconfig_variable", func_dependency_get_pkgconfig_variable, tc_string },
	{ "get_variable", func_dependency_get_variable, tc_string },
	{ "include_type", func_dependency_include_type, tc_string },
	{ "partial_dependency", func_dependency_partial_dependency, tc_dependency },
	{ "type_name", func_dependency_type_name, tc_string },
	{ "name", func_dependency_name, tc_string },
	{ "version", func_dependency_version, tc_string },
	{ NULL, NULL },
};

/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_FUNCTIONS_COMMON_H
#define MUON_FUNCTIONS_COMMON_H

#include "lang/workspace.h"

typedef bool (*func_impl)(struct workspace *wk, obj self, obj *res);
typedef obj (*func_impl_self_transform)(struct workspace *wk, obj self);

struct func_impl {
	const char *name;
	func_impl func;
	type_tag return_type;
	bool pure, fuzz_unsafe, extension;
	func_impl_self_transform self_transform;
};

struct func_impl_group {
	const struct func_impl *impls;
	uint32_t off, len;
};

extern struct func_impl native_funcs[];

extern bool disabler_among_args_immunity, disable_fuzz_unsafe_functions;

void build_func_impl_tables(void);

bool func_lookup(struct workspace *wk, obj self, const char *name, uint32_t *idx, obj *func);
bool func_lookup_for_group(const struct func_impl_group impl_group[],
	enum language_mode mode,
	const char *name,
	uint32_t *idx);

bool interp_args(struct workspace *wk,
	uint32_t args_node,
	struct args_norm positional_args[],
	struct args_norm optional_positional_args[],
	struct args_kw keyword_args[]);
bool builtin_run(struct workspace *wk, bool have_self, obj self_id, uint32_t node_id, obj *res);

/* bool func_obj_call(struct workspace *wk, struct obj_func *f, obj args, obj *res); */
/* bool func_obj_eval(struct workspace *wk, obj func_obj, obj func_module, uint32_t args_node, obj *res); */
bool analyze_function(struct workspace *wk,
	const struct func_impl *fi,
	uint32_t args_node,
	obj self,
	obj *res,
	bool *was_pure);

void dump_function_signatures(struct workspace *wk);
#endif

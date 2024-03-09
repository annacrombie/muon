/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_FUNCTIONS_COMMON_H
#define MUON_FUNCTIONS_COMMON_H

#include "lang/workspace.h"

typedef bool (*func_impl)(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res);
typedef obj (*func_impl_rcvr_transform)(struct workspace *wk, obj rcvr);

struct func_impl {
	const char *name;
	func_impl func;
	type_tag return_type;
	bool pure, fuzz_unsafe, extension;
	func_impl_rcvr_transform rcvr_transform;
};

extern const struct func_impl *kernel_func_tbl[language_mode_count];
extern const struct func_impl *func_tbl[obj_type_count][language_mode_count];

struct args_norm {
	type_tag type;
	obj val, node;
	bool set;
};

struct args_kw {
	const char *key;
	type_tag type;
	obj val, node;
	bool set;
	bool required;
};

extern bool disabler_among_args_immunity, disable_fuzz_unsafe_functions;

void build_func_impl_tables(void);

const struct func_impl *func_lookup(const struct func_impl **impl_tbl, enum language_mode mode, const char *name);

bool interp_args(struct workspace *wk, uint32_t args_node,
	struct args_norm positional_args[],
	struct args_norm optional_positional_args[],
	struct args_kw keyword_args[]);
bool builtin_run(struct workspace *wk, bool have_rcvr, obj rcvr_id, uint32_t node_id, obj *res);

bool func_obj_call(struct workspace *wk, struct obj_func *f, obj args, obj *res);
bool func_obj_eval(struct workspace *wk, obj func_obj, obj func_module, uint32_t args_node, obj *res);
bool analyze_function(struct workspace *wk, const struct func_impl *fi, uint32_t args_node, obj rcvr, obj *res, bool *was_pure);

void dump_function_signatures(struct workspace *wk);
#endif

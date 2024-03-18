/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "buf_size.h"
#include "functions/array.h"
#include "functions/boolean.h"
#include "functions/both_libs.h"
#include "functions/build_target.h"
#include "functions/common.h"
#include "functions/compiler.h"
#include "functions/configuration_data.h"
#include "functions/custom_target.h"
#include "functions/dependency.h"
#include "functions/dict.h"
#include "functions/disabler.h"
#include "functions/environment.h"
#include "functions/external_program.h"
#include "functions/feature_opt.h"
#include "functions/file.h"
#include "functions/generator.h"
#include "functions/kernel.h"
#include "functions/machine.h"
#include "functions/meson.h"
#include "functions/modules.h"
#include "functions/modules/python.h"
#include "functions/number.h"
#include "functions/run_result.h"
#include "functions/source_configuration.h"
#include "functions/source_set.h"
#include "functions/string.h"
#include "functions/subproject.h"
#include "lang/typecheck.h"
#include "log.h"
#include "platform/filesystem.h"
#include "platform/mem.h"
#include "tracy.h"

// When true, disable functions with the .fuzz_unsafe attribute set to true.
// This is useful when running `muon internal eval` on randomly generated
// files, where you don't want to accidentally execute `run_command('rm',
// '-rf', '/')` for example
bool disable_fuzz_unsafe_functions = false;

// HACK: this is pretty terrible, but the least intrusive way to handle
// disablers in function arguments as they are currently implemented.  When
// interp_args sees a disabler, it sets this flag, and "fails".  In the
// function error handler we check this flag and don't raise an error if it is
// set but instead return disabler.
/* static bool disabler_among_args = false; */
// HACK: we also need this for the is_disabler() function :(
bool disabler_among_args_immunity = false;

// HACK: This works like disabler_among_args kind of.  These opts should only
// ever be set by analyze_function().
/* static struct analyze_function_opts { */
/* 	bool do_analyze; */
/* 	bool pure_function; */
/* 	bool encountered_error; */
/* 	bool allow_impure_args, allow_impure_args_except_first; // set to true for set_variable and subdir */

/* 	bool dump_signature; // used when dumping funciton signatures */
/* } analyze_function_opts; */

#if 0
static bool
interp_args_interp_node(struct workspace *wk, uint32_t arg_node, obj *res)
{
	bool was_immune = disabler_among_args_immunity;
	disabler_among_args_immunity = false;

	if (!wk->interp_node(wk, arg_node, res)) {
		return false;
	}

	disabler_among_args_immunity = was_immune;
	return true;
}

static bool
next_arg(struct workspace *wk, uint32_t *arg_node, uint32_t *kwarg_node, const char **kw, struct node **args)
{
	if (!*args || (*args)->type == node_empty) {
		return false;
	}

	assert((*args)->type == node_argument);

	if ((*args)->subtype == arg_kwarg) {
		*kw = get_cstr(wk, get_node(wk->ast, (*args)->l)->data.str);
		*kwarg_node = (*args)->l;
		*arg_node = (*args)->r;
	} else {
		*kw = NULL;
		*arg_node = (*args)->l;
	}

	/* L("got arg %s:%s", *kw, node_to_s(*arg)); */

	if ((*args)->chflg & node_child_c) {
		*args = get_node(wk->ast, (*args)->c);
	} else {
		*args = NULL;
	}

	return true;
}

struct function_signature {
	const char *name,
		   *posargs,
		   *varargs,
		   *optargs,
		   *kwargs,
		   *returns;
	bool is_method;

	const struct func_impl *impl;
};

struct {
	struct arr sigs;
} function_sig_dump;

static const char *
dump_type(struct workspace *wk, type_tag type)
{
	obj types = typechecking_type_to_arr(wk, type);
	obj typestr, sep = make_str(wk, "|");
	obj_array_join(wk, false, types, sep, &typestr);

	if (type & TYPE_TAG_LISTIFY) {
		obj_array_push(wk, types, make_strf(wk, "list[%s]", get_cstr(wk, typestr)));
		obj sorted;
		obj_array_sort(wk, NULL, types, obj_array_sort_by_str, &sorted);
		obj_array_join(wk, false, sorted, sep, &typestr);
	}

	return get_cstr(wk, typestr);
}

static int32_t
arr_sort_by_string(const void *a, const void *b, void *_ctx)
{
	return strcmp(*(const char **)a, *(const char **)b);
}

static void
dump_function_signature(struct workspace *wk,
	struct args_norm posargs[],
	struct args_norm optargs[],
	struct args_kw kwargs[])
{
	uint32_t i;

	struct function_signature *sig = arr_get(&function_sig_dump.sigs, function_sig_dump.sigs.len - 1);

	obj s;
	if (posargs) {
		s = make_str(wk, "");
		for (i = 0; posargs[i].type != ARG_TYPE_NULL; ++i) {
			if (posargs[i].type & TYPE_TAG_GLOB) {
				sig->varargs = get_cstr(wk, make_strf(wk, "    %s\n", dump_type(wk, posargs[i].type)));
				continue;
			}

			str_appf(wk, &s, "    %s\n", dump_type(wk, posargs[i].type));
		}

		const char *ts = get_cstr(wk, s);
		if (*ts) {
			sig->posargs = ts;
		}
	}

	if (optargs) {
		s = make_str(wk, "");
		for (i = 0; optargs[i].type != ARG_TYPE_NULL; ++i) {
			str_appf(wk, &s, "    %s\n", dump_type(wk, optargs[i].type));
		}
		sig->optargs = get_cstr(wk, s);
	}

	if (kwargs) {
		struct arr kwargs_list;
		arr_init(&kwargs_list, 8, sizeof(char *));

		for (i = 0; kwargs[i].key; ++i) {
			const char *v = get_cstr(wk, make_strf(wk, "    %s: %s\n", kwargs[i].key, dump_type(wk, kwargs[i].type)));
			arr_push(&kwargs_list, &v);
		}

		arr_sort(&kwargs_list, NULL, arr_sort_by_string);

		s = make_str(wk, "");
		for (i = 0; i < kwargs_list.len; ++i) {
			str_app(wk, &s, *(const char **)arr_get(&kwargs_list, i));

		}
		sig->kwargs = get_cstr(wk, s);

		arr_destroy(&kwargs_list);
	}
}

static const char *
arity_to_s(struct args_norm positional_args[],
	struct args_norm optional_positional_args[],
	struct args_kw keyword_args[])
{
	static char buf[BUF_SIZE_2k + 1] = { 0 };

	uint32_t i, bufi = 0;

	bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, "(signature: ");

	if (positional_args) {
		bool glob = false;

		for (i = 0; positional_args[i].type != ARG_TYPE_NULL; ++i) {
			if (positional_args[i].type & TYPE_TAG_GLOB) {
				glob = true;
				break;
			}
		}

		if (i) {
			bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, "%d positional", i);
		}

		if (glob) {
			if (i) {
				bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, ", ");
			}
			bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, "varargs");
		}
	}

	if (optional_positional_args) {
		for (i = 0; optional_positional_args[i].type != ARG_TYPE_NULL; ++i) {
		}

		if (positional_args) {
			bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, ", ");
		}
		bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, "%d optional", i);
	}

	if (keyword_args) {
		for (i = 0; keyword_args[i].key; ++i) {
		}

		if (positional_args || optional_positional_args) {
			bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, ", ");
		}
		bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, "%d keyword", i);
	}

	if (!positional_args && !optional_positional_args && !keyword_args) {
		bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, "0 arguments");
	}

	bufi += snprintf(&buf[bufi], BUF_SIZE_2k - bufi, ")");

	return buf;
}

struct typecheck_function_arg_ctx {
	uint32_t err_node;
	obj arr;
	type_tag type;
};

static enum iteration_result
typecheck_function_arg_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct typecheck_function_arg_ctx *ctx = _ctx;

	if (!typecheck(wk, ctx->err_node, val, ctx->type)) {
		return ir_err;
	}

	obj_array_push(wk, ctx->arr, val);

	return ir_cont;
}

static enum iteration_result
typecheck_function_arg_check_disabler_iter(struct workspace *wk, void *_ctx, obj val)
{
	bool *among = _ctx;

	if (val == disabler_id) {
		*among = true;
		return ir_done;
	}
	return ir_cont;
}

static bool
typecheck_function_arg(struct workspace *wk, uint32_t err_node, obj *val, type_tag type)
{
	if (!disabler_among_args_immunity) {
		if (*val == disabler_id) {
			disabler_among_args = true;
			return false;
		} else if (get_obj_type(wk, *val) == obj_array) {
			bool among = false;
			obj_array_foreach_flat(wk, *val, &among, typecheck_function_arg_check_disabler_iter);
			if (among) {
				disabler_among_args = true;
				return false;
			}
		}
	}

	bool array_of = false;
	if (type & TYPE_TAG_LISTIFY) {
		array_of = true;
		type &= ~TYPE_TAG_LISTIFY;
	}

	assert((type & obj_typechecking_type_tag) || type < obj_type_count);

	// If obj_file or tc_file is requested, and the argument is an array of
	// length 1, try to unpack it.
	if (!array_of && (type == obj_file || (type & tc_file) == tc_file)) {
		if (get_obj_type(wk, *val) == obj_array
		    && get_obj_array(wk, *val)->len == 1) {
			obj i0;
			obj_array_index(wk, *val, 0, &i0);
			if (get_obj_type(wk, i0) == obj_file) {
				*val = i0;
			}
		} else if (get_obj_type(wk, *val) == obj_typeinfo
			   && (get_obj_typeinfo(wk, *val)->type & tc_array) == tc_array) {
			return true;
		}
	}

	if (!array_of) {
		return typecheck(wk, err_node, *val, type);
	}

	struct typecheck_function_arg_ctx ctx = {
		.err_node = err_node,
		.type = type,
	};
	make_obj(wk, &ctx.arr, obj_array);

	if (get_obj_type(wk, *val) == obj_array) {
		if (!obj_array_foreach_flat(wk, *val, &ctx, typecheck_function_arg_iter)) {
			return false;
		}
	} else if (get_obj_type(wk, *val) == obj_typeinfo
		   && (get_obj_typeinfo(wk, *val)->type & tc_array) == tc_array) {
		return true;
	} else {
		if (!typecheck_function_arg_iter(wk, &ctx, *val)) {
			return false;
		}
	}

	*val = ctx.arr;
	return true;
}

#define ARITY arity_to_s(positional_args, optional_positional_args, keyword_args)

static bool
process_kwarg(struct workspace *wk, uint32_t kwarg_node, uint32_t arg_node, struct args_kw *keyword_args, const char *kw, obj val)
{
	uint32_t i;
	for (i = 0; keyword_args[i].key; ++i) {
		if (strcmp(kw, keyword_args[i].key) == 0) {
			break;
		}
	}

	if (!keyword_args[i].key) {
		vm_error_at(wk, kwarg_node, "invalid kwarg: '%s'", kw);
		return false;
	}

	if (!typecheck_function_arg(wk, arg_node, &val, keyword_args[i].type)) {
		return false;
	} else if (keyword_args[i].set) {
		vm_error_at(wk, arg_node, "keyword argument '%s' set twice", keyword_args[i].key);
		return false;
	}

	keyword_args[i].val = val;
	keyword_args[i].node = kwarg_node;
	keyword_args[i].set = true;

	return true;
}

struct process_kwarg_dict_ctx {
	uint32_t kwarg_node;
	uint32_t arg_node;
	struct args_kw *keyword_args;
};

static enum iteration_result
process_kwarg_dict_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	struct process_kwarg_dict_ctx *ctx = _ctx;

	if (!process_kwarg(wk, ctx->kwarg_node, ctx->arg_node, ctx->keyword_args, get_cstr(wk, key), val)) {
		return ir_err;
	}

	return ir_cont;
}

struct obj_tainted_by_typeinfo_ctx {
	bool allow_tainted_dict_values;
};

static bool obj_tainted_by_typeinfo(struct workspace *wk, obj o, struct obj_tainted_by_typeinfo_ctx *ctx);

static enum iteration_result
obj_tainted_by_typeinfo_dict_iter(struct workspace *wk, void *_ctx, obj k, obj v)
{
	struct obj_tainted_by_typeinfo_ctx *ctx = _ctx;
	if (obj_tainted_by_typeinfo(wk, k, 0)) {
		return ir_err;
	}

	if (ctx && !ctx->allow_tainted_dict_values && obj_tainted_by_typeinfo(wk, v, 0)) {
		return ir_err;
	}

	return ir_cont;
}

static enum iteration_result
obj_tainted_by_typeinfo_array_iter(struct workspace *wk, void *_ctx, obj v)
{
	if (obj_tainted_by_typeinfo(wk, v, _ctx)) {
		return ir_err;
	}

	return ir_cont;
}

static bool
obj_tainted_by_typeinfo(struct workspace *wk, obj o, struct obj_tainted_by_typeinfo_ctx *ctx)
{
	if (!o) {
		return true;
	}

	switch (get_obj_type(wk, o)) {
	case obj_typeinfo:
		return true;
	case obj_array:
		return !obj_array_foreach(wk, o, ctx, obj_tainted_by_typeinfo_array_iter);
	case obj_dict:
		return !obj_dict_foreach(wk, o, ctx, obj_tainted_by_typeinfo_dict_iter);
	default:
		return false;
	}
}

bool
interp_args(struct workspace *wk, uint32_t args_node,
	struct args_norm positional_args[],
	struct args_norm optional_positional_args[],
	struct args_kw keyword_args[])
{
	if (analyze_function_opts.dump_signature) {
		dump_function_signature(wk, positional_args, optional_positional_args, keyword_args);
		return false;
	}

	const char *kw;
	uint32_t arg_node, kwarg_node;
	uint32_t i, stage;
	struct args_norm *an[2] = { positional_args, optional_positional_args };
	struct node *args = get_node(wk->ast, args_node);
	bool got_dynamic_kwargs_typeinfo = false;

	for (stage = 0; stage < 2; ++stage) {
		if (!an[stage]) {
			continue;
		}

		for (i = 0; an[stage][i].type != ARG_TYPE_NULL; ++i) {
			if (an[stage][i].type & TYPE_TAG_GLOB) {
				assert(stage == 0 && "glob args must not be optional");
				assert(!optional_positional_args && "glob args cannot be followed by optional args");
				assert(an[stage][i + 1].type == ARG_TYPE_NULL && "glob args must come last");
				assert(!(an[stage][i].type & TYPE_TAG_LISTIFY) && "glob args are implicitly TYPE_TAG_LISTIFY");

				an[stage][i].type &= ~TYPE_TAG_GLOB;

				bool set_arg_node = false;

				make_obj(wk, &an[stage][i].val, obj_array);
				an[stage][i].set = true;

				while (next_arg(wk, &arg_node, &kwarg_node, &kw, &args)) {
					if (kw) {
						goto kwargs;
					}

					if (!set_arg_node) {
						an[stage][i].node = arg_node;
						set_arg_node = true;
					}

					obj val;
					if (!interp_args_interp_node(wk, arg_node, &val)) {
						return false;
					}

					// If we get an array, but that isn't a valid type here, flatten it.
					if ((get_obj_type(wk, val) == obj_array
					     || (get_obj_type(wk, val) == obj_typeinfo
						 && (get_obj_typeinfo(wk, val)->type & tc_array) == tc_array))
					    && !(an[stage][i].type == tc_any
						 || an[stage][i].type == obj_array
						 || (an[stage][i].type & tc_array) == tc_array)
					    ) {
						if (get_obj_type(wk, val) == obj_typeinfo) {
							// TODO typecheck subtype
							obj_array_push(wk, an[stage][i].val, val);
						} else {
							if (!typecheck_function_arg(wk, arg_node, &val, TYPE_TAG_LISTIFY | an[stage][i].type)) {
								return false;
							}

							obj_array_extend_nodup(wk, an[stage][i].val, val);
						}
					} else {
						if (!typecheck_function_arg(wk, arg_node, &val, an[stage][i].type)) {
							return false;
						}
						obj_array_push(wk, an[stage][i].val, val);
					}
				}

				if (!set_arg_node) {
					an[stage][i].node = args_node;
				}
				continue;
			}

			if (!next_arg(wk, &arg_node, &kwarg_node, &kw, &args)) {
				if (stage == 0) { // required
					vm_error_at(wk, args_node, "missing arguments %s", ARITY);
					return false;
				} else if (stage == 1) { // optional
					goto end;
				}
			}

			if (kw) {
				if (stage == 0) {
					vm_error_at(wk, kwarg_node, "unexpected kwarg before required arguments %s", ARITY);
					return false;
				}

				goto kwargs;
			}

			if (!interp_args_interp_node(wk, arg_node, &an[stage][i].val)) {
				return false;
			}

			if (!typecheck_function_arg(wk, arg_node, &an[stage][i].val, an[stage][i].type)) {
				return false;
			}

			an[stage][i].node = arg_node;
			an[stage][i].set = true;
		}
	}

	if (keyword_args) {
		while (next_arg(wk, &arg_node, &kwarg_node, &kw, &args)) {
			goto process_kwarg;
kwargs:
			if (!keyword_args) {
				vm_error_at(wk, args_node, "this function does not accept kwargs %s", ARITY);
				return false;
			}
process_kwarg:
			if (!kw) {
				vm_error_at(wk, arg_node, "non-kwarg after kwargs %s", ARITY);
				return false;
			}

			obj val;
			if (!interp_args_interp_node(wk, arg_node, &val)) {
				return false;
			}

			if (strcmp(kw, "kwargs") == 0) {
				if (!typecheck(wk, arg_node, val, obj_dict)) {
					return false;
				}

				struct process_kwarg_dict_ctx ctx = {
					.kwarg_node = kwarg_node,
					.arg_node = arg_node,
					.keyword_args = keyword_args
				};

				if (get_obj_type(wk, val) == obj_typeinfo) {
					got_dynamic_kwargs_typeinfo = true;
				} else {
					if (!obj_dict_foreach(wk, val, &ctx, process_kwarg_dict_iter)) {
						return false;
					}
				}
			} else {
				if (!process_kwarg(wk, kwarg_node, arg_node, keyword_args, kw, val)) {
					return false;
				}
			}
		}
	} else if (next_arg(wk, &arg_node, &kwarg_node, &kw, &args)) {
		if (kw) {
			vm_error_at(wk, kwarg_node, "this function does not accept kwargs %s", ARITY);
		} else {
			vm_error_at(wk, arg_node, "too many arguments %s", ARITY);
		}

		return false;
	}

end:
	if (keyword_args && !got_dynamic_kwargs_typeinfo) {
		for (i = 0; keyword_args[i].key; ++i) {
			if (keyword_args[i].required && !keyword_args[i].set) {
				vm_error_at(wk, args_node, "missing required kwarg: %s", keyword_args[i].key);
				return false;
			}
		}
	}

	if (analyze_function_opts.do_analyze) {
		bool typeinfo_among_args = false;

		for (stage = 0; stage < 2; ++stage) {
			if (!an[stage]) {
				continue;
			}

			for (i = 0; an[stage][i].type != ARG_TYPE_NULL; ++i) {
				if (!an[stage][i].set) {
					continue;
				}

				if (analyze_function_opts.allow_impure_args) {
					continue;
				} else if (analyze_function_opts.allow_impure_args_except_first && ((stage == 0 && i > 0) || stage > 0)) {
					continue;
				}

				if (obj_tainted_by_typeinfo(wk, an[stage][i].val, 0)) {
					typeinfo_among_args = true;
					break;
				}
			}
		}

		if (!typeinfo_among_args && keyword_args) {
			for (i = 0; keyword_args[i].key; ++i) {
				if (!keyword_args[i].set) {
					continue;
				}

				if (analyze_function_opts.allow_impure_args || analyze_function_opts.allow_impure_args_except_first) {
					continue;
				}

				if (obj_tainted_by_typeinfo(wk, keyword_args[i].val, 0)) {
					typeinfo_among_args = true;
					break;
				}
			}
		}

		if (typeinfo_among_args) {
			analyze_function_opts.pure_function = false;
		}

		if (analyze_function_opts.pure_function) {
			return true;
		}

		analyze_function_opts.encountered_error = false;
		// if we are analyzing arguments only return false to halt the
		// function
		return false;
	}

	return true;
}
#endif

#if 0
bool
func_obj_call(struct workspace *wk, struct obj_func *f, obj args, obj *res)
{
	bool ret = false;
	struct project *proj = current_project(wk);
	struct source src = {
		.label = get_cstr(wk, f->src),
		.reopen_type = source_reopen_type_embedded
	};

	// push current interpreter state
	struct source *old_src = wk->src;
	struct ast *old_ast = wk->ast;
	enum language_mode old_lang_mode = wk->lang_mode;
	obj old_scope_stack = proj->scope_stack;

	// set new state
	wk->src = &src;
	wk->ast = f->ast;
	wk->lang_mode = f->lang_mode;
	proj->scope_stack = f->scope_stack;

	++wk->func_depth;

	// setup current scope
	wk->push_local_scope(wk);

	{ // assign arg values
		uint32_t i = 0;
		struct node *arg = bucket_arr_get(&f->ast->nodes, f->args_id);
		while (arg->type != node_empty) {
			obj val = 0;
			obj_array_index(wk, args, i, &val);
			if (!val && i >= f->nargs) {
				obj_array_index(wk, f->kwarg_defaults, i - f->nargs, &val);
			}

			struct node *arg_name = bucket_arr_get(&f->ast->nodes, arg->l);
			wk->assign_variable(wk, get_cstr(wk, arg_name->data.str), val, arg->l, assign_local);
			++i;

			if (!(arg->chflg & node_child_c)) {
				break;
			}

			arg = bucket_arr_get(&f->ast->nodes, arg->c);
		}
	}

	obj _;
	wk->returning = false;
	wk->returned = 0;
	// call block
	ret = wk->interp_node(wk, f->block_id, &_);
	*res = wk->returned;
	/* LO("%s returned %o\n", f->name, wk->returned); */
	wk->returning = false;
	wk->returned = 0;

	if (ret && !typecheck_custom(wk, wk->return_node, *res, f->return_type,
		"function returned invalid type, expected %s, got %s")) {
		ret = false;
	}

	// cleanup and return
	if (old_ast) {
		// pop old interpreter state
		wk->ast = old_ast;
		wk->src = old_src;
		wk->lang_mode = old_lang_mode;
		wk->pop_local_scope(wk);
		proj->scope_stack = old_scope_stack;
		--wk->func_depth;
	}

	return ret;
}

bool
func_obj_eval(struct workspace *wk, obj func_obj, obj func_module, uint32_t args_node, obj *res)
{
	struct obj_func *f = get_obj_func(wk, func_obj);

	obj args = 0;
	if (f->nargs || f->nkwargs) {
		make_obj(wk, &args, obj_array);
	}

	static struct args_norm an[64];
	assert(f->nargs + 1 < ARRAY_LEN(an));
	static struct args_kw akw[128];
	assert(f->nkwargs + 1 < ARRAY_LEN(akw));

	// init and interp args
	memset(an, 0, sizeof(struct args_norm) * (f->nargs + 1));
	an[f->nargs].type = ARG_TYPE_NULL;
	memset(akw, 0, sizeof(struct args_kw) * (f->nkwargs + 1));
	akw[f->nkwargs].type = ARG_TYPE_NULL;

	uint32_t pos_i = 0, kw_i = 0, arg_id = f->args_id;
	while (true) {
		struct node *arg = bucket_arr_get(&f->ast->nodes, arg_id);
		if (arg->type == node_empty) {
			break;
		}

		if (arg->subtype == arg_normal) {
			an[pos_i].type = arg->data.type;
			++pos_i;
		} else if (arg->subtype == arg_kwarg) {
			struct node *key = bucket_arr_get(&f->ast->nodes, arg->l);
			akw[kw_i].key = get_cstr(wk, key->data.str);
			akw[kw_i].type = arg->data.type;
			++kw_i;
		}

		if (!(arg->chflg & node_child_c)) {
			break;
		}
		arg_id = arg->c;
	}

	// Save and restore analyze_function_opts around interp_args to prevent
	// analyzer from halting the function.
	struct analyze_function_opts old_opts = analyze_function_opts;
	analyze_function_opts = (struct analyze_function_opts) { 0 };

	if (!interp_args(wk, args_node, an, NULL, akw)) {
		analyze_function_opts = old_opts;
		return false;
	}

	analyze_function_opts = old_opts;

	arg_id = f->args_id;
	pos_i = kw_i = 0;
	while (true) {
		struct node *arg = bucket_arr_get(&f->ast->nodes, arg_id);
		if (arg->type == node_empty) {
			break;
		}

		if (arg->subtype == arg_normal) {
			obj_array_push(wk, args, an[pos_i].val);
			++pos_i;
		} else if (arg->subtype == arg_kwarg) {
			obj_array_push(wk, args, akw[kw_i].val);
			++kw_i;
		}

		if (!(arg->chflg & node_child_c)) {
			break;
		}
		arg_id = arg->c;
	}

	return func_obj_call(wk, f, args, res);
}
#endif

struct func_impl_group func_impl_groups[obj_type_count][language_mode_count] = {
	[0]                        = { { impl_tbl_kernel },               { impl_tbl_kernel_internal },
				       { impl_tbl_kernel_opts }                                            },
	[obj_meson]                = { { impl_tbl_meson },                { impl_tbl_meson_internal }      },
	[obj_subproject]           = { { impl_tbl_subproject },           { 0 }                            },
	[obj_number]               = { { impl_tbl_number },               { impl_tbl_number, }             },
	[obj_dependency]           = { { impl_tbl_dependency },           { 0 }                            },
	[obj_machine]              = { { impl_tbl_machine },              { impl_tbl_machine }             },
	[obj_compiler]             = { { impl_tbl_compiler },             { 0 }                            },
	[obj_feature_opt]          = { { impl_tbl_feature_opt },          { 0 }                            },
	[obj_run_result]           = { { impl_tbl_run_result },           { impl_tbl_run_result }          },
	[obj_string]               = { { impl_tbl_string },               { impl_tbl_string }              },
	[obj_dict]                 = { { impl_tbl_dict },                 { impl_tbl_dict_internal }       },
	[obj_external_program]     = { { impl_tbl_external_program },     { impl_tbl_external_program }    },
	[obj_python_installation]  = { { impl_tbl_python_installation },  { impl_tbl_python_installation } },
	[obj_configuration_data]   = { { impl_tbl_configuration_data },   { impl_tbl_configuration_data }  },
	[obj_custom_target]        = { { impl_tbl_custom_target },        { 0 }                            },
	[obj_file]                 = { { impl_tbl_file },                 { impl_tbl_file }                },
	[obj_bool]                 = { { impl_tbl_boolean },              { impl_tbl_boolean }             },
	[obj_array]                = { { impl_tbl_array },                { impl_tbl_array_internal }      },
	[obj_build_target]         = { { impl_tbl_build_target },         { 0 }                            },
	[obj_environment]          = { { impl_tbl_environment },          { impl_tbl_environment }         },
	[obj_disabler]             = { { impl_tbl_disabler },             { impl_tbl_disabler }            },
	[obj_generator]            = { { impl_tbl_generator },            { 0 }                            },
	[obj_both_libs]            = { { impl_tbl_both_libs },            { 0 }                            },
	[obj_source_set]           = { { impl_tbl_source_set },           { 0 }                            },
	[obj_source_configuration] = { { impl_tbl_source_configuration }, { 0 }                            },
	[obj_module]               = { { impl_tbl_module },               { 0 }                            },
};

struct func_impl native_funcs[512];

static void
copy_func_impl_group(struct func_impl_group *group, uint32_t *off)
{
	if (!group->impls) {
		return;
	}

	group->off = *off;
	for (group->len = 0; group->impls[group->len].name; ++group->len) {
		assert(group->off + group->len < ARRAY_LEN(native_funcs) && "bump native_funcs size");
		native_funcs[group->off + group->len] = group->impls[group->len];
	}
	*off += group->len;
}

void
build_func_impl_tables(void)
{
	uint32_t off = 0;
	enum module m;
	enum obj_type t;
	enum language_mode lang_mode;

	both_libs_build_impl_tbl();
	python_build_impl_tbl();

	for (t = 0; t < obj_type_count; ++t) {
		for (lang_mode = 0; lang_mode < language_mode_count; ++lang_mode) {
			copy_func_impl_group(&func_impl_groups[t][lang_mode], &off);
		}
	}

	for (m = 0; m < module_count; ++m) {
		for (lang_mode = 0; lang_mode < language_mode_count; ++lang_mode) {
			copy_func_impl_group(&module_func_impl_groups[m][lang_mode], &off);
		}
	}
}

static bool
func_lookup_for_mode(const struct func_impl_group *impl_group, const char *name, uint32_t *idx)
{
	if (!impl_group->impls) {
		return false;
	}

	uint32_t i;
	for (i = 0; impl_group->impls[i].name; ++i) {
		if (strcmp(impl_group->impls[i].name, name) == 0) {
			*idx = impl_group->off + i;
			return true;
		}
	}

	return true;
}

bool
func_lookup_for_group(const struct func_impl_group impl_group[], enum language_mode mode, const char *name, uint32_t *idx)
{
	if (mode == language_extended) {
		if (func_lookup_for_mode(&impl_group[language_internal], name, idx)) {
			return true;
		}

		return func_lookup_for_mode(&impl_group[language_external], name, idx);
	} else {
		return func_lookup_for_mode(&impl_group[mode], name, idx);
	}

	return false;
}

const char *
func_name_str(enum obj_type t, const char *name)
{
	static char buf[256];
	if (t) {
		snprintf(buf, ARRAY_LEN(buf), "method %s.%s()", obj_type_to_s(t), name);
	} else {
		snprintf(buf, ARRAY_LEN(buf), "function %s()", name);
	}

	return buf;
}

bool
func_lookup(struct workspace *wk, obj rcvr, const char *name, uint32_t *idx, obj *func)
{
	enum obj_type t;
	struct func_impl_group *impl_group;
	struct obj_module *m;

	t = get_obj_type(wk, rcvr);
	if (t == obj_module) {
		m = get_obj_module(wk, rcvr);

		if (!m->found && strcmp(name, "found") != 0) {
			vm_error(wk, "module %s was not found", module_names[m->module]);
			return false;
		}

		if (m->exports) {
			if (!obj_dict_index_str(wk, m->exports, name, func)) {
				vm_error(wk, "%s not found in module", name);
				return false;
			}
			return true;
		}

		if (!module_func_lookup(wk, name, m->module, idx)) {
			if (!m->has_impl) {
				vm_error(wk, "module '%s' is unimplemented,\n"
					"  If you would like to make your build files portable to muon, use"
					" `import('%s', required: false)`, and then check"
					" the .found() method before use."
					, module_names[m->module]
					, module_names[m->module]
					);
				return false;
			} else {
				vm_error(wk, "%s not found in module %s", func_name_str(0, name), module_names[m->module]);
				return false;
			}
		}
		return true;
	}

	impl_group = func_impl_groups[t];

	if (!func_lookup_for_group(impl_group, wk->vm.lang_mode, name, idx)) {
		vm_error(wk, "%s not found", func_name_str(t, name));
		return false;
	}

	/* if (fi && fi->fuzz_unsafe && disable_fuzz_unsafe_functions) { */
	/* 	vm_error_at(wk, name_node, "%s is disabled", func_name_str(have_rcvr, rcvr_type, name)); */
	/* 	return false; */
	/* } */

	/* if (have_rcvr && fi && fi->rcvr_transform) { */
	/* 	rcvr_id = fi->rcvr_transform(wk, rcvr_id); */
	/* } */

	/* TracyCZoneC(tctx_func, 0xff5000, true); */
/* #ifdef TRACY_ENABLE */
	/* const char *func_name = func_name_str(have_rcvr, rcvr_type, name); */
	/* TracyCZoneName(tctx_func, func_name, strlen(func_name)); */
/* #endif */

	/* bool func_res; */

	/* if (fi) { */
	/* 	func_res = fi->func(wk, rcvr_id, args_node, res); */
	/* } else { */
	/* 	func_res = func_obj_eval(wk, func_obj, func_module, args_node, res); */
	/* } */

	/* TracyCZoneEnd(tctx_func); */

	/* if (!func_res) { */
	/* 	if (disabler_among_args) { */
	/* 		*res = disabler_id; */
	/* 		disabler_among_args = false; */
	/* 		return true; */
	/* 	} else { */
	/* 		vm_error_at(wk, name_node, "in %s", func_name_str(have_rcvr, rcvr_type, name)); */
	/* 		return false; */
	/* 	} */
	/* } */
	return true;
}

bool
analyze_function(struct workspace *wk, const struct func_impl *fi, uint32_t args_node, obj rcvr, obj *res, bool *was_pure)
{
#if 0
	struct analyze_function_opts old_opts = analyze_function_opts;
	*res = 0;

	bool pure = fi->pure;

	struct obj_tainted_by_typeinfo_ctx tainted_ctx = { .allow_tainted_dict_values = true };
	if (rcvr && obj_tainted_by_typeinfo(wk, rcvr, &tainted_ctx)) {
		pure = false;
	}

	if (!rcvr) {
		if (strcmp(fi->name, "set_variable") == 0 || strcmp(fi->name, "subdir") == 0) {
			analyze_function_opts.allow_impure_args_except_first = true;
		} else if (strcmp(fi->name, "p") == 0) {
			analyze_function_opts.allow_impure_args = true;
		}
	}

	analyze_function_opts.do_analyze = true;
	// pure_function can be set to false even if it was true in the case
	// that any of its arguments are of type obj_typeinfo
	analyze_function_opts.pure_function = pure;
	analyze_function_opts.encountered_error = true;

	bool func_ret = fi->func(wk, rcvr, args_node, res);

	pure = analyze_function_opts.pure_function;
	bool ok = !analyze_function_opts.encountered_error;

	analyze_function_opts = old_opts;

	*was_pure = pure;

	if (pure) {
		return func_ret;
	} else {
		return ok;
	}
#endif
	return false;
}

/* static int32_t */
/* function_sig_sort(const void *a, const void *b, void *_ctx) */
/* { */
/* 	const struct function_signature *sa = a, *sb = b; */

/* 	if ((sa->is_method && sb->is_method) || (!sa->is_method && !sb->is_method)) { */
/* 		return strcmp(sa->name, sb->name); */
/* 	} else if (sa->is_method) { */
/* 		return 1; */
/* 	} else { */
/* 		return -1; */
/* 	} */
/* } */

void
dump_function_signatures(struct workspace *wk)
{
#if 0
	analyze_function_opts.dump_signature = true;

	arr_init(&function_sig_dump.sigs, 64, sizeof(struct function_signature));
	struct function_signature *sig, empty = { 0 };

	uint32_t i;
	for (i = 0; kernel_func_tbl[wk->lang_mode][i].name; ++i) {
		sig = arr_get(&function_sig_dump.sigs, arr_push(&function_sig_dump.sigs, &empty));
		sig->impl = &kernel_func_tbl[wk->lang_mode][i];
		sig->name = kernel_func_tbl[wk->lang_mode][i].name;
		sig->returns = typechecking_type_to_s(wk, kernel_func_tbl[wk->lang_mode][i].return_type);
		kernel_func_tbl[wk->lang_mode][i].func(wk, 0, 0, 0);
	}

	{
		enum obj_type t;
		for (t = 0; t < obj_type_count; ++t) {
			if (!func_tbl[t][wk->lang_mode]) {
				continue;
			}

			for (i = 0; func_tbl[t][wk->lang_mode][i].name; ++i) {
				sig = arr_get(&function_sig_dump.sigs, arr_push(&function_sig_dump.sigs, &empty));
				sig->impl = &func_tbl[t][wk->lang_mode][i];
				sig->is_method = true;
				sig->name = get_cstr(wk, make_strf(wk, "%s.%s", obj_type_to_s(t), func_tbl[t][wk->lang_mode][i].name));
				sig->returns = typechecking_type_to_s(wk, func_tbl[t][wk->lang_mode][i].return_type);
				func_tbl[t][wk->lang_mode][i].func(wk, 0, 0, 0);
			}
		}
	}

	for (i = 0; i < module_count; ++i) {
		if (!module_func_tbl[i][wk->lang_mode]) {
			continue;
		}

		uint32_t j;
		for (j = 0; module_func_tbl[i][wk->lang_mode][j].name; ++j) {
			sig = arr_get(&function_sig_dump.sigs, arr_push(&function_sig_dump.sigs, &empty));
			sig->impl = &module_func_tbl[i][wk->lang_mode][j];
			sig->is_method = true;
			sig->name = get_cstr(wk, make_strf(wk, "import('%s').%s", module_names[i], module_func_tbl[i][wk->lang_mode][j].name));
			sig->returns = typechecking_type_to_s(wk, module_func_tbl[i][wk->lang_mode][j].return_type);
			module_func_tbl[i][wk->lang_mode][j].func(wk, 0, 0, 0);
		}
	}


	arr_sort(&function_sig_dump.sigs, NULL, function_sig_sort);

	for (i = 0; i < function_sig_dump.sigs.len; ++i) {
		sig = arr_get(&function_sig_dump.sigs, i);

		if (sig->impl->extension) {
			printf("extension:");
		}

		printf("%s\n", sig->name);
		if (sig->posargs) {
			printf("  posargs:\n%s", sig->posargs);
		}
		if (sig->varargs) {
			printf("  varargs:\n%s", sig->varargs);
		}
		if (sig->optargs) {
			printf("  optargs:\n%s", sig->optargs);
		}
		if (sig->kwargs) {
			printf("  kwargs:\n%s", sig->kwargs);
		}
		printf("  returns:\n    %s\n", sig->returns);
	}

	arr_destroy(&function_sig_dump.sigs);
#endif
}

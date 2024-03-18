/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"
#include "coerce.h"

#include <string.h>

#include "external/tinyjson.h"
#include "functions/external_program.h"
#include "functions/modules/python.h"
#include "lang/typecheck.h"
#include "platform/filesystem.h"
#include "platform/run_cmd.h"

static const char *introspect_version =
	"import sysconfig\n"
	"print(sysconfig.get_python_version())\n";

static const char *introspect_paths =
	"import sysconfig\n"
	"import json\n"
	"print(json.dumps(sysconfig.get_paths()))\n";

static const char *introspect_vars =
	"import sysconfig\n"
	"import json\n"
	"print(json.dumps(sysconfig.get_config_vars()))\n";

static bool
query_version(struct workspace *wk, const char *path,
	struct obj_python_installation *python)
{
	struct run_cmd_ctx cmd_ctx = { 0 };
	char *const args[] = { (char *)path, "-c", (char *)introspect_version, 0 };
	if (!run_cmd_argv(&cmd_ctx, args, NULL, 0) || cmd_ctx.status != 0) {
		return false;
	}

	size_t index = 0, buf_index = 0, max_buf_index = cmd_ctx.out.len;
	char *buf = cmd_ctx.out.buf, *entry = cmd_ctx.out.buf;
	while (buf_index != max_buf_index) {
		if (buf[buf_index] == '\n') {
			buf[buf_index] = '\0';

			if (index == 0) {  /* language_version */
				python->language_version = make_str(wk, entry);
			}

			entry = &buf[buf_index + 1];
			index++;
		} else {
			buf_index++;
		}
	}

	bool success = buf[buf_index] == '\0';

	run_cmd_ctx_destroy(&cmd_ctx);
	return success;
}

static bool
query_paths(struct workspace *wk, const char *path,
	struct obj_python_installation *python)
{
	struct run_cmd_ctx cmd_ctx = { 0 };
	char *const path_args[] = { (char *)path, "-c", (char *)introspect_paths, 0 };
	if (!run_cmd_argv(&cmd_ctx, path_args, NULL, 0) || cmd_ctx.status != 0) {
		return false;
	}

	bool success = muon_json_to_dict(wk, cmd_ctx.out.buf, &python->sysconfig_paths);

	run_cmd_ctx_destroy(&cmd_ctx);
	return success;
}

static bool
query_vars(struct workspace *wk, const char *path,
	struct obj_python_installation *python)
{
	struct run_cmd_ctx cmd_ctx = { 0 };
	char *const var_args[] = { (char *)path, "-c", (char *)introspect_vars, 0 };
	if (!run_cmd_argv(&cmd_ctx, var_args, NULL, 0) || cmd_ctx.status != 0) {
		return false;
	}

	bool success = muon_json_to_dict(wk, cmd_ctx.out.buf, &python->sysconfig_vars);

	run_cmd_ctx_destroy(&cmd_ctx);
	return success;
}

static bool
introspect_python_interpreter(struct workspace *wk, const char *path,
	struct obj_python_installation *python)
{
	return query_version(wk, path, python) && query_paths(wk, path, python)
	       && query_vars(wk, path, python);
}

static bool
python_module_present(struct workspace *wk, const char *pythonpath, const char *mod)
{
	struct run_cmd_ctx cmd_ctx = { 0 };

	SBUF(importstr);
	sbuf_pushf(wk, &importstr, "import %s", mod);

	char *const *args = (char *const []){
		(char *)pythonpath,
		"-c",
		importstr.buf,
		0
	};

	bool present = run_cmd_argv(&cmd_ctx, args, NULL, 0) && cmd_ctx.status == 0;

	run_cmd_ctx_destroy(&cmd_ctx);

	return present;
}

struct iter_mod_ctx {
	const char *pythonpath;
	uint32_t node;
	enum requirement_type requirement;
};

static enum iteration_result
iterate_required_module_list(struct workspace *wk, void *ctx, obj val)
{
	struct iter_mod_ctx *_ctx = ctx;
	const char *mod = get_cstr(wk, val);

	if (python_module_present(wk, _ctx->pythonpath, mod)) {
		return ir_cont;
	}

	if (_ctx->requirement == requirement_required) {
		vm_error_at(wk, _ctx->node, "python: required module '%s' not found", mod);
	}

	return ir_err;
}

static bool
func_module_python_find_installation(struct workspace *wk,
	obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm ao[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_required,
		kw_disabler,
		kw_modules,
	};
	struct args_kw akw[] = {
		[kw_required] = { "required", tc_required_kw },
		[kw_disabler] = { "disabler", obj_bool },
		[kw_modules] = { "modules", TYPE_TAG_LISTIFY | obj_string },
		0
	};
	if (!interp_args(wk, args_node, NULL, ao, akw)) {
		return false;
	}

	enum requirement_type requirement;
	if (!coerce_requirement(wk, &akw[kw_required], &requirement)) {
		return false;
	}

	bool disabler = akw[kw_disabler].set && get_obj_bool(wk, akw[kw_disabler].val);

	const char *cmd = "python3";
	if (ao[0].set) {
		cmd = get_cstr(wk, ao[0].val);
	}

	SBUF(cmd_path);
	bool found = fs_find_cmd(wk, &cmd_path, cmd);
	if (!found && (requirement == requirement_required)) {
		vm_error_at(wk, args_node, "%s not found", cmd);
		return false;
	}

	if (!found && disabler) {
		*res = disabler_id;
		return true;
	}

	if (akw[kw_modules].set && found) {
		bool all_present = obj_array_foreach(wk,
			akw[kw_modules].val,
			&(struct iter_mod_ctx){
			.pythonpath = cmd_path.buf,
			.node = akw[kw_modules].node,
			.requirement = requirement,
		},
			iterate_required_module_list
			);

		if (!all_present) {
			if (requirement == requirement_required) {
				return false;
			}
			if (disabler) {
				*res = disabler_id;
				return true;
			}
			/* Return a not-found object. */
			found = false;
		}
	}

	make_obj(wk, res, obj_python_installation);
	struct obj_python_installation *python = get_obj_python_installation(wk, *res);
	make_obj(wk, &python->prog, obj_external_program);
	struct obj_external_program *ep = get_obj_external_program(wk, python->prog);
	ep->found = found;
	make_obj(wk, &ep->cmd_array, obj_array);
	obj_array_push(wk, ep->cmd_array, sbuf_into_str(wk, &cmd_path));

	if (!introspect_python_interpreter(wk, cmd_path.buf, python)) {
		vm_error_at(wk, args_node, "failed to introspect python");
		return false;
	}

	return true;
}

static bool
func_python_installation_language_version(struct workspace *wk, obj rcvr,
	uint32_t args_node, obj *res)
{
	if (!interp_args(wk, args_node, NULL, NULL, NULL)) {
		return false;
	}

	*res = get_obj_python_installation(wk, rcvr)->language_version;
	return true;
}

static bool
func_module_python3_find_python(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	struct args_norm ao[] = { { obj_string }, ARG_TYPE_NULL };
	if (!interp_args(wk, args_node, NULL, ao, NULL)) {
		return false;
	}

	const char *cmd = "python3";
	if (ao[0].set) {
		cmd = get_cstr(wk, ao[0].val);
	}

	SBUF(cmd_path);
	if (!fs_find_cmd(wk, &cmd_path, cmd)) {
		vm_error_at(wk, args_node, "python3 not found");
		return false;
	}

	make_obj(wk, res, obj_external_program);
	struct obj_external_program *ep = get_obj_external_program(wk, *res);
	ep->found = true;
	make_obj(wk, &ep->cmd_array, obj_array);
	obj_array_push(wk, ep->cmd_array, sbuf_into_str(wk, &cmd_path));

	return true;
}

static bool
func_python_installation_get_path(struct workspace *wk, obj rcvr,
	uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { obj_string }, ARG_TYPE_NULL };
	if (!interp_args(wk, args_node, an, ao, NULL)) {
		return false;
	}

	obj path = an[0].val;
	obj sysconfig_paths = get_obj_python_installation(wk, rcvr)->sysconfig_paths;
	if (obj_dict_index(wk, sysconfig_paths, path, res)) {
		return true;
	}

	if (!ao[0].set) {
		vm_error_at(wk, args_node,
			"path '%o' not found, no default specified", path);
		return false;
	}

	*res = ao[0].val;
	return true;
}

static bool
func_python_installation_get_var(struct workspace *wk, obj rcvr,
	uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	struct args_norm ao[] = { { obj_string }, ARG_TYPE_NULL };
	if (!interp_args(wk, args_node, an, ao, NULL)) {
		return false;
	}

	obj var = an[0].val;
	obj sysconfig_vars = get_obj_python_installation(wk, rcvr)->sysconfig_vars;
	if (obj_dict_index(wk, sysconfig_vars, var, res)) {
		return true;
	}

	if (!ao[0].set) {
		vm_error_at(wk, args_node,
			"variable '%o' not found, no default specified", var);
		return false;
	}

	*res = ao[0].val;
	return true;
}

static bool
func_python_installation_has_path(struct workspace *wk, obj rcvr,
	uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}

	obj sysconfig_paths = get_obj_python_installation(wk, rcvr)->sysconfig_paths;
	bool found = obj_dict_in(wk, sysconfig_paths, an[0].val);
	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, found);

	return true;
}

static bool
func_python_installation_has_var(struct workspace *wk, obj rcvr,
	uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	if (!interp_args(wk, args_node, an, NULL, NULL)) {
		return false;
	}

	obj sysconfig_vars = get_obj_python_installation(wk, rcvr)->sysconfig_vars;
	bool found = obj_dict_in(wk, sysconfig_vars, an[0].val);
	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, found);

	return true;
}

static obj
python_rcvr_transform(struct workspace *wk, obj rcvr)
{
	return get_obj_python_installation(wk, rcvr)->prog;
}

void
python_build_impl_tbl(void)
{
	uint32_t i;
	for (i = 0; impl_tbl_external_program[i].name; ++i) {
		struct func_impl tmp = impl_tbl_external_program[i];
		tmp.rcvr_transform = python_rcvr_transform;
		impl_tbl_python_installation[i] = tmp;
	}
}

const struct func_impl impl_tbl_module_python[] = {
	{ "find_installation", func_module_python_find_installation, tc_python_installation },
	{ NULL, NULL },
};

const struct func_impl impl_tbl_module_python3[] = {
	{ "find_python", func_module_python3_find_python, tc_external_program },
	{ NULL, NULL },
};

struct func_impl impl_tbl_python_installation[] = {
	[ARRAY_LEN(impl_tbl_external_program) - 1] =
	{ "get_path", func_python_installation_get_path, tc_string },
	{ "get_variable", func_python_installation_get_var, tc_string },
	{ "has_path", func_python_installation_has_path, tc_bool },
	{ "has_variable", func_python_installation_has_var, tc_bool },
	{ "language_version", func_python_installation_language_version, tc_string },
	{ NULL, NULL },
};

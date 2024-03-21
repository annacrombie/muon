/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "args.h"
#include "functions/common.h"
#include "functions/external_program.h"
#include "guess.h"
#include "lang/typecheck.h"
#include "log.h"
#include "platform/run_cmd.h"

void
find_program_guess_version(struct workspace *wk, obj cmd_array, obj *ver)
{
	*ver = 0;
	struct run_cmd_ctx cmd_ctx = { 0 };
	obj args;
	obj_array_dup(wk, cmd_array, &args);
	obj_array_push(wk, args, make_str(wk, "--version"));

	const char *argstr;
	uint32_t argc;
	join_args_argstr(wk, &argstr, &argc, args);

	if (run_cmd(&cmd_ctx, argstr, argc, NULL, 0) && cmd_ctx.status == 0) {
		guess_version(wk, cmd_ctx.out.buf, ver);
	}

	run_cmd_ctx_destroy(&cmd_ctx);
}


static bool
func_external_program_found(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, get_obj_external_program(wk, rcvr)->found);
	return true;
}

static bool
func_external_program_path(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	struct obj_external_program *ep = get_obj_external_program(wk, rcvr);
	if (get_obj_array(wk, ep->cmd_array)->len > 1) {
		vm_error_at(wk, args_node, "cannot return the full_path() of an external program with multiple elements (have: %o)\n", ep->cmd_array);
		return false;
	}

	obj_array_index(wk, get_obj_external_program(wk, rcvr)->cmd_array, 0, res);
	return true;
}

static bool
func_external_program_version(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	struct obj_external_program *prog = get_obj_external_program(wk, rcvr);
	if (!prog->guessed_ver) {
		find_program_guess_version(wk, prog->cmd_array, &prog->ver);
		prog->guessed_ver = true;
	}

	*res = prog->ver;
	return true;
}

const struct func_impl impl_tbl_external_program[] = {
	{ "found", func_external_program_found, tc_bool },
	{ "path", func_external_program_path, tc_string },
	{ "full_path", func_external_program_path, tc_string },
	{ "version", func_external_program_version, tc_string },
	{ NULL, NULL },
};

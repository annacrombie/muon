/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "functions/common.h"
#include "functions/run_result.h"
#include "lang/typecheck.h"
#include "log.h"

static bool
ensure_valid_run_result(struct workspace *wk, obj self)
{
	struct obj_run_result *rr = get_obj_run_result(wk, self);

	if ((rr->flags & run_result_flag_from_compile) && !(rr->flags & run_result_flag_compile_ok)) {
		vm_error(wk, "this run_result was not run because its source could not be compiled");
		return false;
	}

	return true;
}

static bool
func_run_result_returncode(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	if (!ensure_valid_run_result(wk, self)) {
		return false;
	}

	make_obj(wk, res, obj_number);
	set_obj_number(wk, *res, get_obj_run_result(wk, self)->status);
	return true;
}

static bool
func_run_result_stdout(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	if (!ensure_valid_run_result(wk, self)) {
		return false;
	}

	*res = get_obj_run_result(wk, self)->out;
	return true;
}

static bool
func_run_result_stderr(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	if (!ensure_valid_run_result(wk, self)) {
		return false;
	}

	*res = get_obj_run_result(wk, self)->err;
	return true;
}

static bool
func_run_result_compiled(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	struct obj_run_result *rr = get_obj_run_result(wk, self);

	if (!(rr->flags & run_result_flag_from_compile)) {
		vm_error(wk, "this run_result is not from a compiler.run() call");
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, rr->flags & run_result_flag_compile_ok);
	return true;
}

const struct func_impl impl_tbl_run_result[] = {
	{ "compiled", func_run_result_compiled, tc_bool },
	{ "returncode", func_run_result_returncode, tc_number },
	{ "stderr", func_run_result_stderr, tc_string },
	{ "stdout", func_run_result_stdout, tc_string },
	{ NULL, NULL },
};

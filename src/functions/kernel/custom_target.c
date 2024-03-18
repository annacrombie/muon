/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: dffdff2423 <dffdff2423@gmail.com>
 * SPDX-FileCopyrightText: Eli Schwartz <eschwartz@archlinux.org>
 * SPDX-FileCopyrightText: illiliti <illiliti@thunix.net>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "coerce.h"
#include "error.h"
#include "functions/generator.h"
#include "functions/kernel/custom_target.h"
#include "functions/string.h"
#include "install.h"
#include "lang/typecheck.h"
#include "log.h"
#include "platform/filesystem.h"
#include "platform/path.h"

struct custom_target_cmd_fmt_ctx {
	struct process_custom_target_commandline_opts *opts;
	uint32_t i;
	obj *res;
	bool skip_depends;
};

static bool
prefix_plus_index(const struct str *ss, const char *prefix, int64_t *index)
{
	uint32_t len = strlen(prefix);
	if (str_startswith(ss, &WKSTR(prefix))) {
		return str_to_i(&(struct str) {
			.s = &ss->s[len],
			.len = ss->len - len
		}, index, false);
	}

	return false;
}

static void
str_relative_to_build_root(struct workspace *wk, struct custom_target_cmd_fmt_ctx *ctx, const char *path_orig, obj *res)
{
	SBUF(rel);
	const char *path = path_orig;

	if (!ctx->opts->relativize) {
		*res = make_str(wk, path);
		return;
	}

	if (!path_is_absolute(path)) {
		*res = make_str(wk, path);
		return;
	}

	path_relative_to(wk, &rel, wk->build_root, path);

	if (ctx->i == 0) {
		// prefix relative argv0 with ./ so that executables are looked
		// up properly if they reside in the build root.  Without this,
		// an executable in the build root will be called without any
		// path elements, and will be assumed to be on PATH, which
		// either results in the wrong executable being run, or a
		// command not found error.
		SBUF(exe);
		path_executable(wk, &exe, rel.buf);
		*res = sbuf_into_str(wk, &exe);
	} else {
		*res = sbuf_into_str(wk, &rel);
	}
}

static enum format_cb_result
format_cmd_arg_cb(struct workspace *wk, uint32_t node, void *_ctx, const struct str *strkey, obj *elem)
{
	struct custom_target_cmd_fmt_ctx *ctx = _ctx;

	enum cmd_arg_fmt_key {
		key_input,
		key_output,
		key_outdir,
		key_depfile,
		key_plainname,
		key_basename,
		key_private_dir,
		key_source_root,
		key_build_root,
		key_build_dir,
		key_current_source_dir,
		cmd_arg_fmt_key_count,
	};

	const struct {
		char *key;
		bool valid;
		bool needs_name;
	} key_names[cmd_arg_fmt_key_count] = {
		[key_input             ] = { "INPUT", ctx->opts->input },
		[key_output            ] = { "OUTPUT", ctx->opts->output },
		[key_outdir            ] = { "OUTDIR", ctx->opts->output },
		[key_depfile           ] = { "DEPFILE", ctx->opts->depfile },
		[key_plainname         ] = { "PLAINNAME", ctx->opts->input },
		[key_basename          ] = { "BASENAME", ctx->opts->input },
		[key_private_dir       ] = { "PRIVATE_DIR", ctx->opts->output, true, },
		[key_source_root       ] = { "SOURCE_ROOT", true },
		[key_build_root        ] = { "BUILD_ROOT", true },
		[key_build_dir         ] = { "BUILD_DIR", ctx->opts->build_dir },
		[key_current_source_dir] = { "CURRENT_SOURCE_DIR", true },
	};

	enum cmd_arg_fmt_key key;
	for (key = 0; key < cmd_arg_fmt_key_count; ++key) {
		if (!str_eql(strkey, &WKSTR(key_names[key].key))) {
			continue;
		}

		if (!key_names[key].valid
		    || (key_names[key].needs_name && !ctx->opts->name)) {
			return format_cb_not_found;
		}

		break;
	}

	obj e;

	switch (key) {
	case key_input:
	case key_output: {
		obj arr = key == key_input ? ctx->opts->input : ctx->opts->output;

		int64_t index = 0;
		if (!boundscheck(wk, ctx->opts->err_node, get_obj_array(wk, arr)->len, &index)) {
			return format_cb_error;
		}
		obj_array_index(wk, arr, 0, &e);

		str_relative_to_build_root(wk, ctx, get_file_path(wk, e), elem);
		return format_cb_found;
	}
	case key_outdir:
		/* @OUTDIR@: the full path to the directory where the output(s)
		 * must be written */
		str_relative_to_build_root(wk, ctx, get_cstr(wk, current_project(wk)->build_dir), elem);
		return format_cb_found;
	case key_current_source_dir:
		/* @CURRENT_SOURCE_DIR@: this is the directory where the
		 * currently processed meson.build is located in. Depending on
		 * the backend, this may be an absolute or a relative to
		 * current workdir path. */
		str_relative_to_build_root(wk, ctx, get_cstr(wk, current_project(wk)->cwd), elem);
		return format_cb_found;
	case key_private_dir: {
		/* @PRIVATE_DIR@ (since 0.50.1): path to a directory where the
		 * custom target must store all its intermediate files. */
		SBUF(path);
		path_join(wk, &path, get_cstr(wk, current_project(wk)->build_dir), get_cstr(wk, ctx->opts->name));
		sbuf_pushs(wk, &path, ".p");

		str_relative_to_build_root(wk, ctx, path.buf, elem);
		return format_cb_found;
	}
	case key_depfile:
		/* @DEPFILE@: the full path to the dependency file passed to
		 * depfile */
		str_relative_to_build_root(wk, ctx, get_file_path(wk, ctx->opts->depfile), elem);
		return format_cb_found;
	case key_source_root:
		/* @SOURCE_ROOT@: the path to the root of the source tree.
		 * Depending on the backend, this may be an absolute or a
		 * relative to current workdir path. */
		str_relative_to_build_root(wk, ctx, wk->source_root, elem);
		return format_cb_found;
	case key_build_root:
		/* @BUILD_ROOT@: the path to the root of the build tree.
		 * Depending on the backend, this may be an absolute or a
		 * relative to current workdir path. */
		str_relative_to_build_root(wk, ctx, wk->build_root, elem);
		return format_cb_found;
	case key_build_dir:
		// only for generators
		str_relative_to_build_root(wk, ctx, ctx->opts->build_dir, elem);
		return format_cb_found;
	case key_plainname:
	/* @PLAINNAME@: the input filename, without a path */
	case key_basename: {
		/* @BASENAME@: the input filename, with extension removed */
		struct obj_array *in = get_obj_array(wk, ctx->opts->input);
		if (in->len != 1) {
			vm_error_at(wk, ctx->opts->err_node,
				"to use @PLAINNAME@ and @BASENAME@ in a custom "
				"target command, there must be exactly one input");
			return format_cb_error;
		}

		obj in0;
		obj_array_index(wk, ctx->opts->input, 0, &in0);
		const struct str *orig_str = get_str(wk, *get_obj_file(wk, in0));

		SBUF(plainname);
		path_basename(wk, &plainname, orig_str->s);

		if (key == key_basename) {
			SBUF(basename);
			path_without_ext(wk, &basename, plainname.buf);

			str_relative_to_build_root(wk, ctx, basename.buf, elem);
		} else {
			str_relative_to_build_root(wk, ctx, plainname.buf, elem);
		}
		return format_cb_found;
	}
	default:
		break;
	}

	int64_t index;
	obj arr;

	if (prefix_plus_index(strkey, "INPUT", &index)) {
		arr = ctx->opts->input;
	} else if (prefix_plus_index(strkey, "OUTPUT", &index)) {
		arr = ctx->opts->output;
	} else {
		if (ctx->opts->err_node) {
			vm_warning_at(wk, ctx->opts->err_node, "not substituting unknown key '%.*s' in commandline", strkey->len, strkey->s);
		}
		return format_cb_skip;
	}

	if (!boundscheck(wk, ctx->opts->err_node, get_obj_array(wk, arr)->len, &index)) {
		return format_cb_error;
	}

	obj_array_index(wk, arr, index, &e);

	str_relative_to_build_root(wk, ctx, get_file_path(wk, e), elem);
	return format_cb_found;
}

static enum iteration_result
custom_target_cmd_fmt_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct custom_target_cmd_fmt_ctx *ctx = _ctx;

	obj ss;
	enum obj_type t = get_obj_type(wk, val);

	switch (t) {
	case obj_both_libs:
	case obj_build_target:
	case obj_external_program:
	case obj_python_installation:
	case obj_file: {
		obj str, args;
		if (!coerce_executable(wk, ctx->opts->err_node, val, &str, &args)) {
			return ir_err;
		}

		str_relative_to_build_root(wk, ctx, get_cstr(wk, str), &ss);

		if (!ctx->skip_depends) {
			obj_array_push(wk, ctx->opts->depends, ss);
		}

		if (args) {
			obj_array_push(wk, *ctx->res, ss);
			obj_array_extend_nodup(wk, *ctx->res, args);
			return ir_cont;
		}

		break;
	}
	case obj_string: {
		if (ctx->opts->input && str_eql(get_str(wk, val), &WKSTR("@INPUT@"))) {
			ctx->skip_depends = true;
			if (!obj_array_foreach(wk, ctx->opts->input, ctx, custom_target_cmd_fmt_iter)) {
				return ir_err;
			}
			ctx->skip_depends = false;
			return ir_cont;
		} else if (ctx->opts->output && str_eql(get_str(wk, val), &WKSTR("@OUTPUT@"))) {
			ctx->skip_depends = true;
			if (!obj_array_foreach(wk, ctx->opts->output, ctx, custom_target_cmd_fmt_iter)) {
				return ir_err;
			}
			ctx->skip_depends = false;
			goto cont;
		} else if (ctx->opts->extra_args_valid && str_eql(get_str(wk, val), &WKSTR("@EXTRA_ARGS@"))) {
			if (ctx->opts->extra_args) {
				obj_array_extend(wk, *ctx->res, ctx->opts->extra_args);
				ctx->opts->extra_args_used = true;
			}
			goto cont;
		}

		obj s;
		if (!string_format(wk, ctx->opts->err_node, val, &s, ctx, format_cmd_arg_cb)) {
			return ir_err;
		}
		ss = s;
		break;
	}
	case obj_custom_target: {
		obj output = get_obj_custom_target(wk, val)->output;

		if (!obj_array_foreach(wk, output, ctx, custom_target_cmd_fmt_iter)) {
			return ir_err;
		}
		goto cont;
	}
	case obj_compiler: {
		obj cmd_array = get_obj_compiler(wk, val)->cmd_arr;

		if (!obj_array_foreach(wk, cmd_array, ctx, custom_target_cmd_fmt_iter)) {
			return ir_err;
		}
		goto cont;
	}
	default:
		vm_error_at(wk, ctx->opts->err_node, "unable to coerce %o to string", val);
		return ir_err;
	}

	assert(get_obj_type(wk, ss) == obj_string);

	obj_array_push(wk, *ctx->res, ss);
cont:
	++ctx->i;
	return ir_cont;
}

bool
process_custom_target_commandline(struct workspace *wk,
	struct process_custom_target_commandline_opts *opts,
	obj arr, obj *res)
{
	make_obj(wk, res, obj_array);

	struct custom_target_cmd_fmt_ctx ctx = {
		.opts = opts,
		.res = res,
	};

	if (!obj_array_foreach_flat(wk, arr, &ctx, custom_target_cmd_fmt_iter)) {
		return false;
	}

	if (!get_obj_array(wk, *res)->len) {
		vm_error_at(wk, opts->err_node, "cmd cannot be empty");
		return false;
	}
	return true;
}

static enum format_cb_result
format_cmd_output_cb(struct workspace *wk, uint32_t node, void *_ctx, const struct str *strkey, obj *elem)
{
	struct custom_target_cmd_fmt_ctx *ctx = _ctx;

	enum cmd_output_fmt_key {
		key_plainname,
		key_basename,
		cmd_output_fmt_key_count
	};

	const char *key_names[cmd_output_fmt_key_count] = {
		[key_plainname] = "PLAINNAME",
		[key_basename] = "BASENAME",
	};

	enum cmd_output_fmt_key key;
	for (key = 0; key < cmd_output_fmt_key_count; ++key) {
		if (str_eql(strkey, &WKSTR(key_names[key]))) {
			break;
		}
	}

	if (key >= cmd_output_fmt_key_count) {
		return format_cb_not_found;
	}

	struct obj_array *in = get_obj_array(wk, ctx->opts->input);
	if (in->len != 1) {
		vm_error_at(wk, ctx->opts->err_node,
			"to use @PLAINNAME@ and @BASENAME@ in a custom "
			"target output, there must be exactly one input");
		return format_cb_error;
	}

	obj in0;
	obj_array_index(wk, ctx->opts->input, 0, &in0);
	const struct str *ss = get_str(wk, *get_obj_file(wk, in0));
	SBUF(buf);

	switch (key) {
	case key_plainname:
		path_basename(wk, &buf, ss->s);
		break;
	case key_basename: {
		SBUF(basename);
		path_basename(wk, &basename, ss->s);
		path_without_ext(wk, &buf, basename.buf);
		break;
	}
	default:
		assert(false && "unreachable");
		return format_cb_error;
	}

	*elem = sbuf_into_str(wk, &buf);
	return format_cb_found;
}

static enum iteration_result
custom_command_output_format_iter(struct workspace *wk, void *_ctx, obj v)
{
	struct custom_target_cmd_fmt_ctx *ctx = _ctx;

	obj file = *get_obj_file(wk, v);

	obj s;
	if (!string_format(wk, ctx->opts->err_node, file, &s, ctx, format_cmd_output_cb)) {
		return ir_err;
	}

	obj f;
	make_obj(wk, &f, obj_file);
	*get_obj_file(wk, f) = s;

	obj_array_push(wk, ctx->opts->output, f);

	return ir_cont;
}

struct process_custom_tgt_sources_ctx {
	uint32_t err_node;
	obj tgt_id;
	obj res;
};

static enum iteration_result
process_custom_tgt_sources_iter(struct workspace *wk, void *_ctx, obj val)
{
	obj res;
	struct process_custom_tgt_sources_ctx *ctx = _ctx;

	switch (get_obj_type(wk, val)) {
	case obj_generated_list:
		if (!generated_list_process_for_target(wk, ctx->err_node, val, ctx->tgt_id, true, &res)) {
			return ir_err;
		}
		break;
	default: {
		if (!coerce_files(wk, ctx->err_node, val, &res)) {
			return ir_err;
		}
		break;
	}
	}

	obj_array_extend_nodup(wk, ctx->res, res);
	return ir_cont;
}

bool
make_custom_target(struct workspace *wk,
	struct make_custom_target_opts *opts, obj *res)
{
	obj input, raw_output, output, args;

	make_obj(wk, res, obj_custom_target);
	struct obj_custom_target *tgt = get_obj_custom_target(wk, *res);
	tgt->name = opts->name;

	// A custom_target won't have a name if it is from a generator
	if (opts->name) { /* private path */
		SBUF(path);
		path_join(wk, &path, get_cstr(wk, current_project(wk)->build_dir), get_cstr(wk, opts->name));
		sbuf_pushs(wk, &path, ".p");
		tgt->private_path = sbuf_into_str(wk, &path);
	}

	if (opts->input_orig) {
		make_obj(wk, &input, obj_array);

		struct process_custom_tgt_sources_ctx ctx = {
			.err_node = opts->input_node,
			.res = input,
			.tgt_id = *res,
		};

		if (get_obj_type(wk, opts->input_orig) != obj_array) {
			obj arr_input;
			make_obj(wk, &arr_input, obj_array);
			obj_array_push(wk, arr_input, opts->input_orig);
			opts->input_orig = arr_input;
		}

		if (!obj_array_foreach_flat(wk, opts->input_orig, &ctx, process_custom_tgt_sources_iter)) {
			return false;
		}
	} else {
		input = 0;
	}

	if (opts->output_orig) {
		if (!coerce_output_files(wk, opts->output_node, opts->output_orig, opts->output_dir, &raw_output)) {
			return false;
		} else if (!get_obj_array(wk, raw_output)->len) {
			vm_error_at(wk, opts->output_node, "output cannot be empty");
			return false;
		}

		make_obj(wk, &output, obj_array);
		struct custom_target_cmd_fmt_ctx ctx = {
			.opts = &(struct process_custom_target_commandline_opts) {
				.err_node = opts->output_node,
				.input = input,
				.output = output,
				.name = opts->name,
			},
		};
		if (!obj_array_foreach(wk, raw_output, &ctx, custom_command_output_format_iter)) {
			return false;
		}
	} else {
		output = 0;
	}

	obj depfile = 0;
	if (opts->depfile_orig) {
		obj raw_depfiles;
		if (!coerce_output_files(wk, 0, opts->depfile_orig, opts->output_dir, &raw_depfiles)) {
			return false;
		}

		if (!obj_array_flatten_one(wk, raw_depfiles, &depfile)) {
			UNREACHABLE;
		}

		struct custom_target_cmd_fmt_ctx ctx = {
			.opts = &(struct process_custom_target_commandline_opts) {
				.input = input,
			},
		};

		obj depfile_formatted;
		if (!string_format(wk, 0, *get_obj_file(wk, depfile), &depfile_formatted, &ctx, format_cmd_output_cb)) {
			return ir_err;
		}

		*get_obj_file(wk, depfile) = depfile_formatted;
	}

	struct process_custom_target_commandline_opts cmdline_opts = {
		.err_node   = opts->command_node,
		.relativize = true,
		.name       = opts->name,
		.input      = input,
		.output     = output,
		.depfile    = depfile,
		.build_dir  = opts->build_dir,
		.extra_args = opts->extra_args,
		.extra_args_valid = opts->extra_args_valid,
	};
	make_obj(wk, &cmdline_opts.depends, obj_array);
	if (!process_custom_target_commandline(wk, &cmdline_opts, opts->command_orig, &args)) {
		return false;
	}

	if (opts->extra_args && !cmdline_opts.extra_args_used) {
		vm_warning_at(wk, opts->command_node, "extra args passed, but no @EXTRA_ARGS@ key found to substitute");
	}

	if (opts->capture) {
		tgt->flags |= custom_target_capture;
	}

	if (opts->feed) {
		tgt->flags |= custom_target_feed;
	}

	tgt->args = args;
	tgt->input = input;
	tgt->output = output;
	tgt->depfile = depfile;
	tgt->depends = cmdline_opts.depends;
	return true;
}

bool
func_custom_target(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm ao[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_input,
		kw_output,
		kw_command,
		kw_capture,
		kw_install,
		kw_install_dir,
		kw_install_mode,
		kw_install_tag,
		kw_build_by_default,
		kw_depfile,
		kw_depend_files,
		kw_depends,
		kw_build_always_stale,
		kw_build_always,
		kw_env,
		kw_feed,
		kw_console,
	};
	struct args_kw akw[] = {
		[kw_input] = { "input", TYPE_TAG_LISTIFY | tc_coercible_files | tc_generated_list, },
		[kw_output] = { "output", TYPE_TAG_LISTIFY | tc_string, .required = true },
		[kw_command] = { "command", tc_command_array | tc_both_libs, .required = true },
		[kw_capture] = { "capture", obj_bool },
		[kw_install] = { "install", obj_bool },
		[kw_install_dir] = { "install_dir", TYPE_TAG_LISTIFY | tc_string | tc_bool },
		[kw_install_mode] = { "install_mode", tc_install_mode_kw },
		[kw_install_tag] = { "install_tag", tc_string }, // TODO
		[kw_build_by_default] = { "build_by_default", obj_bool },
		[kw_depfile] = { "depfile", obj_string },
		[kw_depend_files] = { "depend_files", TYPE_TAG_LISTIFY | tc_string | tc_file },
		[kw_depends] = { "depends", tc_depends_kw },
		[kw_build_always_stale] = { "build_always_stale", obj_bool },
		[kw_build_always] = { "build_always", obj_bool },
		[kw_env] = { "env", tc_coercible_env },
		[kw_feed] = { "feed", obj_bool },
		[kw_console] = { "console", obj_bool },
		0
	};

	if (!interp_args(wk, args_node, NULL, ao, akw)) {
		return false;
	}

	obj name;
	if (ao[0].set) {
		name = ao[0].val;
	} else {
		if (!get_obj_array(wk, akw[kw_output].val)->len) {
			vm_error_at(wk, akw[kw_output].node, "output cannot be empty");
			return false;
		}

		obj v;
		obj_array_index(wk, akw[kw_output].val, 0, &v);
		name = v;
	}

	struct make_custom_target_opts opts = {
		.name         = name,
		.input_node   = akw[kw_input].node,
		.output_node  = akw[kw_output].node,
		.command_node = akw[kw_command].node,
		.input_orig   = akw[kw_input].val,
		.output_orig  = akw[kw_output].val,
		.output_dir   = get_cstr(wk, current_project(wk)->build_dir),
		.command_orig = akw[kw_command].val,
		.depfile_orig = akw[kw_depfile].val,
		.capture      = akw[kw_capture].set && get_obj_bool(wk, akw[kw_capture].val),
		.feed         = akw[kw_feed].set && get_obj_bool(wk, akw[kw_feed].val),
	};

	if (!make_custom_target(wk, &opts, res)) {
		return false;
	}

	struct obj_custom_target *tgt = get_obj_custom_target(wk, *res);

	if (akw[kw_depend_files].set) {
		obj depend_files;
		if (!coerce_files(wk, akw[kw_depend_files].node, akw[kw_depend_files].val, &depend_files)) {
			return false;
		}

		obj_array_extend_nodup(wk, tgt->depends, depend_files);
	}

	if (akw[kw_depends].set) {
		obj depends;
		if (!coerce_files(wk, akw[kw_depends].node, akw[kw_depends].val, &depends)) {
			return false;
		}

		obj_array_extend_nodup(wk, tgt->depends, depends);
	}

	if (akw[kw_build_always_stale].set && get_obj_bool(wk, akw[kw_build_always_stale].val)) {
		tgt->flags |= custom_target_build_always_stale;
	}

	if (akw[kw_build_by_default].set && get_obj_bool(wk, akw[kw_build_by_default].val)) {
		tgt->flags |= custom_target_build_by_default;
	}

	if (akw[kw_build_always].set && get_obj_bool(wk, akw[kw_build_always].val)) {
		tgt->flags |= custom_target_build_always_stale | custom_target_build_by_default;
	}

	if (akw[kw_console].set && get_obj_bool(wk, akw[kw_console].val)) {
		if (opts.capture) {
			vm_error_at(wk, akw[kw_console].node, "console and capture cannot both be set to true");
			return false;
		}

		tgt->flags |= custom_target_console;
	}

	if ((akw[kw_install].set && get_obj_bool(wk, akw[kw_install].val))
	    || (!akw[kw_install].set && akw[kw_install_dir].set)) {
		if (!akw[kw_install_dir].set || !get_obj_array(wk, akw[kw_install_dir].val)->len) {
			vm_error_at(wk, akw[kw_install].node, "custom target installation requires install_dir");
			return false;
		}

		if (!akw[kw_build_by_default].set) {
			tgt->flags |= custom_target_build_by_default;
		}

		obj install_mode_id = 0;
		if (akw[kw_install_mode].set) {
			install_mode_id = akw[kw_install_mode].val;
		}

		obj install_dir = akw[kw_install_dir].val;
		if (get_obj_array(wk, akw[kw_install_dir].val)->len == 1) {
			obj i0;
			obj_array_index(wk, akw[kw_install_dir].val, 0, &i0);
			install_dir = i0;
		}

		if (!push_install_targets(wk, akw[kw_install_dir].node, tgt->output,
			install_dir, install_mode_id, false)) {
			return false;
		}
	}

	if (!coerce_environment_from_kwarg(wk, &akw[kw_env], false, &tgt->env)) {
		return false;
	}

	L("adding custom target '%s'", get_cstr(wk, tgt->name));

	obj_array_push(wk, current_project(wk)->targets, *res);
	return true;
}

bool
func_vcs_tag(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	enum kwargs {
		kw_input,
		kw_output,
		kw_command,
		kw_fallback,
		kw_replace_string,
	};
	struct args_kw akw[] = {
		[kw_input] = { "input", TYPE_TAG_LISTIFY | tc_coercible_files, .required = true },
		[kw_output] = { "output", obj_string, .required = true },
		[kw_command] = { "command", tc_command_array | tc_both_libs },
		[kw_fallback] = { "fallback", obj_string },
		[kw_replace_string] = { "replace_string", obj_string },
		0
	};

	if (!interp_args(wk, args_node, NULL, NULL, akw)) {
		return false;
	}

	obj replace_string = akw[kw_replace_string].set
		? akw[kw_replace_string].val
		: make_str(wk, "@VCS_TAG@");

	obj fallback;
	if (akw[kw_fallback].set) {
		fallback = akw[kw_fallback].val;
	} else {
		fallback = current_project(wk)->cfg.version;
	}

	obj command;
	make_obj(wk, &command, obj_array);

	push_args_null_terminated(wk, command, (char *const []){
		(char *)wk->argv0,
		"internal",
		"eval",
		"-e",
		"vcs_tagger.meson",
		NULL,
	});

	obj input;
	{
		obj input_arr;
		if (!coerce_files(wk, akw[kw_input].node, akw[kw_input].val, &input_arr)) {
			return false;
		}

		if (!obj_array_flatten_one(wk, input_arr, &input)) {
			vm_error_at(wk, akw[kw_input].node, "expected exactly one input");
			return false;
		}
	}

	obj_array_push(wk, command, input);
	obj_array_push(wk, command, make_str(wk, "@OUTPUT@"));
	obj_array_push(wk, command, replace_string);
	obj_array_push(wk, command, fallback);
	obj_array_push(wk, command, make_str(wk, wk->source_root));

	if (akw[kw_command].set) {
		obj_array_extend(wk, command, akw[kw_command].val);
	}

	struct make_custom_target_opts opts = {
		.name         = make_str(wk, "vcs_tag"),
		.input_node   = akw[kw_input].node,
		.output_node  = akw[kw_output].node,
		.input_orig   = akw[kw_input].val,
		.output_orig  = akw[kw_output].val,
		.output_dir   = get_cstr(wk, current_project(wk)->build_dir),
		.command_orig = command,
	};

	if (!make_custom_target(wk, &opts, res)) {
		return false;
	}

	struct obj_custom_target *tgt = get_obj_custom_target(wk, *res);
	tgt->flags |= custom_target_build_always_stale;

	obj_array_push(wk, current_project(wk)->targets, *res);
	return true;
}

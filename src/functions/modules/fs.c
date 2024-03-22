/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: Eli Schwartz <eschwartz@archlinux.org>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "args.h"
#include "functions/common.h"
#include "functions/kernel/custom_target.h"
#include "functions/modules/fs.h"
#include "lang/typecheck.h"
#include "log.h"
#include "platform/filesystem.h"
#include "platform/path.h"
#include "sha_256.h"

enum fix_file_path_opts {
	fix_file_path_allow_file = 1 << 0,
	fix_file_path_expanduser = 1 << 1,
	fix_file_path_noabs = 1 << 2,
};

static bool
fix_file_path(struct workspace *wk, uint32_t err_node, obj path, enum fix_file_path_opts opts, struct sbuf *buf)
{
	enum obj_type t = get_obj_type(wk, path);
	const struct str *ss;
	switch (t) {
	case obj_string: ss = get_str(wk, path); break;
	case obj_file:
		if (opts & fix_file_path_allow_file) {
			ss = get_str(wk, *get_obj_file(wk, path));
			break;
		}
	// FALLTHROUGH
	default:
		vm_error_at(wk,
			err_node,
			"expected string%s, got %s",
			(opts & fix_file_path_allow_file) ? " or file" : "",
			obj_type_to_s(t));
		return false;
	}

	if (str_has_null(ss)) {
		vm_error_at(wk, err_node, "path cannot contain null bytes");
		return false;
	} else if (!ss->len) {
		vm_error_at(wk, err_node, "path cannot be empty");
		return false;
	}

	if (path_is_absolute(ss->s)) {
		path_copy(wk, buf, ss->s);
	} else {
		if ((opts & fix_file_path_expanduser) && ss->s[0] == '~') {
			const char *home;
			if (!(home = fs_user_home())) {
				vm_error_at(wk, err_node, "failed to get user home directory");
				return false;
			}

			path_join(wk, buf, home, &ss->s[1]);
		} else if (opts & fix_file_path_noabs) {
			path_copy(wk, buf, ss->s);
		} else {
			struct project *proj = current_project(wk);
			path_join(wk, buf, proj ? get_cstr(wk, proj->cwd) : "", ss->s);
		}
	}

	_path_normalize(wk, buf, true);
	return true;
}

typedef bool((*fs_lookup_func)(const char *));

static bool
func_module_fs_lookup_common(struct workspace *wk, obj *res, fs_lookup_func lookup, enum fix_file_path_opts opts)
{
	struct args_norm an[] = { { tc_string | tc_file }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path);
	if (!fix_file_path(wk, an[0].node, an[0].val, opts, &path)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, lookup(path.buf));
	return true;
}

static bool
func_module_fs_exists(struct workspace *wk, obj self, obj *res)
{
	return func_module_fs_lookup_common(wk, res, fs_exists, fix_file_path_expanduser);
}

static bool
func_module_fs_is_file(struct workspace *wk, obj self, obj *res)
{
	return func_module_fs_lookup_common(wk, res, fs_file_exists, fix_file_path_expanduser);
}

static bool
func_module_fs_is_dir(struct workspace *wk, obj self, obj *res)
{
	return func_module_fs_lookup_common(wk, res, fs_dir_exists, fix_file_path_expanduser);
}

static bool
func_module_fs_is_symlink(struct workspace *wk, obj self, obj *res)
{
	return func_module_fs_lookup_common(
		wk, res, fs_symlink_exists, fix_file_path_allow_file | fix_file_path_expanduser);
}

static bool
func_module_fs_parent(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string | tc_file }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path);
	if (!fix_file_path(wk,
		    an[0].node,
		    an[0].val,
		    fix_file_path_allow_file | fix_file_path_expanduser | fix_file_path_noabs,
		    &path)) {
		return false;
	}

	SBUF(buf);
	path_dirname(wk, &buf, path.buf);
	*res = sbuf_into_str(wk, &buf);
	return true;
}

static bool
func_module_fs_read(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string | tc_file }, ARG_TYPE_NULL };
	enum {
		kw_encoding,
	};
	struct args_kw akw[] = {
		[kw_encoding] = { "encoding", obj_string },
		0,
	};
	if (!pop_args(wk, an, akw)) {
		return false;
	}

	if (akw[kw_encoding].set) {
		if (!str_eql(get_str(wk, akw[kw_encoding].val), &WKSTR("utf-8"))) {
			vm_error_at(wk, akw[kw_encoding].node, "only 'utf-8' supported");
		}
	}

	SBUF(path);
	if (!fix_file_path(wk, an[0].node, an[0].val, fix_file_path_allow_file | fix_file_path_expanduser, &path)) {
		return false;
	}

	struct source src = { 0 };
	if (!fs_read_entire_file(path.buf, &src)) {
		return false;
	}

	*res = make_strn(wk, src.src, src.len);

	fs_source_destroy(&src);
	return true;
}

static bool
func_module_fs_is_absolute(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, path_is_absolute(get_cstr(wk, an[0].val)));
	return true;
}

static bool
func_module_fs_expanduser(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path);
	if (!fix_file_path(wk, an[0].node, an[0].val, fix_file_path_expanduser, &path)) {
		return false;
	}

	*res = sbuf_into_str(wk, &path);
	return true;
}

static bool
func_module_fs_name(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string | tc_file }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path);
	if (!fix_file_path(wk, an[0].node, an[0].val, fix_file_path_allow_file, &path)) {
		return false;
	}

	SBUF(basename);
	path_basename(wk, &basename, path.buf);
	*res = sbuf_into_str(wk, &basename);
	return true;
}

static bool
func_module_fs_stem(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string | tc_file }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path);
	if (!fix_file_path(wk, an[0].node, an[0].val, fix_file_path_allow_file, &path)) {
		return false;
	}

	SBUF(basename);
	path_basename(wk, &basename, path.buf);

	char *dot;
	if ((dot = strrchr(basename.buf, '.'))) {
		*dot = 0;
		basename.len = strlen(basename.buf);
	}

	*res = sbuf_into_str(wk, &basename);
	return true;
}

static bool
func_module_as_posix(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	const char *path = get_cstr(wk, an[0].val), *p;

	SBUF(buf);
	for (p = path; *p; ++p) {
		if (*p == '\\') {
			sbuf_push(wk, &buf, '/');
			if (*(p + 1) == '\\') {
				++p;
			}
		} else {
			sbuf_push(wk, &buf, *p);
		}
	}

	*res = sbuf_into_str(wk, &buf);
	return true;
}

static bool
func_module_replace_suffix(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string | tc_file }, { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path);
	if (!fix_file_path(wk, an[0].node, an[0].val, fix_file_path_allow_file | fix_file_path_noabs, &path)) {
		return false;
	}

	char *dot;
	if ((dot = strrchr(path.buf, '.'))) {
		*dot = 0;
		path.len = strlen(path.buf);
	}

	sbuf_pushs(wk, &path, get_cstr(wk, an[1].val));
	*res = sbuf_into_str(wk, &path);
	return true;
}

static bool
func_module_fs_hash(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string | tc_file }, { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	if (!str_eql(get_str(wk, an[1].val), &WKSTR("sha256"))) {
		vm_error_at(wk, an[1].node, "only sha256 is supported");
		return false;
	}

	SBUF(path);
	if (!fix_file_path(wk, an[0].node, an[0].val, fix_file_path_allow_file, &path)) {
		return false;
	}

	struct source src = { 0 };
	if (!fs_read_entire_file(path.buf, &src)) {
		return false;
	}

	uint8_t hash[32] = { 0 };
	calc_sha_256(hash, src.src, src.len); // TODO: other hash algos

	char buf[65] = { 0 };
	uint32_t i, bufi = 0;
	for (i = 0; i < 32; ++i) {
		snprintf(&buf[bufi], 3, "%x", hash[i]);
		bufi += 2;
	}

	*res = make_str(wk, buf);

	fs_source_destroy(&src);
	return true;
}

static bool
func_module_fs_size(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string | tc_file }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path);
	if (!fix_file_path(wk, an[0].node, an[0].val, fix_file_path_allow_file, &path)) {
		return false;
	}

	uint64_t size;
	FILE *f;
	if (!(f = fs_fopen(path.buf, "rb"))) {
		return false;
	} else if (!fs_fsize(f, &size)) {
		return false;
	} else if (!fs_fclose(f)) {
		return false;
	}

	assert(size < INT64_MAX);
	make_obj(wk, res, obj_number);
	set_obj_number(wk, *res, size);
	return true;
}

static bool
func_module_fs_is_samepath(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string | tc_file }, { tc_string | tc_file }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path1);
	if (!fix_file_path(wk, an[0].node, an[0].val, fix_file_path_allow_file, &path1)) {
		return false;
	}

	SBUF(path2);
	if (!fix_file_path(wk, an[1].node, an[1].val, fix_file_path_allow_file, &path2)) {
		return false;
	}

	// TODO: handle symlinks

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, strcmp(path1.buf, path2.buf) == 0);
	return true;
}

static bool
func_module_fs_copyfile(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string | tc_file }, { tc_string, .optional = true }, ARG_TYPE_NULL };
	enum {
		kw_install,
		kw_install_dir,
		kw_install_tag,
		kw_install_mode,
	};
	struct args_kw akw[] = {
		[kw_install] = { "install", obj_bool },
		[kw_install_dir] = { "install_dir", TYPE_TAG_LISTIFY | tc_string | tc_bool },
		[kw_install_tag] = { "install_tag", tc_string }, // TODO
		[kw_install_mode] = { "install_mode", tc_install_mode_kw },
		0,
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	SBUF(path);
	if (!fix_file_path(wk, an[0].node, an[0].val, fix_file_path_allow_file | fix_file_path_expanduser, &path)) {
		return false;
	}

	obj output;
	if (an[1].set) {
		output = an[1].val;
	} else {
		SBUF(dest);
		path_basename(wk, &dest, path.buf);
		output = sbuf_into_str(wk, &dest);
	}

	obj command;
	make_obj(wk, &command, obj_array);
	push_args_null_terminated(wk,
		command,
		(char *const[]){
			(char *)wk->argv0,
			"internal",
			"eval",
			"-e",
			"copyfile.meson",
			"@INPUT@",
			"@OUTPUT@",
			NULL,
		});

	struct make_custom_target_opts opts = {
		.name = make_str(wk, "copyfile"),
		.input_node = an[0].node,
		.output_node = an[1].node,
		.input_orig = an[0].val,
		.output_orig = output,
		.output_dir = get_cstr(wk, current_project(wk)->build_dir),
		.command_orig = command,
	};

	if (!make_custom_target(wk, &opts, res)) {
		return false;
	}

	obj_array_push(wk, current_project(wk)->targets, *res);

	return true;
}

const struct func_impl impl_tbl_module_fs[] = {
	{ "as_posix", func_module_as_posix, tc_string, true },
	{ "copyfile", func_module_fs_copyfile, tc_custom_target },
	{ "exists", func_module_fs_exists, tc_bool },
	{ "expanduser", func_module_fs_expanduser, tc_string },
	{ "hash", func_module_fs_hash, tc_string },
	{ "is_absolute", func_module_fs_is_absolute, tc_bool, true },
	{ "is_dir", func_module_fs_is_dir, tc_bool },
	{ "is_file", func_module_fs_is_file, tc_bool },
	{ "is_samepath", func_module_fs_is_samepath, tc_bool },
	{ "is_symlink", func_module_fs_is_symlink, tc_bool },
	{ "name", func_module_fs_name, tc_string, true },
	{ "parent", func_module_fs_parent, tc_string, true },
	{ "read", func_module_fs_read, tc_string },
	{ "replace_suffix", func_module_replace_suffix, tc_string, true },
	{
		"size",
		func_module_fs_size,
		tc_number,
	},
	{ "stem", func_module_fs_stem, tc_string, true },
	{ NULL, NULL },
};

static bool
func_module_fs_write(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string | tc_file }, { obj_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path);
	if (!fix_file_path(wk, an[0].node, an[0].val, fix_file_path_allow_file | fix_file_path_expanduser, &path)) {
		return false;
	}

	const struct str *ss = get_str(wk, an[1].val);
	if (!fs_write(path.buf, (uint8_t *)ss->s, ss->len)) {
		return false;
	}
	return true;
}

static bool
func_module_fs_copy(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string | tc_file }, { obj_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path);
	if (!fix_file_path(wk, an[0].node, an[0].val, fix_file_path_allow_file | fix_file_path_expanduser, &path)) {
		return false;
	}

	if (!fs_copy_file(path.buf, get_cstr(wk, an[1].val))) {
		return false;
	}
	return true;
}

static bool
func_module_fs_cwd(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	SBUF(cwd);
	path_cwd(wk, &cwd);
	*res = sbuf_into_str(wk, &cwd);
	return true;
}

static bool
func_module_fs_make_absolute(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path);
	path_make_absolute(wk, &path, get_cstr(wk, an[0].val));
	*res = sbuf_into_str(wk, &path);
	return true;
}

static bool
func_module_fs_mkdir(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	return fs_mkdir(get_cstr(wk, an[0].val));
}

static bool
func_module_fs_relative_to(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string }, { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	const char *p1 = get_cstr(wk, an[0].val), *p2 = get_cstr(wk, an[1].val);

	if (!path_is_absolute(p1)) {
		vm_error_at(wk, an[0].node, "base path '%s' is not absolute", p1);
		return false;
	} else if (!path_is_absolute(p2)) {
		vm_error_at(wk, an[1].node, "path '%s' is not absolute", p2);
		return false;
	}

	SBUF(path);
	path_relative_to(wk, &path, p1, p2);
	*res = sbuf_into_str(wk, &path);
	return true;
}

static bool
func_module_fs_is_basename(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, path_is_basename(get_cstr(wk, an[0].val)));
	return true;
}

static bool
func_module_fs_without_ext(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path);
	path_without_ext(wk, &path, get_cstr(wk, an[0].val));
	*res = sbuf_into_str(wk, &path);
	return true;
}

static bool
func_module_fs_is_subpath(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string }, { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool);
	set_obj_bool(wk, *res, path_is_subpath(get_cstr(wk, an[0].val), get_cstr(wk, an[1].val)));
	return true;
}

static bool
func_module_fs_add_suffix(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string }, { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path);
	path_copy(wk, &path, get_cstr(wk, an[0].val));
	sbuf_pushs(wk, &path, get_cstr(wk, an[1].val));
	*res = sbuf_into_str(wk, &path);
	return true;
}

static bool
func_module_fs_executable(struct workspace *wk, obj self, obj *res)
{
	struct args_norm an[] = { { tc_string }, ARG_TYPE_NULL };
	if (!pop_args(wk, an, NULL)) {
		return false;
	}

	SBUF(path);
	path_executable(wk, &path, get_cstr(wk, an[0].val));
	*res = sbuf_into_str(wk, &path);
	return true;
}

const struct func_impl impl_tbl_module_fs_internal[] = {
	{ "as_posix", func_module_as_posix, tc_string, true },
	{
		"copyfile",
		func_module_fs_copyfile,
	},
	{ "exists", func_module_fs_exists, tc_bool },
	{ "expanduser", func_module_fs_expanduser, tc_string },
	{ "hash", func_module_fs_hash, tc_string },
	{ "is_absolute", func_module_fs_is_absolute, tc_bool, true },
	{ "is_dir", func_module_fs_is_dir, tc_bool },
	{ "is_file", func_module_fs_is_file, tc_bool },
	{ "is_samepath", func_module_fs_is_samepath, tc_bool },
	{ "is_symlink", func_module_fs_is_symlink, tc_bool },
	{ "name", func_module_fs_name, tc_string, true },
	{ "parent", func_module_fs_parent, tc_string, true },
	{ "read", func_module_fs_read, tc_string },
	{ "replace_suffix", func_module_replace_suffix, tc_string, true },
	{
		"size",
		func_module_fs_size,
		tc_number,
	},
	{ "stem", func_module_fs_stem, tc_string, true },
	// non-standard muon extensions
	{ "add_suffix", func_module_fs_add_suffix, tc_string, true },
	{ "copy", func_module_fs_copy, .fuzz_unsafe = true },
	{ "cwd", func_module_fs_cwd, tc_string },
	{ "executable", func_module_fs_executable, tc_string, true },
	{ "is_basename", func_module_fs_is_basename, tc_bool, true },
	{ "is_subpath", func_module_fs_is_subpath, tc_bool, true },
	{ "make_absolute", func_module_fs_make_absolute, tc_string },
	{ "mkdir", func_module_fs_mkdir, .fuzz_unsafe = true },
	{ "relative_to", func_module_fs_relative_to, tc_string, true },
	{ "without_ext", func_module_fs_without_ext, tc_string, true },
	{ "write", func_module_fs_write, .fuzz_unsafe = true },
	{ NULL, NULL },
};

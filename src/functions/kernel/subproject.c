/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "functions/kernel/subproject.h"
#include "functions/string.h"
#include "lang/typecheck.h"
#include "log.h"
#include "options.h"
#include "platform/filesystem.h"
#include "platform/path.h"
#include "wrap.h"

static bool
subproject_prepare(struct workspace *wk, struct sbuf *cwd_buf, const char **cwd,
	struct sbuf *build_dir_buf, const char **build_dir, bool required,
	bool *found)
{
	if (!fs_dir_exists(*cwd)) {
		bool wrap_ok = false;

		SBUF(wrap_path);
		SBUF(base_path);
		sbuf_pushf(wk, &wrap_path, "%s.wrap", *cwd);

		if (!fs_file_exists(wrap_path.buf)) {
			goto wrap_done;
		}

		path_dirname(wk, &base_path, *cwd);

		struct wrap wrap = { 0 };
		enum wrap_mode wrap_mode = get_option_wrap_mode(wk);
		if (!wrap_handle(wrap_path.buf, base_path.buf, &wrap, wrap_mode != wrap_mode_nodownload)) {
			goto wrap_cleanup;
		}

		if (wrap.fields[wf_directory]) {
			path_join(wk, cwd_buf, base_path.buf, wrap.fields[wf_directory]);

			path_dirname(wk, &base_path, *build_dir);
			path_join(wk, build_dir_buf, base_path.buf, wrap.fields[wf_directory]);

			*cwd = cwd_buf->buf;
			*build_dir = build_dir_buf->buf;
		}

		wrap_ok = true;
wrap_cleanup:
		wrap_destroy(&wrap);
wrap_done:
		if (!wrap_ok) {
			if (required) {
				LOG_E("project %s not found", *cwd);
				return false;
			} else {
				*found = false;
				return true;
			}
		}
	}

	SBUF(src);
	path_join(wk, &src, *cwd, "meson.build");

	if (!fs_file_exists(src.buf)) {
		if (required) {
			LOG_E("project %s does not contain a meson.build", *cwd);
			return false;
		} else {
			*found = false;
			return true;
		}
	}

	*found = true;
	return true;
}

bool
subproject(struct workspace *wk, obj name, enum requirement_type req, struct args_kw *default_options,
	struct args_kw *versions, obj *res)
{
	// don't re-evaluate the same subproject
	if (obj_dict_index(wk, wk->subprojects, name, res)) {
		return true;
	}

	make_obj(wk, res, obj_subproject);
	if (req == requirement_skip) {
		return true;
	}

	const char *subproj_name = get_cstr(wk, name);
	SBUF(cwd);
	SBUF(build_dir);

	path_join(wk, &cwd, get_cstr(wk, current_project(wk)->source_root), get_cstr(wk, current_project(wk)->subprojects_dir));
	path_push(wk, &cwd, subproj_name);

	path_join(wk, &build_dir, get_cstr(wk, current_project(wk)->build_root), get_cstr(wk, current_project(wk)->subprojects_dir));
	path_push(wk, &build_dir, subproj_name);

	uint32_t subproject_id = 0;
	bool found;

	const char *sp_cwd = cwd.buf, *sp_build_dir = build_dir.buf;
	SBUF(sp_cwd_buf);
	SBUF(sp_build_dir_buf);

	if (!subproject_prepare(wk, &sp_cwd_buf, &sp_cwd, &sp_build_dir_buf,
		&sp_build_dir, req == requirement_required, &found)) {
		return false;
	}

	if (!found) {
		return true;
	}

	if (default_options && default_options->set) {
		if (!parse_and_set_default_options(wk, default_options->node,
			default_options->val, name, true)) {
			return false;
		}
	}

	if (!eval_project(wk, subproj_name, sp_cwd, sp_build_dir, &subproject_id)) {
		goto not_found;
	}

	if (versions && versions->set) {
		struct project *subp = arr_get(&wk->projects, subproject_id);

		bool compare_result;
		if (!version_compare(wk, versions->node, get_str(wk, subp->cfg.version),
			versions->val, &compare_result)) {
			goto not_found;
		}

		if (!compare_result) {
			if (req == requirement_required) {
				vm_error_at(wk, versions->node,
					"subproject version mismatch; wanted %o, got %o",
					versions->val, subp->cfg.version);
				goto not_found;
			}
		}
	}

	make_obj(wk, res, obj_subproject);
	struct obj_subproject *sub = get_obj_subproject(wk, *res);
	sub->id = subproject_id;
	sub->found = true;
	obj_dict_set(wk, wk->subprojects, name, *res);

	if (fs_dir_exists(wk->build_root)) {
		if (!fs_mkdir_p(build_dir.buf)) {
			return false;
		}
	}

	return true;
not_found:
	if (subproject_id) {
		struct project *proj = arr_get(&wk->projects, subproject_id);
		proj->not_ok = true;
	}

	return req != requirement_required;
}

bool
func_subproject(struct workspace *wk, obj _, obj *res)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_default_options,
		kw_required,
		kw_version,

	};
	struct args_kw akw[] = {
		[kw_default_options] = { "default_options", TYPE_TAG_LISTIFY | obj_string },
		[kw_required] = { "required", tc_required_kw },
		[kw_version] = { "version", TYPE_TAG_LISTIFY | obj_string },
		0
	};

	if (!pop_args(wk, an, akw)) {
		return false;
	}

	enum requirement_type req;
	if (!coerce_requirement(wk, &akw[kw_required], &req)) {
		return false;
	}

	if (!subproject(wk, an[0].val, req, &akw[kw_default_options], &akw[kw_version], res)) {
		return false;
	}

	return true;
}

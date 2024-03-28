/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <string.h>

#include "backend/output.h"
#include "error.h"
#include "lang/interpreter.h"
#include "lang/workspace.h"
#include "log.h"
#include "options.h"
#include "platform/mem.h"
#include "platform/path.h"

struct project *
make_project(struct workspace *wk, uint32_t *id, const char *subproject_name,
	const char *cwd, const char *build_dir)
{
	*id = arr_push(&wk->projects, &(struct project){ 0 });
	struct project *proj = arr_get(&wk->projects, *id);

	proj->scope_stack = wk->scope_stack_dup(wk, wk->default_scope);

	make_obj(wk, &proj->args, obj_dict);
	make_obj(wk, &proj->compilers, obj_dict);
	make_obj(wk, &proj->link_args, obj_dict);
	make_obj(wk, &proj->include_dirs, obj_dict);
	make_obj(wk, &proj->opts, obj_dict);
	make_obj(wk, &proj->summary, obj_dict);
	make_obj(wk, &proj->targets, obj_array);
	make_obj(wk, &proj->tests, obj_array);
	make_obj(wk, &proj->dep_cache.static_deps, obj_dict);
	make_obj(wk, &proj->dep_cache.shared_deps, obj_dict);
	make_obj(wk, &proj->wrap_provides_deps, obj_dict);
	make_obj(wk, &proj->wrap_provides_exes, obj_dict);
	proj->subprojects_dir = make_str(wk, "subprojects");

	if (subproject_name) {
		proj->subproject_name = make_str(wk, subproject_name);
	} else {
		proj->subproject_name = 0;
	}

	proj->cwd = make_str(wk, cwd);
	proj->source_root = proj->cwd;
	proj->build_dir = make_str(wk, build_dir);
	proj->build_root = proj->build_dir;

	return proj;
}

struct project *
current_project(struct workspace *wk)
{
	return arr_get(&wk->projects, wk->cur_project);
}

void
workspace_init_bare(struct workspace *wk)
{
	*wk = (struct workspace){
		.interp_node = interp_node,
		.assign_variable = assign_variable,
		.unassign_variable = unassign_variable,
		.push_local_scope = push_local_scope,
		.pop_local_scope = pop_local_scope,
		.scope_stack_dup = scope_stack_dup,
		.get_variable = get_variable,
		.eval_project_file = eval_project_file,
	};

#ifdef TRACY_ENABLE
	{
		static bool first = true;

		if (first) {
			wk->tracy.is_master_workspace = true;
		}

		first = false;
	}
#endif

	bucket_arr_init(&wk->chrs, 4096, 1);
	bucket_arr_init(&wk->objs, 1024, sizeof(struct obj_internal));
	bucket_arr_init(&wk->dict_elems, 1024, sizeof(struct obj_dict_elem));
	bucket_arr_init(&wk->dict_hashes, 16, sizeof(struct hash));

	const struct {
		uint32_t item_size;
		uint32_t bucket_size;
	} sizes[] = {
		[obj_number] = { sizeof(int64_t), 1024 },
		[obj_string] = { sizeof(struct str), 1024 },
		[obj_compiler] = { sizeof(struct obj_compiler), 4 },
		[obj_array] = { sizeof(struct obj_array), 2048 },
		[obj_dict] = { sizeof(struct obj_dict), 512 },
		[obj_build_target] = { sizeof(struct obj_build_target), 16 },
		[obj_custom_target] = { sizeof(struct obj_custom_target), 16 },
		[obj_subproject] = { sizeof(struct obj_subproject), 16 },
		[obj_dependency] = { sizeof(struct obj_dependency), 16 },
		[obj_external_program] = { sizeof(struct obj_external_program), 32 },
		[obj_python_installation] = { sizeof(struct obj_python_installation), 32 },
		[obj_run_result] = { sizeof(struct obj_run_result), 32 },
		[obj_configuration_data] = { sizeof(struct obj_configuration_data), 16 },
		[obj_test] = { sizeof(struct obj_test), 64 },
		[obj_module] = { sizeof(struct obj_module), 16 },
		[obj_install_target] = { sizeof(struct obj_install_target), 128 },
		[obj_environment] = { sizeof(struct obj_environment), 4 },
		[obj_include_directory] = { sizeof(struct obj_include_directory), 16 },
		[obj_option] = { sizeof(struct obj_option), 32 },
		[obj_generator] = { sizeof(struct obj_generator), 16 },
		[obj_generated_list] = { sizeof(struct obj_generated_list), 16 },
		[obj_alias_target] = { sizeof(struct obj_alias_target), 4 },
		[obj_both_libs] = { sizeof(struct obj_both_libs), 4 },
		[obj_typeinfo] = { sizeof(struct obj_typeinfo), 32 },
		[obj_func] = { sizeof(struct obj_func), 4 },
		[obj_source_set] = { sizeof(struct obj_source_set), 4 },
		[obj_source_configuration] = { sizeof(struct obj_source_configuration), 4 },
	};

	uint32_t i;
	for (i = _obj_aos_start; i < obj_type_count; ++i) {
		bucket_arr_init(&wk->obj_aos[i - _obj_aos_start], sizes[i].bucket_size, sizes[i].item_size);
	}

	obj id;
	make_obj(wk, &id, obj_null);
	assert(id == 0);

	bucket_arr_pushn(&wk->dict_elems, 0, 0, 1); // reserve dict_elem 0 as a null element

	hash_init(&wk->obj_hash, 128, sizeof(obj));
	hash_init_str(&wk->str_hash, 128);
}

void
workspace_init(struct workspace *wk)
{
	workspace_init_bare(wk);

	wk->argv0 = "dummy";
	wk->build_root = "dummy";

	obj id;
	make_obj(wk, &id, obj_disabler);
	assert(id == disabler_id);

	make_obj(wk, &id, obj_bool);
	assert(id == obj_bool_true);
	set_obj_bool(wk, id, true);
	make_obj(wk, &id, obj_bool);
	assert(id == obj_bool_false);
	set_obj_bool(wk, id, false);

	SBUF(source_root);
	path_cwd(wk, &source_root);
	wk->source_root = get_cstr(wk, sbuf_into_str(wk, &source_root));

	arr_init(&wk->projects, 16, sizeof(struct project));
	arr_init(&wk->option_overrides, 32, sizeof(struct option_override));
	bucket_arr_init(&wk->asts, 4, sizeof(struct ast));

	make_obj(wk, &wk->default_scope, obj_array);
	obj scope;
	make_obj(wk, &scope, obj_dict);
	obj_array_push(wk, wk->default_scope, scope);

	make_obj(wk, &id, obj_meson);
	obj_dict_set(wk, scope, make_str(wk, "meson"), id);

	make_obj(wk, &id, obj_machine);
	obj_dict_set(wk, scope, make_str(wk, "host_machine"), id);
	obj_dict_set(wk, scope, make_str(wk, "build_machine"), id);
	obj_dict_set(wk, scope, make_str(wk, "target_machine"), id);

	make_obj(wk, &wk->binaries, obj_dict);
	make_obj(wk, &wk->host_machine, obj_dict);
	make_obj(wk, &wk->regenerate_deps, obj_array);
	make_obj(wk, &wk->install, obj_array);
	make_obj(wk, &wk->install_scripts, obj_array);
	make_obj(wk, &wk->postconf_scripts, obj_array);
	make_obj(wk, &wk->subprojects, obj_dict);
	make_obj(wk, &wk->global_args, obj_dict);
	make_obj(wk, &wk->global_link_args, obj_dict);
	make_obj(wk, &wk->dep_overrides_static, obj_dict);
	make_obj(wk, &wk->dep_overrides_dynamic, obj_dict);
	make_obj(wk, &wk->find_program_overrides, obj_dict);
	make_obj(wk, &wk->global_opts, obj_dict);
	make_obj(wk, &wk->compiler_check_cache, obj_dict);

	if (!init_global_options(wk)) {
		UNREACHABLE;
	}
}

void
workspace_destroy_bare(struct workspace *wk)
{
	uint32_t i;
	struct bucket_arr *ba = &wk->obj_aos[obj_string - _obj_aos_start];
	for (i = 0; i < ba->len; ++i) {
		struct str *s = bucket_arr_get(ba, i);
		if (s->flags & str_flag_big) {
			z_free((void *)s->s);
		}
	}

	for (i = _obj_aos_start; i < obj_type_count; ++i) {
		bucket_arr_destroy(&wk->obj_aos[i - _obj_aos_start]);
	}

	for (i = 0; i < wk->dict_hashes.len; ++i) {
		struct hash *h = bucket_arr_get(&wk->dict_hashes, i);
		hash_destroy(h);
	}

	bucket_arr_destroy(&wk->chrs);
	bucket_arr_destroy(&wk->objs);
	bucket_arr_destroy(&wk->dict_elems);
	bucket_arr_destroy(&wk->dict_hashes);

	hash_destroy(&wk->obj_hash);
	hash_destroy(&wk->str_hash);
}

void
workspace_destroy(struct workspace *wk)
{
	uint32_t i;
	for (i = 0; i < wk->asts.len; ++i) {
		struct ast *ast = bucket_arr_get(&wk->asts, i);
		ast_destroy(ast);
	}

	arr_destroy(&wk->projects);
	arr_destroy(&wk->option_overrides);
	bucket_arr_destroy(&wk->asts);

	workspace_destroy_bare(wk);
}

bool
workspace_setup_paths(struct workspace *wk, const char *build, const char *argv0,
	uint32_t argc, char *const argv[])
{
	SBUF(build_root);
	path_make_absolute(wk, &build_root, build);
	wk->build_root = get_cstr(wk, sbuf_into_str(wk, &build_root));

	if (path_is_basename(argv0)) {
		wk->argv0 = get_cstr(wk, make_str(wk, argv0));
	} else {
		SBUF(argv0_abs);
		path_make_absolute(wk, &argv0_abs, argv0);
		wk->argv0 = get_cstr(wk, sbuf_into_str(wk, &argv0_abs));
	}

	wk->original_commandline.argc = argc;
	wk->original_commandline.argv = argv;

	SBUF(muon_private);
	path_join(wk, &muon_private, wk->build_root, output_path.private_dir);
	wk->muon_private = get_cstr(wk, sbuf_into_str(wk, &muon_private));

	if (!fs_mkdir_p(wk->muon_private)) {
		return false;
	}

	SBUF(path);
	{
		const struct str *gitignore_src = &WKSTR("*\n");
		path_join(wk, &path, wk->build_root, ".gitignore");
		if (!fs_write(path.buf, (const uint8_t *)gitignore_src->s, gitignore_src->len)) {
			return false;
		}
	}

	{
		const struct str *hgignore_src = &WKSTR("syntax: glob\n**/*\n");
		path_join(wk, &path, wk->build_root, ".hgignore");
		if (!fs_write(path.buf, (const uint8_t *)hgignore_src->s, hgignore_src->len)) {
			return false;
		}
	}

	return true;
}

static enum iteration_result
print_summaries_line_iter(struct workspace *wk, void *_ctx, obj k, obj v)
{
	FILE *out = _ctx;

	obj_fprintf(wk, out, "      %#o: %o\n", k, v);

	return ir_cont;
}

static enum iteration_result
print_summaries_section_iter(struct workspace *wk, void *_ctx, obj k, obj v)
{
	FILE *out = _ctx;

	if (get_str(wk, k)->len) {
		obj_fprintf(wk, out, "    %#o\n", k);
	}

	obj_dict_foreach(wk, v, out, print_summaries_line_iter);
	return ir_cont;
}

void
workspace_print_summaries(struct workspace *wk, FILE *out)
{
	bool printed_summary_header = false;
	uint32_t i;
	struct project *proj;
	for (i = 0; i < wk->projects.len; ++i) {
		proj = arr_get(&wk->projects, i);
		if (proj->not_ok) {
			continue;
		}

		struct obj_dict *d = get_obj_dict(wk, proj->summary);
		if (!d->len) {
			continue;
		}

		if (!printed_summary_header) {
			fprintf(out, "summary:\n");
			printed_summary_header = true;
		}

		fprintf(out, "- %s %s\n", get_cstr(wk, proj->cfg.name), get_cstr(wk, proj->cfg.version));
		obj_dict_foreach(wk, proj->summary, out, print_summaries_section_iter);
	}
}

static enum iteration_result
workspace_add_regenerate_deps_iter(struct workspace *wk, void *_ctx, obj v)
{
	SBUF(path);
	const char *s = get_cstr(wk, v);
	if (!path_is_absolute(s)) {
		path_join(wk, &path, get_cstr(wk, current_project(wk)->cwd), s);
		v = sbuf_into_str(wk, &path);
		s = get_cstr(wk, v);
	}

	if (path_is_subpath(wk->build_root, s)) {
		return ir_cont;
	}

	if (!fs_file_exists(s)) {
		return ir_cont;
	}

	obj_array_push(wk, wk->regenerate_deps, v);
	return ir_cont;
}

void
workspace_add_regenerate_deps(struct workspace *wk, obj obj_or_arr)
{
	if (get_obj_type(wk, obj_or_arr) == obj_array) {
		obj_array_foreach(wk, obj_or_arr, NULL,
			workspace_add_regenerate_deps_iter);
	} else {
		workspace_add_regenerate_deps_iter(wk, NULL, obj_or_arr);
	}
}

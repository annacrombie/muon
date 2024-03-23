/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "external/bestline.h"
#include "lang/analyze.h"
#include "lang/compiler.h"
#include "lang/eval.h"
#include "log.h"
#include "options.h"
#include "platform/filesystem.h"
#include "platform/mem.h"
#include "platform/path.h"
#include "tracy.h"
#include "wrap.h"

bool
eval_project(struct workspace *wk,
	const char *subproject_name,
	const char *cwd,
	const char *build_dir,
	uint32_t *proj_id)
{
	SBUF(src);
	path_join(wk, &src, cwd, "meson.build");

	bool ret = false;
	uint32_t parent_project = wk->cur_project;

	make_project(wk, &wk->cur_project, subproject_name, cwd, build_dir);
	*proj_id = wk->cur_project;

	obj parent_eval_trace = wk->vm.dbg_state.eval_trace;

	const char *parent_prefix = log_get_prefix();
	char log_prefix[256] = { 0 };
	if (wk->cur_project > 0) {
		const char *clr = log_clr() ? "\033[35m" : "", *no_clr = log_clr() ? "\033[0m" : "";
		snprintf(log_prefix, 255, "[%s%s%s]", clr, subproject_name, no_clr);
		log_set_prefix(log_prefix);
	}
	if (subproject_name) {
		LOG_I("entering subproject '%s'", subproject_name);
	}

	if (!setup_project_options(wk, cwd)) {
		goto cleanup;
	}

	if (wk->vm.dbg_state.eval_trace) {
		wk->vm.dbg_state.eval_trace_subdir = true;
	}

	if (!wk->vm.behavior.eval_project_file(wk, src.buf, true)) {
		goto cleanup;
	}

	if (wk->cur_project == 0 && !check_invalid_subproject_option(wk)) {
		goto cleanup;
	}

	ret = true;
cleanup:
	wk->vm.dbg_state.eval_trace = parent_eval_trace;
	wk->cur_project = parent_project;

	log_set_prefix(parent_prefix);
	return ret;
}

#if 0
static bool
ensure_project_is_first_statement(struct workspace *wk, struct ast *ast, bool check_only)
{
	uint32_t err_node;
	bool first_statement_is_a_call_to_project = false;
	struct node *n;

	err_node = ast->root;
	n = get_node(ast, ast->root);
	if (!(n->type == node_block && n->chflg & node_child_l)) {
		goto err;
	}

	err_node = n->l;
	n = get_node(ast, n->l);
	if (!(n->type == node_function)) {
		goto err;
	}

	err_node = n->l;
	n = get_node(ast, n->l);
	if (!(n->type == node_id && str_eql(get_str(wk, n->data.str), &WKSTR("project")))) {
		goto err;
	}

	first_statement_is_a_call_to_project = true;
err:
	if (!first_statement_is_a_call_to_project) {
		if (!check_only) {
			vm_error_at(wk, err_node, "first statement is not a call to project()");
		}
		return false;
	}
	return true;
}
#endif

bool
eval(struct workspace *wk, struct source *src, enum eval_mode mode, obj *res)
{
	TracyCZoneAutoS;

	arr_push(&wk->vm.src, src);

	uint32_t entry;
	if (!compile(wk, src, mode, &entry)) {
		return false;
	}

	arr_push(&wk->vm.call_stack,
		&(struct call_frame){
			.type = call_frame_type_eval,
			.return_ip = wk->vm.ip,
		});

	wk->vm.ip = entry;

	vm_execute(wk);

	TracyCZoneAutoE;

	return !wk->vm.error;

	/* if (wk->in_analyzer) { */
	/* 	ast->src_id = error_diagnostic_store_push_src(src); */
	/* 	if (wk->vm.dbg_state.eval_trace) { */
	/* 		obj_array_push(wk, wk->vm.dbg_state.eval_trace, make_str(wk, src->label)); */
	/* 		if ((wk->vm.dbg_state.eval_trace_subdir)) { */
	/* 			obj subdir_eval_trace; */
	/* 			make_obj(wk, &subdir_eval_trace, obj_array); */
	/* 			obj_array_push(wk, wk->vm.dbg_state.eval_trace, subdir_eval_trace); */
	/* 			wk->vm.dbg_state.eval_trace = subdir_eval_trace; */
	/* 			wk->vm.dbg_state.eval_trace_subdir = false; */
	/* 		} */
	/* 	} */
	/* } */

	/* enum parse_mode parse_mode = 0; */
	/* if (mode == eval_mode_repl) { */
	/* 	parse_mode |= pm_ignore_statement_with_no_effect; */
	/* } */
	/* if (wk->lang_mode == language_internal || wk->lang_mode == language_extended) { */
	/* 	parse_mode |= pm_functions; */
	/* } */

	/* if (!parser_parse(wk, ast, src, parse_mode)) { */
	/* 	goto ret; */
	/* } */

	/* struct source *old_src = wk->src; */
	/* struct ast *old_ast = wk->ast; */

	/* wk->src = src; */
	/* wk->ast = ast; */

	/* if (mode == eval_mode_first) { */
	/* 	if (!ensure_project_is_first_statement(wk, ast, false)) { */
	/* 		goto ret; */
	/* 	} */
	/* } */

	/* ret = wk->interp_node(wk, wk->ast->root, res); */

	/* if (wk->subdir_done) { */
	/* 	wk->subdir_done = false; */
	/* } */

	/* wk->src = old_src; */
	/* wk->ast = old_ast; */
	/* ret: */
	/* TracyCZoneAutoE; */
	/* return ret; */
}

bool
eval_str(struct workspace *wk, const char *str, enum eval_mode mode, obj *res)
{
	struct source src = { .label = "<internal>", .src = str, .len = strlen(str) };
	return eval(wk, &src, mode, res);
}

bool
eval_project_file(struct workspace *wk, const char *path, bool first)
{
	/* L("evaluating '%s'", path); */
	bool ret = false;
	workspace_add_regenerate_deps(wk, make_str(wk, path));

	struct source src = { 0 };
	if (!fs_read_entire_file(path, &src)) {
		return false;
	}

	obj res;
	if (!eval(wk, &src, first ? eval_mode_first : eval_mode_default, &res)) {
		goto ret;
	}

	ret = true;
ret:
	fs_source_destroy(&src);
	return ret;
}

static bool
repl_eval_str(struct workspace *wk, const char *str, obj *repl_res)
{
	bool ret, o_break_on_err = wk->vm.dbg_state.break_on_err;
	wk->vm.dbg_state.break_on_err = false;
	ret = eval_str(wk, str, eval_mode_repl, repl_res);
	wk->vm.dbg_state.break_on_err = o_break_on_err;
	return ret;
}

void
repl(struct workspace *wk, bool dbg)
{
	bool loop = true;
	obj repl_res = 0;
	char *line;
	FILE *out = stderr;
	enum repl_cmd {
		repl_cmd_noop,
		repl_cmd_exit,
		repl_cmd_abort,
		repl_cmd_step,
		repl_cmd_list,
		repl_cmd_inspect,
		repl_cmd_watch,
		repl_cmd_unwatch,
		repl_cmd_help,
	};
	enum repl_cmd cmd = repl_cmd_noop;
	struct {
		const char *name[3];
		enum repl_cmd cmd;
		bool valid, has_arg;
		const char *help_text;
	} repl_cmds[] = { { { "abort", 0 }, repl_cmd_abort, dbg },
		{ { "c", "continue", 0 }, repl_cmd_exit, dbg },
		{ { "exit", 0 }, repl_cmd_exit, !dbg },
		{ { "h", "help", 0 }, repl_cmd_help, true },
		{ { "i", "inspect", 0 }, repl_cmd_inspect, dbg, true, .help_text = "\\inspect <expr>" },
		{ { "l", "list", 0 }, repl_cmd_list, dbg },
		{ { "s", "step", 0 }, repl_cmd_step, dbg },
		{ { "w", "watch", 0 }, repl_cmd_watch, dbg, true },
		{ { "uw", "unwatch", 0 }, repl_cmd_unwatch, dbg, true },
		0 };

	if (dbg) {
		/* list_line_range(wk->src, get_node(wk->ast, wk->vm.dbg_state.node)->location.line, 1, 0); */

		if (wk->vm.dbg_state.stepping) {
			cmd = repl_cmd_step;
		}
	}

	const char *prompt = "> ", cmd_char = '\\';

	while (loop && (line = muon_bestline(prompt))) {
		muon_bestline_history_add(line);

		if (!*line || *line == cmd_char) {
			char *arg = NULL;

			if (!*line || !line[1]) {
				goto cmd_found;
			}

			if ((arg = strchr(line, ' '))) {
				*arg = 0;
				++arg;
			}

			uint32_t i, j;
			for (i = 0; *repl_cmds[i].name; ++i) {
				if (repl_cmds[i].valid) {
					for (j = 0; repl_cmds[i].name[j]; ++j) {
						if (strcmp(&line[1], repl_cmds[i].name[j]) == 0) {
							if (repl_cmds[i].has_arg) {
								if (!arg) {
									fprintf(out, "missing argument\n");
									goto cont;
								}
							} else {
								if (arg) {
									fprintf(out,
										"this command does not take an argument\n");
									goto cont;
								}
							}

							cmd = repl_cmds[i].cmd;
							goto cmd_found;
						}
					}
				}
			}

			fprintf(out, "unknown repl command '%s'\n", &line[1]);
			goto cont;
cmd_found:
			switch (cmd) {
			case repl_cmd_abort: exit(1); break;
			case repl_cmd_exit:
				wk->vm.dbg_state.stepping = false;
				loop = false;
				break;
			case repl_cmd_help:
				fprintf(out, "repl commands:\n");
				for (i = 0; *repl_cmds[i].name; ++i) {
					if (!repl_cmds[i].valid) {
						continue;
					}

					fprintf(out, "  - ");
					for (j = 0; repl_cmds[i].name[j]; ++j) {
						fprintf(out, "%c%s", cmd_char, repl_cmds[i].name[j]);
						if (repl_cmds[i].name[j + 1]) {
							fprintf(out, ", ");
						}
					}

					if (repl_cmds[i].help_text) {
						fprintf(out, " - %s", repl_cmds[i].help_text);
					}
					fprintf(out, "\n");
				}
				break;
			case repl_cmd_list: {
				/* list_line_range(wk->src, get_node(wk->ast, wk->vm.dbg_state.node)->location.line, 11, 0); */
				break;
			}
			case repl_cmd_step:
				wk->vm.dbg_state.stepping = true;
				loop = false;
				break;
			case repl_cmd_inspect:
				if (!repl_eval_str(wk, arg, &repl_res)) {
					break;
				}

				obj_inspect(wk, out, repl_res);
				break;
			case repl_cmd_watch:
				if (!wk->vm.dbg_state.watched) {
					make_obj(wk, &wk->vm.dbg_state.watched, obj_array);
				}

				obj_array_push(wk, wk->vm.dbg_state.watched, make_str(wk, arg));
				break;
			case repl_cmd_unwatch:
				if (wk->vm.dbg_state.watched) {
					uint32_t idx;
					if (obj_array_index_of(wk, wk->vm.dbg_state.watched, make_str(wk, arg), &idx)) {
						obj_array_del(wk, wk->vm.dbg_state.watched, idx);
					}
				}
				break;
			case repl_cmd_noop: break;
			}
		} else {
			cmd = repl_cmd_noop;

			if (!repl_eval_str(wk, line, &repl_res)) {
				goto cont;
			}

			if (repl_res) {
				obj_fprintf(wk, out, "%o\n", repl_res);
				wk->vm.behavior.assign_variable(wk, "_", repl_res, 0, assign_local);
			}
		}
cont:
		muon_bestline_free(line);
	}

	muon_bestline_history_free();

	if (!line) {
		wk->vm.dbg_state.stepping = false;
	}
}

const char *
determine_project_root(struct workspace *wk, const char *path)
{
#if 0
	SBUF(tmp);
	SBUF(new_path);

	path_make_absolute(wk, &new_path, path);
	path = new_path.buf;

	while (true) {
		if (!fs_file_exists(path)) {
			goto cont;
		}

		struct ast ast = { 0 };
		struct source src = { 0 };

		if (!fs_read_entire_file(path, &src)) {
			return NULL;
		} else if (!parser_parse(wk, &ast, &src, pm_quiet)) {
			return NULL;
		}

		wk->src = &src;
		wk->ast = &ast;
		if (ensure_project_is_first_statement(wk, &ast, true)) {
			// found
			path_dirname(wk, &tmp, path);
			obj s = sbuf_into_str(wk, &tmp);
			return get_cstr(wk, s);
		}

cont:
		path_dirname(wk, &tmp, path);
		path_dirname(wk, &new_path, tmp.buf);
		if (strcmp(new_path.buf, tmp.buf) == 0) {
			return NULL;
		}

		path_push(wk, &new_path, "meson.build");
		path = new_path.buf;
	}
#endif
	return 0;
}

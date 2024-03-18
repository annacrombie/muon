/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_LANG_WORKSPACE_H
#define MUON_LANG_WORKSPACE_H

#include "buf_size.h"
#include "datastructures/arr.h"
#include "datastructures/bucket_arr.h"
#include "datastructures/hash.h"
#include "lang/eval.h"
#include "lang/object.h"
#include "lang/source.h"
#include "lang/string.h"
#include "lang/vm.h"

struct project {
	/* array of dicts */
	obj scope_stack;

	obj source_root, build_root, cwd, build_dir, subproject_name;
	obj opts, compilers, targets, tests, test_setups, summary;
	obj args, link_args, include_dirs;
	struct { obj static_deps, shared_deps; } dep_cache;
	obj wrap_provides_deps, wrap_provides_exes;

	// string
	obj rule_prefix;
	obj subprojects_dir;

	struct {
		obj name;
		obj version;
		obj license;
		obj license_files;
		bool no_version;
	} cfg;

	bool not_ok; // set by failed subprojects
	bool initialized;
};

struct workspace {
	const char *argv0, *source_root, *build_root, *muon_private;

	struct {
		uint32_t argc;
		char *const *argv;
	} original_commandline;

	/* Global objects
	 * These should probably be cleaned up into a separate struct.
	 * ----------------- */
	/* obj_array that tracks files for build regeneration */
	obj regenerate_deps;
	/* TODO host machine dict */
	obj host_machine;
	/* TODO binaries dict */
	obj binaries;
	obj install;
	obj install_scripts;
	obj postconf_scripts;
	obj subprojects;
	/* args dict for add_global_arguments() */
	obj global_args;
	/* args dict for add_global_link_arguments() */
	obj global_link_args;
	/* overridden dependencies dict */
	obj dep_overrides_static, dep_overrides_dynamic;
	/* overridden find_program dict */
	obj find_program_overrides;
	/* global options */
	obj global_opts;
	/* dict[sha_512 -> [bool, any]] */
	obj compiler_check_cache;
	/* ----------------- */

	struct vm vm;

	struct arr projects;
	struct arr option_overrides;

	uint32_t cur_project;

#ifdef TRACY_ENABLE
	struct {
		bool is_master_workspace;
	} tracy;
#endif
};

void workspace_init_bare(struct workspace *wk);
void workspace_init(struct workspace *wk);
void workspace_destroy_bare(struct workspace *wk);
void workspace_destroy(struct workspace *wk);
bool workspace_setup_paths(struct workspace *wk, const char *build, const char *argv0,
	uint32_t argc, char *const argv[]);
void workspace_add_regenerate_deps(struct workspace *wk, obj obj_or_arr);

struct project *make_project(struct workspace *wk, uint32_t *id, const char *subproject_name,
	const char *cwd, const char *build_dir);
struct project *current_project(struct workspace *wk);

void workspace_print_summaries(struct workspace *wk, FILE *out);
#endif

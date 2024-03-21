/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_OPTIONS_H
#define MUON_OPTIONS_H
#include "lang/workspace.h"

extern bool initializing_builtin_options;

struct option_override {
	// strings
	obj proj, name, val;
	enum option_value_source source;
	bool obj_value;
};

bool create_option(struct workspace *wk, obj opts, obj opt, obj val);
bool get_option(struct workspace *wk, const struct project *proj, const struct str *name, obj *res);
bool get_option_overridable(struct workspace *wk,
	const struct project *proj,
	obj overrides,
	const struct str *name,
	obj *res);
void get_option_value(struct workspace *wk, const struct project *proj, const char *name, obj *res);
void get_option_value_overridable(struct workspace *wk,
	const struct project *proj,
	obj overrides,
	const char *name,
	obj *res);

bool check_invalid_option_overrides(struct workspace *wk);
bool check_invalid_subproject_option(struct workspace *wk);
bool prefix_dir_opts(struct workspace *wk);

bool setup_project_options(struct workspace *wk, const char *cwd);
bool init_global_options(struct workspace *wk);

bool parse_and_set_cmdline_option(struct workspace *wk, char *lhs);
bool
parse_and_set_default_options(struct workspace *wk, uint32_t err_node, obj arr, obj project_name, bool for_subproject);
bool parse_and_set_override_options(struct workspace *wk, uint32_t err_node, obj arr, obj *res);

enum wrap_mode {
	wrap_mode_nopromote,
	wrap_mode_nodownload,
	wrap_mode_nofallback,
	wrap_mode_forcefallback,
};
enum wrap_mode get_option_wrap_mode(struct workspace *wk);
enum tgt_type get_option_default_library(struct workspace *wk);
bool get_option_bool(struct workspace *wk, obj overrides, const char *name, bool fallback);

struct list_options_opts {
	bool list_all, only_modified;
};
bool list_options(const struct list_options_opts *list_opts);
#endif

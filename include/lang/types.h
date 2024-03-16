/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_LANG_TYPES_H
#define MUON_LANG_TYPES_H
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t obj;
typedef uint64_t type_tag;

struct args_norm {
	type_tag type;
	const char *name;
	obj val;
	uint32_t node;
	bool set;
};

struct args_kw {
	const char *key;
	type_tag type;
	obj val;
	uint32_t node;
	bool set;
	bool required;
};

enum language_mode {
	language_external,
	language_internal,
	language_opts,
	language_mode_count,

	language_extended,
};

enum log_level {
	log_quiet,
	log_error,
	log_warn,
	log_info,
	log_debug,
	log_level_count,
};
#endif
